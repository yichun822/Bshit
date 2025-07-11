cmake_minimum_required(VERSION 3.31)
project(untitled1)

set(CMAKE_CXX_STANDARD 20)


add_executable(untitled1 main.cpp)
target_link_libraries(untitled1 PRIVATE avcodec avformat avfilter avutil swscale)
