#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstring>
#include <fstream>
#include <chrono> // 用于计算传输时间
#pragma comment(lib, "Ws2_32.lib")

using namespace std;

#define SERVER_PORT 3333
#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 1024

#define ROUTER_IP "127.0.0.1" // 路由器的 IP 地址
#define ROUTER_PORT 2222     // 路由器的端口

struct Message {
    uint32_t Seq;
    uint32_t Ack;
    uint16_t Flag;
    uint16_t Length;
    uint16_t Checksum;
    char Data[BUFFER_SIZE];
};

enum Flag {
    SYN = 1,
    ACK = 2,
    FIN = 4,
    DATA = 8,
    FILENAME = 16,
    CLOSE = 32
};

// 计算校验和函数
uint16_t calculateChecksum(const char* data, int length) {
    uint32_t sum = 0;
    const uint16_t* ptr = reinterpret_cast<const uint16_t*>(data);

    while (length > 1) {
        sum += *ptr++;
        length -= 2;
    }

    if (length > 0) {
        uint16_t last_byte = 0;
        *reinterpret_cast<uint8_t*>(&last_byte) = *reinterpret_cast<const uint8_t*>(ptr);
        sum += last_byte;
    }

    // 将 32 位的 sum 转换为 16 位
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return static_cast<uint16_t>(~sum);
}

// 初始化 Winsock
void initWinsock() {
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        cerr << "WSAStartup failed: " << result << endl;
        exit(1);
    }
}

// 创建 UDP 套接字
SOCKET createSocket() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        cerr << "Failed to create socket: " << WSAGetLastError() << endl;
        WSACleanup();
        exit(1);
    }
    return sock;
}

// 配置套接字地址
sockaddr_in configureAddress(const char* ip, int port) {
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    return addr;
}

// 发送消息到服务器
void sendMessage(SOCKET sock, const sockaddr_in& destAddr, Message& msg) {
    // 计算校验和
    msg.Checksum = 0; // 在计算前将校验和字段设为 0
    msg.Checksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));

    int sendResult = sendto(sock, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
        reinterpret_cast<const sockaddr*>(&destAddr), sizeof(destAddr));
    if (sendResult == SOCKET_ERROR) {
        cerr << "发送失败，错误码: " << WSAGetLastError() << endl;
        closesocket(sock);
        WSACleanup();
        exit(1);
    }

    // 日志输出
    cout << "发送数据包 - Seq: " << msg.Seq << ", Ack: " << msg.Ack
        << ", Checksum: " << msg.Checksum << ", Flag: " << msg.Flag << endl;
}

// 带超时重传的发送函数
int sendWithTimeout(SOCKET sock, const sockaddr_in& destAddr, Message& msg, uint32_t expectedAck, int timeoutMs, int maxRetransmissions) {
    int retransmissions = 0;
    fd_set readfds;
    timeval timeout;
    sockaddr_in from;
    int fromSize = sizeof(from);
    Message recvMsg;

    while (retransmissions < maxRetransmissions) {
        // 发送消息
        sendMessage(sock, destAddr, msg);

        // 设置超时时间
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;

        // 等待ACK
        int selectResult = select(0, &readfds, NULL, NULL, &timeout);
        if (selectResult > 0) {
            // 接收ACK
            int bytesReceived = recvfrom(sock, reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg), 0,
                reinterpret_cast<sockaddr*>(&from), &fromSize);

            if (bytesReceived > 0) {
                // 验证校验和
                uint16_t receivedChecksum = recvMsg.Checksum;
                recvMsg.Checksum = 0;
                uint16_t calculatedChecksum = calculateChecksum(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
                if (receivedChecksum != calculatedChecksum) {
                    cerr << "收到的ACK校验和不匹配，丢弃数据。" << endl;
                    continue;
                }

                if ((recvMsg.Flag & ACK) && recvMsg.Ack == msg.Seq + 1) {
                    // 收到正确的ACK

                    // 日志输出
                    cout << "收到ACK - Seq: " << recvMsg.Seq << ", Ack: " << recvMsg.Ack
                        << ", Checksum: " << receivedChecksum << endl;

                    return 0;
                }
            }
        }
        else if (selectResult == 0) {
            // 超时，重传
            retransmissions++;
            cout << "超时，正在重传数据包 (" << retransmissions << "/" << maxRetransmissions << ")" << endl;
        }
        else {
            // 发生错误
            cerr << "Select函数出错: " << WSAGetLastError() << endl;
            return -1;
        }
    }

    // 超过最大重传次数
    cerr << "超过最大重传次数，未收到ACK确认。" << endl;
    return -1;
}

int main() {
    initWinsock();
    SOCKET clientSocket = createSocket();
    // 配置服务器地址
    sockaddr_in serverAddr = configureAddress(SERVER_IP, SERVER_PORT);

    // 配置路由器地址
    sockaddr_in routerAddr = configureAddress(ROUTER_IP, ROUTER_PORT);
    int routerAddrLen = sizeof(routerAddr);

    // 如果需要通过路由器发送数据，将 destAddr 设置为 routerAddr
    sockaddr_in destAddr = routerAddr; // 或者直接使用 routerAddr

    // 开始连接建立（三次握手）
    Message msg = {};
    msg.Seq = 1;  // 初始序列号
    msg.Flag = SYN;

    // 设置超时和最大重传次数
    int timeoutMs = 1000;  // 超时时间：1秒
    int maxRetransmissions = 5;  // 最大重传次数：3次

    cout << "尝试连接服务器..." << endl;
    sendMessage(clientSocket, destAddr, msg);

    // 等待服务器的 SYN-ACK
    sockaddr_in from;
    int fromSize = sizeof(from);
    int bytesReceived = recvfrom(clientSocket, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
        reinterpret_cast<sockaddr*>(&from), &fromSize);
    if (bytesReceived > 0 && (msg.Flag & (SYN | ACK))) {
        // 验证校验和
        uint16_t receivedChecksum = msg.Checksum;
        msg.Checksum = 0;
        uint16_t calculatedChecksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));
        if (receivedChecksum != calculatedChecksum) {
            cerr << "收到的SYN-ACK校验和不匹配。" << endl;
            closesocket(clientSocket);
            WSACleanup();
            return 1;
        }

        cout << "收到 SYN-ACK，发送 ACK..." << endl;
        msg.Flag = ACK;
        sendMessage(clientSocket, destAddr, msg);
        cout << "三次握手成功。" << endl;
    }
    else {
        cerr << "连接建立过程中出错。" << endl;
        closesocket(clientSocket);
        WSACleanup();
        return 1;
    }

    bool connected = true;
    while (connected) {
        cout << "请选择操作: 1 - 发送文件, 2 - 断开连接" << endl;
        int choice;
        cin >> choice;

        if (choice == 1) {
            cout << "请输入文件路径：" << endl;
            string filePath;
            cin >> filePath;

            // 提取文件名
            size_t pos = filePath.find_last_of("\\/");
            string fileName = (pos == string::npos) ? filePath : filePath.substr(pos + 1);

            // 初始化序列号
            uint32_t sequenceNumber = 0;

            // 发送文件名给服务器
            msg.Seq = sequenceNumber;
            msg.Ack = 0;
            msg.Flag = FILENAME;
            msg.Length = static_cast<uint16_t>(fileName.size());
            strcpy_s(msg.Data, fileName.c_str());

            int result = sendWithTimeout(clientSocket, destAddr, msg, sequenceNumber + 1, 1000, 5);
            if (result == -1) {
                cerr << "发送文件名失败，终止文件传输。" << endl;
                continue;
            }

            // 更新序列号
            sequenceNumber++;

            ifstream fileStream(filePath, ios::in | ios::binary);
            if (!fileStream) {
                cerr << "无法打开文件: " << filePath << endl;
                continue;
            }

            // 开始计时
            auto startTime = chrono::high_resolution_clock::now();
            size_t totalBytesSent = 0;

            // 开始发送文件内容
            cout << "开始发送文件..." << endl;
            while (!fileStream.eof()) {
                fileStream.read(msg.Data, BUFFER_SIZE);
                size_t bytesRead = fileStream.gcount();

                if (bytesRead > 0) {
                    msg.Seq = sequenceNumber;
                    msg.Ack = 0;
                    msg.Length = static_cast<uint16_t>(bytesRead);
                    msg.Flag = DATA;

                    result = sendWithTimeout(clientSocket, destAddr, msg, sequenceNumber + 1, 1000, 5);
                    if (result == -1) {
                        cerr << "发送数据包失败，终止文件传输。" << endl;
                        break;
                    }

                    // 更新序列号
                    sequenceNumber++;
                    totalBytesSent += bytesRead;
                }
            }

            fileStream.close();

            // 文件发送完成，发送FIN通知服务器
            msg.Seq = sequenceNumber;
            msg.Ack = 0;
            msg.Flag = FIN;
            msg.Length = 0; // FIN消息不携带数据

            result = sendWithTimeout(clientSocket, serverAddr, msg, 0, 1000, 5);
            if (result == -1) {
                cerr << "发送FIN失败，终止连接。" << endl;
                continue;
            }

            // 结束计时
            auto endTime = chrono::high_resolution_clock::now();
            chrono::duration<double> duration = endTime - startTime;

            // 计算吞吐率
            double throughput = totalBytesSent / duration.count() / 1024; // KB/s

            cout << "文件发送成功。" << endl;
            cout << "传输时间: " << duration.count() << " 秒" << endl;
            cout << "吞吐率: " << throughput << " KB/s" << endl;

            // 循环继续，提示用户选择下一步操作
        }
        else if (choice == 2) {
            // 发送断开连接的 CLOSE 消息
            cout << "正在关闭连接..." << endl;
            msg.Flag = CLOSE;
            msg.Seq = 0;
            msg.Ack = 0;
            msg.Length = 0;

            sendMessage(clientSocket, destAddr, msg);

            // 接收服务器的 ACK 确认
            int bytesReceived = recvfrom(clientSocket, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
                reinterpret_cast<sockaddr*>(&from), &fromSize);

            if (bytesReceived > 0) {
                // 验证校验和
                uint16_t receivedChecksum = msg.Checksum;
                msg.Checksum = 0;
                uint16_t calculatedChecksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));
                if (receivedChecksum != calculatedChecksum) {
                    cerr << "收到的消息校验和不匹配，丢弃数据。" << endl;
                }
                else if (msg.Flag & ACK) {
                    cout << "收到服务器的 ACK 确认。" << endl;

                    // 等待服务器发送 FIN
                    bytesReceived = recvfrom(clientSocket, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
                        reinterpret_cast<sockaddr*>(&from), &fromSize);

                    if (bytesReceived > 0) {
                        // 验证校验和
                        receivedChecksum = msg.Checksum;
                        msg.Checksum = 0;
                        calculatedChecksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));
                        if (receivedChecksum != calculatedChecksum) {
                            cerr << "收到的消息校验和不匹配，丢弃数据。" << endl;
                        }
                        else if (msg.Flag & FIN) {
                            cout << "收到服务器的 FIN，发送 ACK 确认..." << endl;

                            // 发送 ACK 确认
                            Message ackMsg = {};
                            ackMsg.Seq = 0;
                            ackMsg.Ack = msg.Seq + 1;
                            ackMsg.Flag = ACK;

                            sendMessage(clientSocket, serverAddr, ackMsg);

                            cout << "四次挥手完成，连接已关闭。" << endl;
                            connected = false;  // 退出循环，结束程序
                        }
                        else {
                            cerr << "未收到服务器的 FIN 消息。" << endl;
                        }
                    }
                    else {
                        cerr << "未收到服务器的 FIN 消息。" << endl;
                    }
                }
                else {
                    cerr << "未收到服务器的 ACK 确认。" << endl;
                }
            }
            else {
                cerr << "未收到服务器的响应。" << endl;
            }
        }
        else {
            cout << "无效的选择，请重新输入。" << endl;
        }
    }

    closesocket(clientSocket);
    WSACleanup();
    return 0;
}
