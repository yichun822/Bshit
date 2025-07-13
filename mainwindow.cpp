#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include <QFileDialog>
#include <thread>
#include "mkvideo.h"
#include <unistd.h>
#include <QMessageBox>
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
}

MainWindow::~MainWindow()
{
    delete ui;
}




void MainWindow::on_toolButton_clicked()
{
    QString filePath = QFileDialog::getOpenFileName(
        ui->toolButton,
        "选择文件",
        "",
        "所有文件 (*.*);;文本文件 (*.txt);;图像文件 (*.png *.jpg)"
        );
    ui->lineEdit->setText(filePath);
}


void MainWindow::on_toolButton_2_clicked()
{
    QString filePath = QFileDialog::getOpenFileName(
        ui->toolButton,
        "选择文件",
        "",
        "所有文件 (*.*);;文本文件 (*.txt);;图像文件 (*.png *.jpg)"
        );
    ui->lineEdit_2->setText(filePath);
}

//真进度条太麻烦了，做个假的意思意思得了
void MainWindow::on_pushButton_clicked()
{
    ui->progressBar->setValue(0);
    //mkvideo(ui->lineEdit->text().toStdString(),ui->lineEdit_2->text().toStdString());
    int sign=0;
    std::thread outvideo(mkvideo,ui->lineEdit->text().toStdString(),ui->lineEdit_2->text().toStdString(),std::ref(sign));
    int cnt=0;
    while (cnt<=90) {
        ui->progressBar->setValue(cnt++);
        usleep(200000);
    }
    outvideo.join();
    ui->progressBar->setValue(100);
    if (sign==0)QMessageBox::information(this,"消息       ","成功生成",QMessageBox::Ok);
    else QMessageBox::critical(this,"error","错误      ",QMessageBox::Ok);

}

