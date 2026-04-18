import socket
import json
import struct
import subprocess
import os
import time
import select
import shlex
import signal
import fcntl

# 配置参数
SERVER_IP = "192.168.140.16"
SERVER_PORT = 8888
HEARTBEAT_INTERVAL = 5
TIMEOUT = 30
CONNECTION_TIMEOUT = 180
BUFFER_SIZE = 65536

# 全局变量
current_work_dir = os.getcwd()
current_process = None  # 记录当前正在执行的命令进程

#长度前缀法 发送 长度包+JSON数据包
def send_data(sock, data):
    json_str = json.dumps(data)
    json_bytes = json_str.encode("utf-8")
    length_bytes = struct.pack(">I", len(json_bytes))
    sock.sendall(length_bytes + json_bytes)

def recv_data(sock):
    try:
        #先在缓冲区中接收四个字节的长度前缀数据
        length_bytes = sock.recv(4)
        if not length_bytes:
            return None
        #使用 struct 模块将二进制字节流解包为整数
        length = struct.unpack(">I", length_bytes)[0]
        if length == 0:
            return None

        #循环读取JSON数据
        data_bytes = b""
        while len(data_bytes) < length:
            chunk = sock.recv(min(BUFFER_SIZE, length - len(data_bytes)))
            if not chunk:
                return None
            data_bytes += chunk
        return json.loads(data_bytes.decode("utf-8"))
    except socket.timeout:
        raise
    except Exception as e:
        print(f"接收数据错误: {e}")
        return None

def set_non_blocking(fd):
    """设置文件描述符为非阻塞（Linux专用）"""
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

# 消除中间shell，信号直接命中目标进程
def execute_command(cmd):
    global current_work_dir, current_process
    cmd = cmd.strip()
    if not cmd:
        return ""
    
    try:
        # 处理cd命令
        if cmd.startswith("cd "):
            target_dir = cmd[3:].strip()
            new_dir = os.path.abspath(os.path.join(current_work_dir, target_dir)) if not os.path.isabs(target_dir) else target_dir
            if os.path.isdir(new_dir):
                current_work_dir = new_dir
                return f"✅ 已切换目录: {current_work_dir}"
            else:
                return f"❌ 目录不存在: {target_dir}"
        
        # 如果已有进程在运行，先停止
        if current_process:
            stop_current_process()
        
        # 加exec，让命令替换shell进程，无中间层
        cmd = f"exec {cmd}"
        # 启动新进程
        current_process = subprocess.Popen(
            cmd,
            shell=True,
            cwd=current_work_dir,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True
        )
        # 设置stdout为非阻塞，实现实时读取
        set_non_blocking(current_process.stdout.fileno())
        return f"✅ 命令已启动: {cmd[5:]}\n"
    except Exception as e:
        current_process = None
        return f"❌ 执行错误: {str(e)}"

def poll_command_output():
    """轻量级轮询：读取实时输出、检查进程状态"""
    global current_process
    if not current_process:
        return None, False

    output = ""
    # 检查进程是否结束
    if current_process.poll() is not None:
        # 读取剩余输出
        try:
            remaining = current_process.stdout.read()
            if remaining:
                output += remaining
        except:
            pass
        output += f"\n✅ 命令执行完成，退出码: {current_process.returncode}"
        current_process = None
        return output, True

    # 实时读取输出
    try:
        chunk = current_process.stdout.read(4096)
        if chunk:
            output += chunk
    except BlockingIOError:
        pass  # 无数据，正常跳过
    except Exception as e:
        output += f"\n❌ 读取错误: {str(e)}"
        current_process = None
        return output, True

    return output if output else None, False

def stop_current_process():
    """真正的Ctrl+C，彻底终止当前命令"""
    global current_process
    if not current_process:
        return "ℹ️ 暂无正在执行的命令"

    output = ""
    try:
        pid = current_process.pid
        # 先发SIGINT（等效终端Ctrl+C）
        os.kill(pid, signal.SIGINT)
        time.sleep(0.5)
        # 检查进程是否还在
        if current_process.poll() is None:
            # 还在就发SIGTERM
            os.kill(pid, signal.SIGTERM)
            time.sleep(0.5)
            if current_process.poll() is None:
                #  最后强制SIGKILL兜底
                os.kill(pid, signal.SIGKILL)
                time.sleep(0.2)
        
        # 读取所有剩余输出，清空缓冲区
        try:
            remaining = current_process.stdout.read()
            if remaining:
                output += remaining
        except:
            pass
        output += "\n✅ 已中断命令执行(Ctrl+C)"
        current_process = None
        return output if output else "✅ 已中断当前命令"
    except Exception as e:
        # 强制杀死兜底
        try:
            if current_process:
                os.kill(current_process.pid, signal.SIGKILL)
                current_process = None
        except:
            current_process = None
        return "⚠️ 命令已强制终止"

def send_file(sock, file_path, last_heartbeat_send_ref):
    abs_path = os.path.abspath(os.path.join(current_work_dir, file_path))
    if not os.path.isfile(abs_path):
        send_data(sock, {"type": "error", "data": "文件不存在"})
        return

    file_name = os.path.basename(abs_path)
    file_size = os.path.getsize(abs_path)
    # 增大文件读取/发送块大小到128KB，减少系统调用次数
    SEND_CHUNK_SIZE = 128 * 1024
    
    # 发送文件元数据
    send_data(sock, {"type": "file_meta", "name": file_name, "size": file_size})

    try:
        with open(abs_path, "rb") as f:
            sent = 0
            while sent < file_size:
                chunk = f.read(SEND_CHUNK_SIZE)
                sock.sendall(chunk)
                sent += len(chunk)
                
                # 优化：每发送1MB才检查一次心跳，减少循环开销，不影响保活
                if sent % (1024*1024) == 0:
                    current_time = time.time()
                    if current_time - last_heartbeat_send_ref[0] > HEARTBEAT_INTERVAL:
                        send_data(sock, {"type": "heartbeat", "data": "ping"})
                        last_heartbeat_send_ref[0] = current_time
        # 发送结束标记
        send_data(sock, {"type": "file_end", "data": "success"})
    except Exception as e:
        print(f"发送文件异常: {e}")
        send_data(sock, {"type": "file_end", "data": f"传输失败: {str(e)}"})

def recv_file(sock, save_dir="./downloads"):
    abs_save_dir = os.path.abspath(os.path.join(current_work_dir, save_dir))
    os.makedirs(abs_save_dir, exist_ok=True)
    save_path = ""
    original_timeout = sock.gettimeout()
    try:
        meta = recv_data(sock)
        if not meta or meta["type"] != "file_meta":
            send_data(sock, {"type": "error", "data": "元数据失败"})
            return False

        file_name, file_size = meta["name"], meta["size"]
        save_path = os.path.join(abs_save_dir, file_name)
        sock.settimeout(None)
        
        with open(save_path, "wb") as f:
            received = 0
            while received < file_size:
                chunk = sock.recv(min(BUFFER_SIZE, file_size - received))
                if not chunk:
                    raise Exception("连接中断")
                f.write(chunk)
                received += len(chunk)
        
        end = recv_data(sock)
        success = end and end["data"] == "success"
        msg = f"✅ 接收成功: {save_path}" if success else "❌ 接收失败"
        send_data(sock, {"type": "result", "data": msg})
        return success
    except:
        sock.settimeout(original_timeout)
        if os.path.exists(save_path):
            os.remove(save_path)
        send_data(sock, {"type": "error", "data": "文件传输异常"})
        return False
    finally:
        sock.settimeout(original_timeout)

def main():
    global current_work_dir
    while True:
        sock = None
        is_file_transfer = False
        last_heartbeat_recv = time.time()
        last_heartbeat_send = time.time()
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.settimeout(TIMEOUT)
            sock.connect((SERVER_IP, SERVER_PORT))
            print(f"已连接到上位机")
            current_work_dir = os.getcwd()
            last_heartbeat_send = time.time()

            
            # 大文件传输默认关闭TCP_NODELAY，开启Nagle算法，交互场景再临时开启
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 0)
            # 增大发送/接收缓冲区到256KB，可根据树莓派内存调整到1MB
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 256*1024)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 256*1024)
            # 开启TCP保活，避免大文件传输中途被防火墙断开
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            sock.settimeout(TIMEOUT)
            sock.connect((SERVER_IP, SERVER_PORT))

            while True:
                current_time = time.time()

                # 主循环加入命令轮询
                if not is_file_transfer:
                    output, finished = poll_command_output()
                    if output:
                        send_data(sock, {"type": "result", "data": output})

                if not is_file_transfer:
                    if current_time - last_heartbeat_send > HEARTBEAT_INTERVAL:
                        send_data(sock, {"type": "heartbeat", "data": "ping"})
                        last_heartbeat_send = current_time
                    if current_time - last_heartbeat_recv > CONNECTION_TIMEOUT:
                        print("长时间无响应，断开")
                        break
                    
                    #socket 非阻塞可读检测
                    readable, _, _ = select.select([sock], [], [], 0.1)
                    if not readable:
                        continue

                try:
                    data = recv_data(sock) if not is_file_transfer else None
                except socket.timeout:
                    continue

                if data is None and not is_file_transfer:
                    print("连接断开")
                    break

                if not is_file_transfer and data:
                    last_heartbeat_recv = time.time()

                if data:
                    if data["type"] == "cmd":
                        result = execute_command(data["data"])
                        send_data(sock, {"type": "result", "data": result})
                    elif data["type"] == "stop":
                        result = stop_current_process()
                        send_data(sock, {"type": "result", "data": result})
                    elif data["type"] == "send_file":
                        is_file_transfer = True
                        heartbeat_ref = [last_heartbeat_send]
                        send_file(sock, data["data"], heartbeat_ref)
                        last_heartbeat_send = heartbeat_ref[0]
                        is_file_transfer = False
                        last_heartbeat_recv = time.time()
                    elif data["type"] == "prepare_recv_file":
                        is_file_transfer = True
                        recv_file(sock)
                        is_file_transfer = False
                        last_heartbeat_recv = time.time()
                    elif data["type"] == "heartbeat":
                        last_heartbeat_recv = time.time()

        except Exception as e:
            print(f"异常: {e}，5秒后重连")
        finally:
            if sock:
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except:
                    pass
                sock.close()
            time.sleep(5)

if __name__ == "__main__":
    main()