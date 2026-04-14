#include <QJsonObject>
#include <QJsonDocument>
#include "TcpClienthandler.h"
#include <QStandardPaths>
#include <QDir>
#include <QThread> // 线程相关头文件
//初始化成员变量，连接QTcpSocket的信号槽
TcpClientHandler::TcpClientHandler(QTcpSocket* socket, QObject* parent)
    : QObject(parent), m_socket(socket), m_transferFile(nullptr),
    m_transferTotal(0), m_transferReceived(0), m_isReceivingFile(false)
{
    // 将socket的父对象设置为当前handler
    m_socket->setParent(this);
    // 连接QTcpSocket的readyRead信号到本类的onReadyRead槽
    connect(m_socket, &QTcpSocket::readyRead, this, &TcpClientHandler::onReadyRead);
    // 连接QTcpSocket的disconnected信号到本类的onDisconnected槽
    connect(m_socket, &QTcpSocket::disconnected, this, &TcpClientHandler::onDisconnected);

    // 接收缓存
    m_socket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 256 * 1024);
    m_socket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, 256 * 1024);
    // 禁用Nagle算法
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
}
//清理文件对象和套接字
TcpClientHandler::~TcpClientHandler()
{
    // 如果有正在传输的文件，关闭并删除文件对象
    if (m_transferFile) {
        m_transferFile->close();
        delete m_transferFile;
    }
    // 用abort()强制断开避免阻塞
    if (m_socket) {
        m_socket->abort();
        m_socket->deleteLater();
    }
}
// 槽函数：处理套接字的可读事件
void TcpClientHandler::onReadyRead()
{
    // 如果正在接收文件，直接处理文件数据，不进入协议解析缓冲区
    if (m_isReceivingFile) {
        continueReceivingFile();
    }
    else {
        // 否则将数据追加到协议解析缓冲区，然后处理
        m_buffer.append(m_socket->readAll());
        processData();
    }
}
// 解析接收缓冲区中的数据，处理粘包和协议
void TcpClientHandler::processData()
{
    // 循环处理缓冲区，直到数据不足一个完整包
    while (m_buffer.size() >= 4) {
        // 读取前4字节，转换为小端序的32位无符号整数
        quint32 length = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(m_buffer.constData()));
        // 防御性判断，避免长度为0导致的死循环
        if (length == 0) {
            m_buffer = m_buffer.mid(4);
            continue;
        }
        // 如果缓冲区数据不足（4字节长度 + 实际数据），等待更多数据
        if (m_buffer.size() < 4 + length) {
            return;
        }
        // 提取完整的JSON数据部分
        QByteArray jsonData = m_buffer.mid(4, length);
        // 从缓冲区中移除已处理的数据（4字节长度 + JSON数据）
        m_buffer = m_buffer.mid(4 + length);
        // 解析JSON数据为文档对象
        QJsonDocument doc = QJsonDocument::fromJson(jsonData);
        // 如果不是有效的JSON对象，跳过
        if (!doc.isObject()) {
            emit logReceived("警告：收到无效JSON数据包，已丢弃");
            continue;
        }
        // 获取JSON对象
        QJsonObject obj = doc.object();
        // 获取协议类型字段
        QString type = obj["type"].toString();
        // 根据协议类型分发处理
        if (type == "result" || type == "error") {
            // 命令执行结果或错误，发送logReceived信号给UI显示
            emit logReceived(obj["data"].toString());
        }
        // 心跳包处理，收到心跳立即回应，兼容下位机心跳逻辑
        else if (type == "heartbeat") {
            sendData(QJsonObject{ {"type", "heartbeat"}, {"data", "pong"} });
        }
        else if (type == "file_meta") {
            // 文件元数据（名称、大小），开始接收文件
            startReceivingFile(obj["name"].toString(), obj["size"].toVariant().toLongLong());
        }
        else if (type == "file_end") {
            // 文件传输结束标记，完成接收
            finishReceivingFile(obj["data"].toString());
        }
        else {
            emit logReceived("警告：收到未知类型数据包，类型：" + type);
        }
    }
}
// 发送带长度前缀的JSON数据
void TcpClientHandler::sendData(const QJsonObject& obj)
{
    // 添加套接字状态判断，避免无效发送
    if (!m_socket || m_socket->state() != QTcpSocket::ConnectedState) {
        return;
    }
    // 将JSON对象转换为紧凑格式的JSON文档
    QJsonDocument doc(obj);
    // 将JSON文档转换为字节数组 UTF-8编码
    QByteArray jsonData = doc.toJson(QJsonDocument::Compact);
    // 准备4字节长度前缀
    QByteArray lengthData;
    lengthData.resize(4);
    // 将JSON数据长度转换为大端序（网络字节序） 将 Json 数据长度写入 lengthData
    // 强制转换为quint32，避免int溢出风险
    qToBigEndian<quint32>(static_cast<quint32>(jsonData.size()), reinterpret_cast<uchar*>(lengthData.data()));
    // 先发送长度前缀，再发送JSON数据
    m_socket->write(lengthData);
    m_socket->write(jsonData);
    // 刷新套接字缓冲区，确保数据立即发送
    m_socket->flush();
}
// 槽函数：发送Shell命令到下位机
void TcpClientHandler::sendCommand(const QString& cmd)
{
    /* 构建JSON对象并发送 */
    // 构建命令协议JSON对象
    QJsonObject obj;
    obj["type"] = "cmd";
    obj["data"] = cmd;
    // 发送数据 
    sendData(obj);
    // 发送logReceived信号，在UI上显示发送的命令
    emit logReceived("> " + cmd);
}
// 槽函数：发送本地文件到下位机
void TcpClientHandler::sendFile(const QString& filePath)
{
    // 检查文件是否存在且是普通文件
    if (!QFile::exists(filePath) || !QFileInfo(filePath).isFile()) {
        emit logReceived("错误: 文件不存在或不是普通文件");
        return;
    }
    // 打开文件为只读模式
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit logReceived("错误: 无法打开文件");
        return;
    }
    // 先通知下位机准备接收文件，避免数据先发导致解析错误
    sendData(QJsonObject{ {"type", "prepare_recv_file"} });
    m_socket->waitForBytesWritten(500);
    QThread::msleep(100);
    // 发送文件元数据（名称、大小）
    QJsonObject metaObj;
    metaObj["type"] = "file_meta";
    metaObj["name"] = QFileInfo(filePath).fileName();
    metaObj["size"] = file.size();
    sendData(metaObj);
    m_socket->waitForBytesWritten(500);
    // 分块发送文件二进制数据
    //获取起始 便于进度条更新
    qint64 sent = 0;
    qint64 total = file.size();
    // 4KB缓冲区，平衡内存占用和传输效率
    char buffer[4096];
    //分块发送文件
    while (sent < total) {
        // 读取一块数据
        qint64 read = file.read(buffer, sizeof(buffer));
        if (read <= 0) {
            break;
        }
        // 检查写入结果，处理socket写缓冲区满的情况
        qint64 written = m_socket->write(buffer, read);
        if (written < 0) {
            emit logReceived("错误: 文件数据发送失败");
            break;
        }
        // 等待数据写入
        m_socket->waitForBytesWritten(100); // 100ms超时
        sent += written;
        // 发送进度更新信号
        emit fileTransferProgress(sent, total);
    }
    // 关闭文件
    file.close();
    // 发送文件传输结束标记
    QJsonObject endObj;
    endObj["type"] = "file_end";
    endObj["data"] = sent == total ? "success" : "传输中断";
    sendData(endObj);
    m_socket->flush(); // 强制刷新Socket缓冲区，确保数据立刻发出
}
// 槽函数：请求下位机发送指定文件
void TcpClientHandler::requestFile(const QString& remotePath)
{
    // 构建请求文件协议JSON对象
    QJsonObject obj;
    obj["type"] = "send_file";
    obj["data"] = remotePath;
    // 发送数据
    sendData(obj);
    // 发送logReceived信号，在UI上显示请求
    emit logReceived("> 请求文件: " + remotePath);
}
// 开始接收文件
void TcpClientHandler::startReceivingFile(const QString& fileName, qint64 fileSize)
{
    // 获取系统下载目录，并创建RemoteFiles子目录
    QString saveDir = "D:/QT_xm/HostComputer/downloads";
    QDir().mkpath(saveDir);
    // 拼接完整的本地保存路径
    QString savePath = saveDir + "/" + fileName;
    // 创建文件对象并打开为只写模式
    m_transferFile = new QFile(savePath);
    if (!m_transferFile->open(QIODevice::WriteOnly)) {
        // 打开失败，清理并发送错误
        delete m_transferFile;
        m_transferFile = nullptr;
        sendData(QJsonObject{ {"type", "error"}, {"data", "无法创建本地文件"} });
        return;
    }

    // 初始化文件传输状态
    m_transferTotal = fileSize;
    m_transferReceived = 0;
    m_isReceivingFile = true;
    m_fileWriteBuffer.clear(); // 初始化写入缓冲
    m_fileWriteBuffer.reserve(FILE_WRITE_BUFFER_SIZE); // 预分配缓冲内存

    emit logReceived("开始接收文件: " + fileName);

    // 开始接收时立即发送0%进度，强制UI显示进度条
    emit fileTransferProgress(0, fileSize);

    // 检查套接字中是否已有剩余数据，立即处理
    continueReceivingFile();
}
// 继续接收文件数据
void TcpClientHandler::continueReceivingFile()
{
    // 检查文件对象和状态是否有效
    if (!m_transferFile || !m_isReceivingFile) {
        return;
    }

    // 传输开始时强制更新一次进度，确保UI显示进度条
    if (m_transferReceived == 0) {
        emit fileTransferProgress(0, m_transferTotal);
    }

    // 循环读取，一次性读满socket缓冲区
    while (true) {
        // 计算剩余需要接收的字节数
        qint64 remaining = m_transferTotal - m_transferReceived;
        if (remaining <= 0) {
            // 接收完成：先把缓冲里的剩余数据写入文件
            if (!m_fileWriteBuffer.isEmpty()) {
                m_transferFile->write(m_fileWriteBuffer);
                m_fileWriteBuffer.clear();
            }
            m_transferFile->flush();
            // 接收完成时强制更新100%进度
            emit fileTransferProgress(m_transferTotal, m_transferTotal);
            // 重置接收文件状态
            m_isReceivingFile = false;
            // 将套接字中剩余的数据追加到协议解析缓冲区
            m_buffer.append(m_socket->readAll());
            // 处理协议解析缓冲区
            processData();
            return;
        }

        // 单次读取块256KB
        QByteArray data = m_socket->read(qMin(remaining, READ_CHUNK_SIZE));
        if (data.isEmpty()) {
            // 无更多数据，退出循环，等待下一次readyRead信号
            break;
        }

        // 批量写入缓冲
        m_fileWriteBuffer.append(data);
        if (m_fileWriteBuffer.size() >= FILE_WRITE_BUFFER_SIZE) {
            m_transferFile->write(m_fileWriteBuffer);
            m_fileWriteBuffer.clear();
        }

        // 更新已接收字节数
        m_transferReceived += data.size();


        emit fileTransferProgress(m_transferReceived, m_transferTotal);
    }
}

// 完成文件接收
void TcpClientHandler::finishReceivingFile(const QString& status)
{
    // 关闭并删除文件对象
    if (m_transferFile) {
        // 把缓冲里的剩余数据写入文件
        if (!m_fileWriteBuffer.isEmpty()) {
            m_transferFile->write(m_fileWriteBuffer);
            m_fileWriteBuffer.clear();
        }
        m_transferFile->flush();
        m_transferFile->close();
        delete m_transferFile;
        m_transferFile = nullptr;
    }
    // 重置接收文件状态
    m_isReceivingFile = false;
    m_fileWriteBuffer.clear();
    // 根据传输状态发送日志
    if (status == "success") {
        emit logReceived("文件接收成功");
    }
    else {
        emit logReceived("文件接收失败: " + status);
    }
}
// 处理连接断开
void TcpClientHandler::onDisconnected()
{
    // 清理传输中的文件
    if (m_transferFile) {
        m_transferFile->close();
        m_transferFile->remove(); // 删除未完成的文件
        delete m_transferFile;
        m_transferFile = nullptr;
    }
    m_isReceivingFile = false;
    // 强制断开Socket，彻底释放资源
    if (m_socket) {
        m_socket->abort();
    }
    emit disconnected();
}

// 停止执行当前命令
void TcpClientHandler::sendStopCommand()
{
    QJsonObject obj;
    obj["type"] = "stop";
    sendData(obj);
}
