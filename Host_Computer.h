#pragma once
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QtWidgets/QMainWindow>
//TCP服务器，用于创建TCP服务器端：处理新连接、断开连接、发送和接收数据的操作 属于 QtNetwork 类
#include <QtNetwork/QTcpServer>
#include "ui_Host_Computer.h"
//Tcp服务器端各种处理函数的封装
#include "TcpClientHandler.h"

QT_BEGIN_NAMESPACE
namespace Ui { class Host_ComputerClass; };
QT_END_NAMESPACE

class Host_Computer : public QMainWindow
{
    Q_OBJECT
public:
    Host_Computer(QWidget* parent = nullptr);
    ~Host_Computer();

signals:      //这些信号 是调用TcpClientHandler类使用的
    //发送命令
    void sendCommand(const QString& cmd);
    //发送文件
    void sendFile(const QString& filePath);
    //请求接收文件
    void requestFile(const QString& remotePath);
    // 停止当前执行命令
    void sendStopCommand(); 

private slots:
    //切换服务器状态 打开/关闭服务器
    void toggleServer();
    //处理新连接
    void onNewConnection();
    //处理客户端断开连接
    void onClientDisconnected();
    //处理客户端发送的数据
    void appendLog(const QString& log);
    //更新文件传输进度条
    void updateProgress(qint64 sent, qint64 total);
    //处理发送命令按钮点击事件
    void sendCommandClicked(); 
    //处理发送文件按钮点击事件
    void selectAndSendFile();
    //处理请求接收文件按钮点击事件
    void requestFileClicked(); 
    //清空结果
    void on_clear_outcomes_clicked();
    //停止当前执行命令
    void on_stopbtn_clicked();
private:
    Ui::Host_ComputerClass* ui;
    //服务器对象
    QTcpServer* m_server;
    //网络通信控制类
    TcpClientHandler* m_clientHandler;
    //线程类
    QThread* m_clientThread;
};
#endif // MAINWINDOW_H