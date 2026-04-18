#ifndef TCPCLIENTHANDLER_H
#define TCPCLIENTHANDLER_H

#if 0
    此类为服务器端处理类：负责在独立线程中处理与单个树莓派客户端的所有网络通信操作
#endif

#include <QObject>
#include <QtNetwork/QTcpSocket>
#include <QThread>
#include <QFile>
#include <QTimer>

// TcpClientHandler类：负责在独立线程中处理与单个树莓派客户端的所有网络通信
// 数据收发、协议解析、命令转发、文件传输、状态管理
class TcpClientHandler : public QObject
{
    Q_OBJECT
public:
    // 构造函数：接收已连接的QTcpSocket对象，parent为父对象
    explicit TcpClientHandler(QTcpSocket* socket, QObject* parent = nullptr);
    ~TcpClientHandler();

signals:
    // 客户端连接断开时发送
    void disconnected();
    // 收到日志/结果时发送，参数为日志内容
    void logReceived(const QString& log);
    // 文件传输进度更新时发送，参数为已发送/接收字节数和总字节数
    void fileTransferProgress(qint64 sent, qint64 total);
public slots:
    // 发送Shell命令到下位机，参数为命令字符串
    void sendCommand(const QString& cmd);
    // 发送本地文件到下位机，参数为本地文件路径
    void sendFile(const QString& filePath);
    // 请求下位机发送指定文件，参数为下位机文件路径
    void requestFile(const QString& remotePath);
    //发送停止信息 相当于 Ctrl+C
    void sendStopCommand();
private slots:
    // QTcpSocket有数据可读时触发
    void onReadyRead();
    // QTcpSocket连接断开时触发
    void onDisconnected();

private:
    QTcpSocket* m_socket;         // 与下位机通信的TCP套接字
    QByteArray m_buffer;           // 数据接收缓冲区，用于处理粘包和不完整数据
    QFile* m_transferFile;         // 当前正在传输的文件对象
    qint64 m_transferTotal;        // 当前传输文件的总大小
    qint64 m_transferReceived;     // 当前已接收的文件字节数
    bool m_isReceivingFile;        // 是否正在接收文件的状态标志
    QByteArray m_fileWriteBuffer; //  文件写入批量缓冲
    const qint64 FILE_WRITE_BUFFER_SIZE = 128 * 1024; // 128KB写入缓冲
    const qint64 READ_CHUNK_SIZE = 256 * 1024; // 256KB单次读取块，和发送端匹配
    QTimer* m_fileReceiveTimer; // 文件接收超时定时器
    const int FILE_RECEIVE_TIMEOUT = 30000; // 30秒无数据超时

    // 处理接收缓冲区中的数据，解析协议
    void processData();
    // 发送带4字节大端序长度前缀的JSON数据
    void sendData(const QJsonObject& obj);
    // 开始接收文件，创建本地文件并初始化传输状态
    void startReceivingFile(const QString& fileName, qint64 fileSize);
    // 继续接收文件数据，写入本地文件
    void continueReceivingFile();
    // 完成文件接收，关闭文件并更新状态
    void finishReceivingFile(const QString& status);
};
#endif // TCPCLIENTHANDLER_H