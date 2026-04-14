#include "ui_Host_Computer.h"
#include "Host_Computer.h"
#include <QMessageBox>
#include <QFileDialog>
#include <QTimer>

Host_Computer::Host_Computer(QWidget * parent)
    : QMainWindow(parent)
    , ui(new Ui::Host_ComputerClass)
    , m_server(nullptr)
    , m_clientHandler(nullptr)
    , m_clientThread(nullptr)
{
    ui->setupUi(this);
    setWindowTitle("远程控制器");
    //初始隐藏进度条
    ui->progressBar->setVisible(false);
    //初始禁用按钮
    ui->sendCmdBtn->setEnabled(false);
    ui->sendFileBtn->setEnabled(false);
    ui->requestFileBtn->setEnabled(false);
    // 按钮点击事件连接
    // 启动/停止服务器
    connect(ui->toggleServerBtn, &QPushButton::clicked, this, &Host_Computer::toggleServer);
    // 命令发送
    connect(ui->sendCmdBtn, &QPushButton::clicked, this, &Host_Computer::sendCommandClicked);
    // 命令发送 但是为点击 Enter 键
    connect(ui->cmdLineEdit, &QLineEdit::returnPressed, this, &Host_Computer::sendCommandClicked);
    // 文件发送
    connect(ui->sendFileBtn, &QPushButton::clicked, this, &Host_Computer::selectAndSendFile);
    // 文件请求
    connect(ui->requestFileBtn, &QPushButton::clicked, this, &Host_Computer::requestFileClicked);
  
}
Host_Computer::~Host_Computer()
{
    // 先停止客户端线程，再关闭服务器（避免线程持有socket导致崩溃）
    if (m_clientThread) {
        m_clientThread->quit();
        // 给wait()加3秒超时，超时则强制终止线程
        if (!m_clientThread->wait(3000)) {
            m_clientThread->terminate();
            m_clientThread->wait();
        }
        m_clientThread->deleteLater();
        m_clientThread = nullptr;
    }
    // 关闭服务器，停止接受新连接
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    delete ui;
}
//打开或关闭服务器
void Host_Computer::toggleServer()
{
    //若服务器对象不存在 说明要创建
    if (!m_server) {
        m_server = new QTcpServer(this);
        //当有新连接时 调用onNewConnection函数 添加参数确保绑定不重复
        connect(m_server, &QTcpServer::newConnection, this, &Host_Computer::onNewConnection, Qt::UniqueConnection);
        //监听端口
        if (m_server->listen(QHostAddress::Any, 8888)) {
            ui->statusLabel->setText("服务器已启动，端口：8888");
            ui->toggleServerBtn->setText("停止服务器");
        }
        else {
            QMessageBox::critical(this, "错误", "无法启动服务器：" + m_server->errorString());
            m_server->deleteLater();
            m_server = nullptr;
        }
    }
    //若服务器若服务器对象存在 说明需要关闭清空
    else {
        //先关闭再删除
        m_server->close();
        m_server->deleteLater();
        //置于空指针 安全之法
        m_server = nullptr;
        //更新UI
        ui->statusLabel->setText("服务器已停止");
        ui->toggleServerBtn->setText("启动服务器");
        // 关闭服务器时，主动断开客户端连接（避免残留）
        if (m_clientHandler) {
            onClientDisconnected(); // 触发客户端断开逻辑
        }
    }
}
//处理新连接的槽
void Host_Computer::onNewConnection()
{
    //获取连接队列中下一个连接 也就是第一个连接
    QTcpSocket* socket = m_server->nextPendingConnection();
    if (m_clientHandler) {
        // 正确清理新socket（先关闭再删除）
        socket->abort(); // 强制关闭
        socket->deleteLater();
        QMessageBox::information(this, "提示", "已有客户端连接，拒绝新连接");
        //防止原对象还未释放完成 从而频繁触发弹窗 强制置于空可以使得弹窗最多触发一次
        m_clientHandler = nullptr;
        return;
    }
    //创建线程并将客户端处理对象移动到线程中
    m_clientThread = new QThread(this);
    m_clientHandler = new TcpClientHandler(socket);
    m_clientHandler->moveToThread(m_clientThread);
    // 线程结束自动清理
    connect(m_clientThread, &QThread::finished, m_clientHandler, &QObject::deleteLater);
    // 客户端状态信号连接
    // 客户端断开连接时，线程结束
    connect(m_clientHandler, &TcpClientHandler::disconnected, this, &Host_Computer::onClientDisconnected, Qt::UniqueConnection);
    // 客户端执行结果返回到屏幕上
    connect(m_clientHandler, &TcpClientHandler::logReceived, this, &Host_Computer::appendLog);
    // 文件传输进度更新 跨进程安全
    connect(m_clientHandler, &TcpClientHandler::fileTransferProgress, this, &Host_Computer::updateProgress,Qt::QueuedConnection);
    // 把MainWindow的信号 和 客户端处理类的槽函数 连接
    connect(this, &Host_Computer::sendCommand, m_clientHandler, &TcpClientHandler::sendCommand);
    connect(this, &Host_Computer::sendFile, m_clientHandler, &TcpClientHandler::sendFile);
    connect(this, &Host_Computer::requestFile, m_clientHandler, &TcpClientHandler::requestFile);
    // 停止执行命令
    connect(this, &Host_Computer::sendStopCommand, m_clientHandler, &TcpClientHandler::sendStopCommand);
    //启动线程
    m_clientThread->start();
    //设置UI 使按钮可以操作
    ui->statusLabel->setText("客户端已连接：" + socket->peerAddress().toString());
    ui->sendCmdBtn->setEnabled(true);
    ui->sendFileBtn->setEnabled(true);
    ui->requestFileBtn->setEnabled(true);
}
// 客户端断开连接槽
void Host_Computer::onClientDisconnected()
{
    // 线程安全退出
    if (m_clientThread) {
        m_clientThread->quit();
        if (!m_clientThread->wait(3000)) {
            // 超时则强制终止线程
            m_clientThread->terminate();
            m_clientThread->wait();
        }
        m_clientThread->deleteLater();
        m_clientThread = nullptr;
    }
    // 强制清空客户端对象，释放旧连接，允许新连接接入
    m_clientHandler = nullptr;
    // 更新UI状态
    ui->statusLabel->setText("客户端已断开");
    ui->sendCmdBtn->setEnabled(false);
    ui->sendFileBtn->setEnabled(false);
    ui->requestFileBtn->setEnabled(false);
    ui->progressBar->setVisible(false);
}
// 客户端返回的信息显示槽
void Host_Computer::appendLog(const QString& log)
{
    ui->logTextEdit->append(log);
}
// 文件传输进度更新槽
void Host_Computer::updateProgress(qint64 sent, qint64 total)
{
    
    //显示进度条
    ui->progressBar->setVisible(true);
    ui->progressBar->setMaximum(static_cast<int>(total));
    ui->progressBar->setValue(static_cast<int>(sent));
    //当进度条到底时隐藏
    if (sent == total) {
        QTimer::singleShot(1000, this, [=](){
            // 定时器回调中增加UI有效性判断
            if (ui) ui->progressBar->setVisible(false);
            });
    }
}
// 发送命令按钮点击槽函数，发射MainWindow自己的信号
void Host_Computer::sendCommandClicked()
{
    //获取命令行输入文本 去两端空白字符
    QString cmd = ui->cmdLineEdit->text().trimmed();
    if (cmd.isEmpty()) {
        return;
    }
    // 发送信号
    emit sendCommand(cmd);
    //发送完即清空
    ui->cmdLineEdit->clear();
}
// 发送文件按钮点击槽函数
void Host_Computer::selectAndSendFile()
{
    //便利方法获取选择单个文件
    QString filePath = QFileDialog::getOpenFileName(this, "选择要发送的文件");
    if (!filePath.isEmpty()) {
        emit sendFile(filePath);
    }
}
// 请求文件按钮点击槽函数
void Host_Computer::requestFileClicked()
{
    //获取命令行输入文本 去两端空白字符
    QString remotePath = ui->requestFileLineEdit->text().trimmed();
    if (remotePath.isEmpty()) {
        QMessageBox::information(this, "提示", "请输入树莓派上的文件路径");
        return;
    }
    emit requestFile(remotePath);
    ui->requestFileLineEdit->clear();
}

//清空结果
void Host_Computer::on_clear_outcomes_clicked()
{
    ui->logTextEdit->clear();
}

//停止当前执行命令
void  Host_Computer::on_stopbtn_clicked()
{
    if (m_clientHandler) {
        // 发送stop指令
        emit sendStopCommand();
        appendLog("ℹ️ 已发送中断信号");
    }
    else {
        appendLog("❌ 未连接客户端");
    }
}