//该程序使用ffmpeg与cpp实现将视频A.mp4(第一个文件)强制转为60帧，并将视频A.mp4的每一秒的第一帧替换为视频B.mp4(第二个文件)对应第n秒的第一帧
//该程序使用deepseek制作，作者只提供维护
//该程序是B站你看你大坝进度条的cpp实现

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
}

#include <map>
#include <string>
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <fstream>

// 错误处理宏
#define CHECK_ERROR(ret, msg) \
    if (ret < 0) { \
        char errbuf[AV_ERROR_MAX_STRING_SIZE]; \
        av_make_error_string(errbuf, sizeof(errbuf), ret); \
        std::cerr << "[错误] " << msg << ": " << errbuf << " (在 " << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        return ret; \
    }

#define CHECK_NULL(ptr, msg) \
    if (!ptr) { \
        std::cerr << "[错误] " << msg << ": 内存分配失败 (在 " << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        return AVERROR(ENOMEM); \
    }

// 视频质量分析器
class VideoQualityAnalyzer {
private:
    double total_psnr = 0.0;
    double total_ssim = 0.0;
    int frame_count = 0;
    std::ofstream logfile;

public:
    VideoQualityAnalyzer() {
        logfile.open("video_quality.log");
        logfile << "帧号\tPSNR\tSSIM\n";
    }

    ~VideoQualityAnalyzer() {
        if (logfile.is_open()) logfile.close();
    }

    void analyze(AVFrame* original, AVFrame* processed) {
        if (!original || !processed) return;

        // 计算PSNR
        uint64_t mse = 0;
        for (int y = 0; y < original->height; y++) {
            for (int x = 0; x < original->width; x++) {
                int diff = original->data[0][y * original->linesize[0] + x] -
                          processed->data[0][y * processed->linesize[0] + x];
                mse += diff * diff;
            }
        }
        double psnr = (mse == 0) ? 100.0 : 10 * log10((255.0 * 255.0) / (mse / (double)(original->width * original->height)));

        // 计算SSIM (简化版)
        double ssim = 0.0;
        // 实际应用中应实现完整SSIM算法

        total_psnr += psnr;
        total_ssim += ssim;
        frame_count++;

        logfile << frame_count << "\t" << psnr << "\t" << ssim << "\n";
    }

    double getAveragePSNR() const {
        return frame_count ? total_psnr / frame_count : 0.0;
    }

    double getAverageSSIM() const {
        return frame_count ? total_ssim / frame_count : 0.0;
    }
};

// 清理资源的辅助函数
struct FFmpegResources {
    AVFormatContext* fmtCtx = nullptr;
    AVCodecContext* codecCtx = nullptr;
    AVFilterGraph* filterGraph = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* swsCtx = nullptr;

    ~FFmpegResources() {
        if (codecCtx) avcodec_free_context(&codecCtx);
        if (fmtCtx) avformat_close_input(&fmtCtx);
        if (filterGraph) avfilter_graph_free(&filterGraph);
        if (frame) av_frame_free(&frame);
        if (swsCtx) sws_freeContext(swsCtx);
    }
};

// 动态码率控制器
class DynamicBitrateController {
private:
    int base_bitrate;
    int current_bitrate;
    int max_bitrate;
    int min_bitrate;
    int frame_counter = 0;
    double current_crf = 18.0;
    const double max_crf = 23.0;
    const double min_crf = 16.0;

public:
    DynamicBitrateController(int width, int height, int fps) {
        // 根据分辨率计算基础码率
        int pixels = width * height;

        if (pixels >= 3840 * 2160) { // 4K
            base_bitrate = 50000000; // 50 Mbps
        } else if (pixels >= 1920 * 1080) { // 1080p
            base_bitrate = 15000000; // 15 Mbps
        } else if (pixels >= 1280 * 720) { // 720p
            base_bitrate = 7500000; // 7.5 Mbps
        } else { // SD
            base_bitrate = 3000000; // 3 Mbps
        }

        // 根据帧率调整
        base_bitrate = base_bitrate * fps / 30;

        current_bitrate = base_bitrate;
        min_bitrate = base_bitrate * 0.8;
        max_bitrate = base_bitrate * 2.0;
    }

    void update(AVFrame* frame, int quality_metric) {
        frame_counter++;

        // 每100帧检查一次质量
        if (frame_counter % 100 == 0) {
            // 质量下降时增加码率
            if (quality_metric < 30) {
                current_bitrate = std::min(max_bitrate, (int)(current_bitrate * 1.2));
                current_crf = std::max(min_crf, current_crf - 0.5);
                std::cout << "质量下降，提升码率至: " << current_bitrate/1000000 << " Mbps, CRF: " << current_crf << std::endl;
            }
            // 质量过高时降低码率
            else if (quality_metric > 45) {
                current_bitrate = std::max(min_bitrate, (int)(current_bitrate * 0.9));
                current_crf = std::min(max_crf, current_crf + 0.3);
                std::cout << "质量良好，降低码率至: " << current_bitrate/1000000 << " Mbps, CRF: " << current_crf << std::endl;
            }
        }
    }

    int getBitrate() const { return current_bitrate; }
    double getCRF() const { return current_crf; }
};

int main(int argc, char* argv[]) {
    const char* inputA = argv[1];
    const char* inputB = argv[2];
    const char* output = "output.mp4";

    // 初始化FFmpeg

    av_log_set_level(AV_LOG_VERBOSE);

    // 创建质量分析器
    VideoQualityAnalyzer qualityAnalyzer;

    // 1. 准备视频B的帧映射
    std::cout << "步骤1: 提取视频B的关键帧..." << std::endl;
    std::map<int, AVFrame*> bFrames;

    FFmpegResources resB;
    int ret = avformat_open_input(&resB.fmtCtx, inputB, nullptr, nullptr);
    CHECK_ERROR(ret, "打开B.mp4失败");

    ret = avformat_find_stream_info(resB.fmtCtx, nullptr);
    CHECK_ERROR(ret, "获取B.mp4流信息失败");

    int videoStreamB = av_find_best_stream(resB.fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStreamB < 0) {
        std::cerr << "错误: 在B.mp4中找不到视频流" << std::endl;
        return -1;
    }

    AVCodecParameters* codecParB = resB.fmtCtx->streams[videoStreamB]->codecpar;
    const AVCodec* decoderB = avcodec_find_decoder(codecParB->codec_id);
    CHECK_NULL(decoderB, "找不到B.mp4的解码器");

    resB.codecCtx = avcodec_alloc_context3(decoderB);
    CHECK_NULL(resB.codecCtx, "为B.mp4分配解码器上下文失败");

    ret = avcodec_parameters_to_context(resB.codecCtx, codecParB);
    CHECK_ERROR(ret, "复制B.mp4编解码器参数失败");

    ret = avcodec_open2(resB.codecCtx, decoderB, nullptr);
    CHECK_ERROR(ret, "打开B.mp4解码器失败");

    resB.frame = av_frame_alloc();
    CHECK_NULL(resB.frame, "为B.mp4分配帧失败");

    AVPacket pkt;
    std::vector<int> processedSeconds;

    // 读取并解码视频B
    while (av_read_frame(resB.fmtCtx, &pkt) >= 0) {
        if (pkt.stream_index == videoStreamB) {
            ret = avcodec_send_packet(resB.codecCtx, &pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                av_packet_unref(&pkt);
                continue;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(resB.codecCtx, resB.frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) break;

                // 计算当前秒数
                double pts_sec = resB.frame->best_effort_timestamp *
                                av_q2d(resB.fmtCtx->streams[videoStreamB]->time_base);
                int sec = static_cast<int>(floor(pts_sec));

                // 如果是新秒的第一帧则存储
                if (bFrames.find(sec) == bFrames.end()) {
                    AVFrame* clone = av_frame_clone(resB.frame);
                    if (clone) {
                        bFrames[sec] = clone;
                        processedSeconds.push_back(sec);
                        // std::cout << "提取B.mp4第 " << sec << " 秒的帧 (PTS: "
                        //           << resB.frame->pts << ")" << std::endl;
                    }
                }
            }
        }
        av_packet_unref(&pkt);
    }

    // 刷新解码器
    avcodec_send_packet(resB.codecCtx, nullptr);
    while (avcodec_receive_frame(resB.codecCtx, resB.frame) >= 0) {
        // 处理剩余帧
    }

    std::cout << "成功提取 " << bFrames.size() << " 个关键帧" << std::endl;

    // 2. 处理视频A
    std::cout << "步骤2: 处理视频A并转换为60fps..." << std::endl;
    FFmpegResources resA;

    ret = avformat_open_input(&resA.fmtCtx, inputA, nullptr, nullptr);
    CHECK_ERROR(ret, "打开A.mp4失败");

    ret = avformat_find_stream_info(resA.fmtCtx, nullptr);
    CHECK_ERROR(ret, "获取A.mp4流信息失败");

    int videoStreamA = av_find_best_stream(resA.fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStreamA < 0) {
        std::cerr << "错误: 在A.mp4中找不到视频流" << std::endl;
        return -1;
    }

    AVCodecParameters* codecParA = resA.fmtCtx->streams[videoStreamA]->codecpar;
    const AVCodec* decoderA = avcodec_find_decoder(codecParA->codec_id);
    CHECK_NULL(decoderA, "找不到A.mp4的解码器");

    resA.codecCtx = avcodec_alloc_context3(decoderA);
    CHECK_NULL(resA.codecCtx, "为A.mp4分配解码器上下文失败");

    ret = avcodec_parameters_to_context(resA.codecCtx, codecParA);
    CHECK_ERROR(ret, "复制A.mp4编解码器参数失败");

    ret = avcodec_open2(resA.codecCtx, decoderA, nullptr);
    CHECK_ERROR(ret, "打开A.mp4解码器失败");

    // 创建动态码率控制器
    DynamicBitrateController bitrateController(
        resA.codecCtx->width,
        resA.codecCtx->height,
        60
    );

    // 准备输出上下文
    AVFormatContext* outCtx = nullptr;
    ret = avformat_alloc_output_context2(&outCtx, nullptr, "mp4", output);
    CHECK_ERROR(ret, "创建输出上下文失败");

    AVStream* outStream = avformat_new_stream(outCtx, nullptr);
    CHECK_NULL(outStream, "创建输出流失败");

    // 配置输出编码器 (H.264)
    const AVCodec* encoder = avcodec_find_encoder_by_name("libx264");
    if (!encoder) {
        encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    CHECK_NULL(encoder, "找不到H.264编码器");

    AVCodecContext* encCtx = avcodec_alloc_context3(encoder);
    CHECK_NULL(encCtx, "分配编码器上下文失败");

    // 设置编码参数
    encCtx->width = resA.codecCtx->width;
    encCtx->height = resA.codecCtx->height;
    encCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    encCtx->time_base = (AVRational){1, 60};  // 60fps

    // 使用动态码率控制
    encCtx->bit_rate = bitrateController.getBitrate();
    encCtx->rc_max_rate = bitrateController.getBitrate() * 1.5;
    encCtx->rc_buffer_size = bitrateController.getBitrate() * 2;

    // 高质量编码设置
    encCtx->gop_size = 60;
    encCtx->max_b_frames = 4;
    encCtx->has_b_frames = 1;
    encCtx->refs = 4;
    encCtx->qmin = 10;   // 最小量化值
    encCtx->qmax = 30;   // 最大量化值
    encCtx->trellis = 2; // 高质量量化

    // 高级编码参数
    av_opt_set(encCtx->priv_data, "preset", "slow", 0);
    av_opt_set(encCtx->priv_data, "tune", "film", 0);
    av_opt_set(encCtx->priv_data, "profile", "high", 0);
    av_opt_set_double(encCtx->priv_data, "crf", bitrateController.getCRF(), 0);
    av_opt_set(encCtx->priv_data, "x264-params",
              "aq-mode=3:psy-rd=1.0:deblock=-1:-1:merange=32:bframes=4:b-adapt=2:direct=auto",
              0);

    // 设置HDR元数据 (如果适用)
    if (resA.codecCtx->color_range == AVCOL_RANGE_JPEG ||
        resA.codecCtx->color_primaries == AVCOL_PRI_BT2020) {
        av_opt_set(encCtx->priv_data, "x264-params",
                  "colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc", 0);
        std::cout << "启用HDR编码参数" << std::endl;
    }

    ret = avcodec_open2(encCtx, encoder, nullptr);
    CHECK_ERROR(ret, "打开编码器失败");

    ret = avcodec_parameters_from_context(outStream->codecpar, encCtx);
    CHECK_ERROR(ret, "复制输出流参数失败");

    outStream->time_base = encCtx->time_base;

    // 打开输出文件
    if (!(outCtx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&outCtx->pb, output, AVIO_FLAG_WRITE);
        CHECK_ERROR(ret, "打开输出文件失败");
    }

    ret = avformat_write_header(outCtx, nullptr);
    CHECK_ERROR(ret, "写入文件头失败");

    // 3. 创建滤镜图进行帧率转换
    std::cout << "步骤3: 创建60fps转换滤镜..." << std::endl;
    resA.filterGraph = avfilter_graph_alloc();
    CHECK_NULL(resA.filterGraph, "分配滤镜图失败");

    AVFilterContext* bufferSrcCtx = nullptr;
    AVFilterContext* bufferSinkCtx = nullptr;

    const AVFilter* bufferSrc = avfilter_get_by_name("buffer");
    const AVFilter* bufferSink = avfilter_get_by_name("buffersink");

    char args[512];
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             resA.codecCtx->width, resA.codecCtx->height, resA.codecCtx->pix_fmt,
             resA.fmtCtx->streams[videoStreamA]->time_base.num,
             resA.fmtCtx->streams[videoStreamA]->time_base.den,
             resA.codecCtx->sample_aspect_ratio.num ? resA.codecCtx->sample_aspect_ratio.num : 1,
             resA.codecCtx->sample_aspect_ratio.den ? resA.codecCtx->sample_aspect_ratio.den : 1);

    ret = avfilter_graph_create_filter(&bufferSrcCtx, bufferSrc, "in", args, nullptr, resA.filterGraph);
    CHECK_ERROR(ret, "创建buffer source失败");

    ret = avfilter_graph_create_filter(&bufferSinkCtx, bufferSink, "out", nullptr, nullptr, resA.filterGraph);
    CHECK_ERROR(ret, "创建buffer sink失败");

    // 配置fps滤镜和高质量缩放
    const char* filterDesc = "fps=fps=60:round=down, format=yuv420p, scale=flags=lanczos";
    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();

    outputs->name = av_strdup("in");
    outputs->filter_ctx = bufferSrcCtx;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = bufferSinkCtx;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    ret = avfilter_graph_parse_ptr(resA.filterGraph, filterDesc, &inputs, &outputs, nullptr);
    CHECK_ERROR(ret, "解析滤镜描述失败");

    ret = avfilter_graph_config(resA.filterGraph, nullptr);
    CHECK_ERROR(ret, "配置滤镜图失败");

    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    // 4. 处理帧
    std::cout << "步骤4: 处理并编码视频帧..." << std::endl;
    resA.frame = av_frame_alloc();
    CHECK_NULL(resA.frame, "为A.mp4分配帧失败");

    int64_t frameCount = 0;
    int64_t replacedFrames = 0;
    int64_t lastPts = AV_NOPTS_VALUE;
    AVFrame* originalFrameForQuality = nullptr;

    while (av_read_frame(resA.fmtCtx, &pkt) >= 0) {
        if (pkt.stream_index == videoStreamA) {
            ret = avcodec_send_packet(resA.codecCtx, &pkt);
            while (ret >= 0) {
                ret = avcodec_receive_frame(resA.codecCtx, resA.frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                CHECK_ERROR(ret, "解码A.mp4帧失败");

                // 保存原始帧用于质量分析
                if (frameCount % 10 == 0) {
                    if (originalFrameForQuality) av_frame_free(&originalFrameForQuality);
                    originalFrameForQuality = av_frame_clone(resA.frame);
                }

                // 推入滤镜
                ret = av_buffersrc_add_frame(bufferSrcCtx, resA.frame);
                CHECK_ERROR(ret, "向滤镜输入帧失败");

                while (true) {
                    AVFrame* filteredFrame = av_frame_alloc();
                    CHECK_NULL(filteredFrame, "分配滤镜帧失败");

                    ret = av_buffersink_get_frame(bufferSinkCtx, filteredFrame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        av_frame_free(&filteredFrame);
                        break;
                    }
                    CHECK_ERROR(ret, "从滤镜获取帧失败");

                    // 检查是否为每秒第一帧 (60帧倍数)
                    double pts_sec = filteredFrame->pts * av_q2d(encCtx->time_base);
                    int sec = static_cast<int>(floor(pts_sec));

                    // 替换帧逻辑
                    if (frameCount % 60 == 0 && bFrames.find(sec) != bFrames.end()) {
                        AVFrame* bFrame = bFrames[sec];

                        // 创建高质量格式转换上下文
                        SwsContext* swsCtx = sws_getCachedContext(
                            resA.swsCtx,
                            bFrame->width, bFrame->height, static_cast<AVPixelFormat>(bFrame->format),
                            encCtx->width, encCtx->height, encCtx->pix_fmt,
                            SWS_LANCZOS | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);

                        if (!swsCtx) {
                            std::cerr << "警告: 无法创建转换上下文，跳过帧替换" << std::endl;
                        } else {
                            AVFrame* converted = av_frame_alloc();
                            CHECK_NULL(converted, "分配转换帧失败");

                            converted->width = encCtx->width;
                            converted->height = encCtx->height;
                            converted->format = encCtx->pix_fmt;
                            converted->pts = filteredFrame->pts;

                            ret = av_frame_get_buffer(converted, 32); // 32字节对齐
                            if (ret >= 0) {
                                // 高质量转换
                                sws_scale(swsCtx,
                                          bFrame->data, bFrame->linesize, 0, bFrame->height,
                                          converted->data, converted->linesize);

                                // 提升关键帧质量
                                converted->pict_type = AV_PICTURE_TYPE_I;

                                av_frame_unref(filteredFrame);
                                av_frame_free(&filteredFrame);
                                filteredFrame = converted;
                                replacedFrames++;

                                // std::cout << "替换第 " << sec << " 秒的帧 (时间: "
                                //           << pts_sec << "s)" << std::endl;
                            } else {
                                av_frame_free(&converted);
                                std::cerr << "警告: 无法分配转换帧缓冲区" << std::endl;
                            }
                        }
                    }

                    // 设置时间戳连续性
                    if (lastPts != AV_NOPTS_VALUE && filteredFrame->pts <= lastPts) {
                        filteredFrame->pts = lastPts + 1;
                    }
                    lastPts = filteredFrame->pts;

                    // 编码帧
                    ret = avcodec_send_frame(encCtx, filteredFrame);
                    if (ret < 0 && ret != AVERROR(EAGAIN)) {
                        std::cerr << "警告: 发送帧到编码器失败，错误: " << ret << std::endl;
                    }

                    // 更新码率控制器
                    if (originalFrameForQuality) {
                        qualityAnalyzer.analyze(originalFrameForQuality, filteredFrame);
                        bitrateController.update(filteredFrame, 35); // 模拟质量指标
                    }

                    while (ret >= 0) {
                        AVPacket encPkt;
                        av_init_packet(&encPkt);
                        encPkt.data = nullptr;
                        encPkt.size = 0;

                        ret = avcodec_receive_packet(encCtx, &encPkt);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                            break;
                        }
                        CHECK_ERROR(ret, "编码数据包失败");

                        // 重新调整时间戳
                        av_packet_rescale_ts(&encPkt, encCtx->time_base, outStream->time_base);
                        encPkt.stream_index = outStream->index;

                        ret = av_interleaved_write_frame(outCtx, &encPkt);
                        if (ret < 0) {
                            std::cerr << "警告: 写入帧失败，错误: " << ret << std::endl;
                        }

                        av_packet_unref(&encPkt);
                    }

                    av_frame_free(&filteredFrame);
                    frameCount++;
                }
            }
        }
        av_packet_unref(&pkt);
    }

    // 刷新滤镜
    av_buffersrc_add_frame(bufferSrcCtx, nullptr);
    while (true) {
        AVFrame* filteredFrame = av_frame_alloc();
        ret = av_buffersink_get_frame(bufferSinkCtx, filteredFrame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_frame_free(&filteredFrame);
            break;
        }

        // 编码剩余帧
        avcodec_send_frame(encCtx, filteredFrame);
        av_frame_free(&filteredFrame);
    }

    // 刷新编码器
    avcodec_send_frame(encCtx, nullptr);
    while (true) {
        AVPacket encPkt;
        av_init_packet(&encPkt);
        encPkt.data = nullptr;
        encPkt.size = 0;

        ret = avcodec_receive_packet(encCtx, &encPkt);
        if (ret == AVERROR_EOF || ret < 0) break;

        av_packet_rescale_ts(&encPkt, encCtx->time_base, outStream->time_base);
        encPkt.stream_index = outStream->index;
        av_interleaved_write_frame(outCtx, &encPkt);
        av_packet_unref(&encPkt);
    }

    // 写入文件尾
    av_write_trailer(outCtx);

    // 5. 清理资源
    std::cout << "步骤5: 清理资源..." << std::endl;
    for (auto& pair : bFrames) {
        av_frame_free(&pair.second);
    }

    if (originalFrameForQuality) {
        av_frame_free(&originalFrameForQuality);
    }

    if (outCtx && !(outCtx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&outCtx->pb);
    }
    avformat_free_context(outCtx);
    avcodec_free_context(&encCtx);

    // 输出质量报告
    std::cout << "\n===== 视频质量报告 =====" << std::endl;
    std::cout << "总处理帧数: " << frameCount << std::endl;
    std::cout << "替换帧数: " << replacedFrames << std::endl;
    std::cout << "平均PSNR: " << qualityAnalyzer.getAveragePSNR() << " dB" << std::endl;
    std::cout << "平均SSIM: " << qualityAnalyzer.getAverageSSIM() << std::endl;
    std::cout << "最终码率: " << bitrateController.getBitrate()/1000000 << " Mbps" << std::endl;
    std::cout << "最终CRF: " << bitrateController.getCRF() << std::endl;
    std::cout << "输出文件: " << output << std::endl;
    std::cout << "质量日志已保存到: video_quality.log" << std::endl;

    return 0;
}