#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstring>
#include <fstream>
#include <chrono> // 用于计算传输时间
#pragma comment(lib, "Ws2_32.lib")

using namespace std;

#define SERVER_PORT 3333
#define BUFFER_SIZE 1024

#define ROUTER_IP "127.0.0.1"  // 路由器的 IP 地址
#define ROUTER_PORT 2222      // 路由器的端口

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

//// 配置套接字地址
//sockaddr_in configureAddress(int port) {
//    sockaddr_in addr;
//    addr.sin_family = AF_INET;
//    addr.sin_port = htons(port);
//    addr.sin_addr.s_addr = INADDR_ANY;
//    return addr;
//}
// 配置套接字地址
sockaddr_in configureAddress(const char* ip, int port) {
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    return addr;
}

// 发送消息到客户端
void sendMessage(SOCKET sock, const sockaddr_in& destAddr, Message& msg) {
    // 计算校验和
    msg.Checksum = 0; // 在计算前将校验和字段设为 0
    msg.Checksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));

    int sendResult = sendto(sock, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
        reinterpret_cast<const sockaddr*>(&destAddr), sizeof(destAddr));
    if (sendResult == SOCKET_ERROR) {
        cerr << "发送失败，错误码: " << WSAGetLastError() << endl;
    }

    // 日志输出
    cout << "发送数据包 - Seq: " << msg.Seq << ", Ack: " << msg.Ack
        << ", Checksum: " << msg.Checksum << ", Flag: " << msg.Flag << endl;
}

// 处理接收到的消息
void handleMessages(SOCKET serverSocket, sockaddr_in& clientAddr, int& clientAddrSize) {
    Message msg;
    ofstream outputFile;
    string fileName;

    // 配置路由器地址
    sockaddr_in routerAddr = configureAddress(ROUTER_IP, ROUTER_PORT);
    int routerAddrLen = sizeof(routerAddr);

    // 用于保存客户端的真实地址
    sockaddr_in realClientAddr;
    int realClientAddrLen = sizeof(realClientAddr);

    // 初始化期望的序列号
    uint32_t expectedSeq = 0;
    size_t totalBytesReceived = 0;
    auto startTime = chrono::high_resolution_clock::now();
    bool timing = false;

    while (true) {
        int bytesReceived = recvfrom(serverSocket, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
            reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrSize);
        if (bytesReceived == SOCKET_ERROR) {
            cerr << "接收数据时出错: " << WSAGetLastError() << endl;
            continue;
        }

        // 验证校验和
        uint16_t receivedChecksum = msg.Checksum;
        msg.Checksum = 0; // 在计算前将校验和字段设为 0
        uint16_t calculatedChecksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));
        if (receivedChecksum != calculatedChecksum) {
            cerr << "收到的数据包校验和不匹配，丢弃数据包。" << endl;
            continue; // 丢弃该数据包并等待下一个
        }

        // 日志输出
        cout << "收到数据包 - Seq: " << msg.Seq << ", Ack: " << msg.Ack
            << ", Checksum: " << receivedChecksum << ", Flag: " << msg.Flag << endl;

        if (msg.Flag & SYN) {
            cout << "收到 SYN，发送 SYN-ACK..." << endl;

            // 保存客户端的真实地址
            memcpy(&realClientAddr, &clientAddr, sizeof(sockaddr_in));
            realClientAddrLen = clientAddrSize;

            msg.Ack = msg.Seq + 1;
            msg.Seq = 0;
            msg.Flag = SYN | ACK;

            // 计算校验和
            msg.Checksum = 0;
            msg.Checksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));

            sendto(serverSocket, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
                reinterpret_cast<sockaddr*>(&clientAddr), clientAddrSize);
            cout << "三次握手成功，等待操作。" << endl;
            continue;  // 等待客户端发送文件名和数据
        }
        else if (msg.Flag & FILENAME) {
            if (msg.Seq != expectedSeq) {
                // 序列号不匹配，发送当前期望的 ACK
                Message ackMsg = {};
                ackMsg.Seq = 0;
                ackMsg.Ack = expectedSeq;
                ackMsg.Flag = ACK;

                // 计算校验和
                ackMsg.Checksum = 0;
                ackMsg.Checksum = calculateChecksum(reinterpret_cast<char*>(&ackMsg), sizeof(ackMsg));

                sendto(serverSocket, reinterpret_cast<char*>(&ackMsg), sizeof(ackMsg), 0,
                    reinterpret_cast<sockaddr*>(&clientAddr), clientAddrSize);
                continue;
            }

            // 接收到文件名
            fileName = string(msg.Data, msg.Length);
            cout << "接收到文件名：" << fileName << endl;

            // 打开文件流，保存到指定路径
            string savePath = "E:\\computer_network\\Lab3_1\\save_file\\" + fileName;
            outputFile.open(savePath, ios::binary);
            if (!outputFile) {
                cerr << "无法打开文件进行写入：" << savePath << endl;
                exit(1);
            }

            // 发送 ACK 确认
            Message ackMsg = {};
            ackMsg.Seq = 0;
            ackMsg.Ack = msg.Seq + 1;
            ackMsg.Flag = ACK;

            // 计算校验和
            ackMsg.Checksum = 0;
            ackMsg.Checksum = calculateChecksum(reinterpret_cast<char*>(&ackMsg), sizeof(ackMsg));

            sendto(serverSocket, reinterpret_cast<char*>(&ackMsg), sizeof(ackMsg), 0,
                reinterpret_cast<sockaddr*>(&clientAddr), clientAddrSize);

            // 更新期望的序列号
            expectedSeq++;

            // 开始计时
            startTime = chrono::high_resolution_clock::now();
            timing = true;
            totalBytesReceived = 0;
        }
        else if (msg.Flag & DATA) {
            if (msg.Seq != expectedSeq) {
                // 序列号不匹配，发送当前期望的 ACK
                Message ackMsg = {};
                ackMsg.Seq = 0;
                ackMsg.Ack = expectedSeq;
                ackMsg.Flag = ACK;

                // 计算校验和
                ackMsg.Checksum = 0;
                ackMsg.Checksum = calculateChecksum(reinterpret_cast<char*>(&ackMsg), sizeof(ackMsg));

                sendto(serverSocket, reinterpret_cast<char*>(&ackMsg), sizeof(ackMsg), 0,
                    reinterpret_cast<sockaddr*>(&clientAddr), clientAddrSize);
                continue;
            }

            // 确保文件流已经打开
            if (outputFile.is_open()) {
                outputFile.write(msg.Data, msg.Length);
                totalBytesReceived += msg.Length;

                // 发送 ACK 确认
                Message ackMsg = {};
                ackMsg.Seq = 0;
                ackMsg.Ack = msg.Seq + 1;
                ackMsg.Flag = ACK;

                // 计算校验和
                ackMsg.Checksum = 0;
                ackMsg.Checksum = calculateChecksum(reinterpret_cast<char*>(&ackMsg), sizeof(ackMsg));

                sendto(serverSocket, reinterpret_cast<char*>(&ackMsg), sizeof(ackMsg), 0,
                    reinterpret_cast<sockaddr*>(&clientAddr), clientAddrSize);

                // 更新期望的序列号
                expectedSeq++;
            }
            else {
                cerr << "文件流未打开，无法写入数据。" << endl;
            }
        }
        else if (msg.Flag & FIN) {
            cout << "收到 FIN，发送 ACK..." << endl;
            // 发送 ACK 确认
            Message ackMsg = {};
            ackMsg.Seq = 0;
            ackMsg.Ack = msg.Seq + 1;
            ackMsg.Flag = ACK;

            // 计算校验和
            ackMsg.Checksum = 0;
            ackMsg.Checksum = calculateChecksum(reinterpret_cast<char*>(&ackMsg), sizeof(ackMsg));

            sendto(serverSocket, reinterpret_cast<char*>(&ackMsg), sizeof(ackMsg), 0,
                reinterpret_cast<sockaddr*>(&clientAddr), clientAddrSize);

            // 关闭文件流
            if (outputFile.is_open()) {
                outputFile.close();
            }

            // 结束计时
            if (timing) {
                auto endTime = chrono::high_resolution_clock::now();
                chrono::duration<double> duration = endTime - startTime;

                // 计算吞吐率
                double throughput = totalBytesReceived / duration.count() / 1024; // KB/s

                cout << "文件接收完毕，传输时间: " << duration.count() << " 秒" << endl;
                cout << "吞吐率: " << throughput << " KB/s" << endl;

                timing = false;
            }

            cout << "等待下一个文件传输..." << endl;

            // 重置期望的序列号
            expectedSeq = 0;
        }
        else if (msg.Flag & CLOSE) {
            cout << "收到客户端的关闭请求，发送 ACK..." << endl;
            // 发送 ACK 确认
            Message ackMsg = {};
            ackMsg.Seq = 0;
            ackMsg.Ack = msg.Seq + 1;
            ackMsg.Flag = ACK;

            // 计算校验和
            ackMsg.Checksum = 0;
            ackMsg.Checksum = calculateChecksum(reinterpret_cast<char*>(&ackMsg), sizeof(ackMsg));

            sendto(serverSocket, reinterpret_cast<char*>(&ackMsg), sizeof(ackMsg), 0,
                reinterpret_cast<sockaddr*>(&clientAddr), clientAddrSize);

            // 发送 FIN
            Message finMsg = {};
            finMsg.Seq = 0;
            finMsg.Ack = 0;
            finMsg.Flag = FIN;

            // 计算校验和
            finMsg.Checksum = 0;
            finMsg.Checksum = calculateChecksum(reinterpret_cast<char*>(&finMsg), sizeof(finMsg));

            sendto(serverSocket, reinterpret_cast<char*>(&finMsg), sizeof(finMsg), 0,
                reinterpret_cast<sockaddr*>(&clientAddr), clientAddrSize);

            // 等待客户端的 ACK
            int bytesReceived = recvfrom(serverSocket, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
                reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrSize);

            if (bytesReceived > 0) {
                // 验证校验和
                receivedChecksum = msg.Checksum;
                msg.Checksum = 0;
                calculatedChecksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));
                if (receivedChecksum != calculatedChecksum) {
                    cerr << "收到的消息校验和不匹配，丢弃数据。" << endl;
                }
                else if (msg.Flag & ACK) {
                    cout << "收到客户端的 ACK，连接已关闭。" << endl;
                    break; // 退出循环，关闭套接字
                }
                else {
                    cerr << "未收到客户端的 ACK 确认。" << endl;
                }
            }
            else {
                cerr << "未收到客户端的响应。" << endl;
            }
        }
        else if (msg.Flag & ACK) {
            cout << "收到 ACK。" << endl;
            // 可以在这里处理 ACK 消息的逻辑
        }
    }
}

int main() {
    initWinsock();
    SOCKET serverSocket = createSocket();
    // 配置服务器地址
    sockaddr_in serverAddr = configureAddress("0.0.0.0", SERVER_PORT);
    sockaddr_in clientAddr;
    int clientAddrSize = sizeof(clientAddr);


    if (bind(serverSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        cerr << "绑定失败: " << WSAGetLastError() << endl;
        closesocket(serverSocket);
        WSACleanup();
        exit(1);
    }

    cout << "服务器正在运行，等待连接..." << endl;
    handleMessages(serverSocket, clientAddr, clientAddrSize);

    closesocket(serverSocket);
    WSACleanup();
    return 0;
}
