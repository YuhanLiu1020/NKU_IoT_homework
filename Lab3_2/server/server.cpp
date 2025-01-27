#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstring>
#include <fstream>
#include <chrono>
#include <map>
#include <iomanip> 
#pragma comment(lib, "Ws2_32.lib")

using namespace std;

#define SERVER_PORT 3333
#define BUFFER_SIZE 1024
#define WINDOW_SIZE 20 // 固定窗口大小

#define ROUTER_IP "127.0.0.1"
#define ROUTER_PORT 2222

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

// 校验和计算函数
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

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return static_cast<uint16_t>(~sum);
}

// 获取当前时间的字符串表示
string getCurrentTime() {
    auto now = chrono::system_clock::now();
    time_t now_time = chrono::system_clock::to_time_t(now);
    struct tm timeInfo;
    localtime_s(&timeInfo, &now_time);
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeInfo);
    return string(buffer);
}

void logMessage(const string& direction, const Message& msg, const string& additionalInfo = "") {
    cout << "[" << getCurrentTime() << "] "
        << direction << " | Seq: " << msg.Seq
        << " | Ack: " << msg.Ack
        << " | Checksum: " << msg.Checksum
        << " | Flags: " << msg.Flag;
    if (!additionalInfo.empty()) {
        cout << " | " << additionalInfo;
    }
    cout << endl;
}

void logWindowStatus(int windowSize, int expectedSeq) {
    cout << "[" << getCurrentTime() << "] "
        << "窗口状态 | 窗口大小: " << windowSize
        << " | 期望序列号: " << expectedSeq
        << endl;
}

void logTransmissionStats(double durationSeconds, size_t totalBytes) {
    double throughput = (totalBytes / 1024.0) / durationSeconds; // KB/s
    cout << "[" << getCurrentTime() << "] "
        << "文件接收完毕，传输时间:" << fixed << setprecision(2) << durationSeconds << " 秒"
        << "吞吐率: " << fixed << setprecision(2) << throughput << " KB/s"
        << endl;
}

void initWinsock() {
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        cerr << "[" << getCurrentTime() << "] Winsock初始化失败，错误码: " << result << endl;
        exit(1);
    }
}

SOCKET createSocket() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        cerr << "[" << getCurrentTime() << "] 套接字创建失败，错误码: " << WSAGetLastError() << endl;
        WSACleanup();
        exit(1);
    }
    return sock;
}

sockaddr_in configureAddress(const char* ip, int port) {
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    return addr;
}

void sendMessage(SOCKET sock, const sockaddr_in& destAddr, Message& msg) {
    msg.Checksum = 0;
    msg.Checksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));

    int sendResult = sendto(sock, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
        reinterpret_cast<const sockaddr*>(&destAddr), sizeof(destAddr));
    if (sendResult == SOCKET_ERROR) {
        cerr << "[" << getCurrentTime() << "] 消息发送失败，错误码: " << WSAGetLastError() << endl;
    }
    else {
        logMessage("发送数据包", msg);
    }
}

void handleMessages(SOCKET serverSocket, sockaddr_in& clientAddr, int& clientAddrSize) {
    Message msg;
    ofstream outputFile;
    string fileName;

    sockaddr_in routerAddr = configureAddress(ROUTER_IP, ROUTER_PORT);
    int routerAddrLen = sizeof(routerAddr);

    sockaddr_in realClientAddr;
    int realClientAddrLen = sizeof(realClientAddr);

    uint32_t expectedSeq = 0;
    size_t totalBytesReceived = 0;
    auto startTime = chrono::high_resolution_clock::now();
    bool timing = false;

    while (true) {
        int bytesReceived = recvfrom(serverSocket, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
            reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrSize);
        if (bytesReceived == SOCKET_ERROR) {
            cerr << "[" << getCurrentTime() << "] 接收数据出错: " << WSAGetLastError() << endl;
            continue;
        }

        // 校验和验证
        uint16_t receivedChecksum = msg.Checksum;
        msg.Checksum = 0;
        uint16_t calculatedChecksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));
        if (receivedChecksum != calculatedChecksum) {
            cerr << "[" << getCurrentTime() << "] 校验和不匹配，丢弃数据包。" << endl;
            continue;
        }

        logMessage("收到数据包", msg);

        // 记录窗口状态
        logWindowStatus(WINDOW_SIZE, expectedSeq);

        if (msg.Flag & SYN) {
            cout << "[" << getCurrentTime() << "] 收到 SYN，正在回复 SYN-ACK..." << endl;

            memcpy(&realClientAddr, &clientAddr, sizeof(sockaddr_in));
            realClientAddrLen = clientAddrSize;

            msg.Ack = msg.Seq + 1;
            msg.Seq = 0;
            msg.Flag = SYN | ACK;

            sendMessage(serverSocket, clientAddr, msg);
            cout << "[" << getCurrentTime() << "] 三次握手成功，等待客户端操作。" << endl;
            continue;
        }
        else if (msg.Flag & FILENAME) {
            if (msg.Seq != expectedSeq) {
                cout << "[" << getCurrentTime() << "]序列号不匹配，期望: " << expectedSeq << "，实际: " << msg.Seq
                    << "，发送ACK重新确认。" << endl;

                Message ackMsg = {};
                ackMsg.Seq = 0;
                ackMsg.Ack = expectedSeq;
                ackMsg.Flag = ACK;

                sendMessage(serverSocket, clientAddr, ackMsg);
                continue;
            }

            fileName = string(msg.Data, msg.Length);
            cout << "[" << getCurrentTime() << "] 接收到文件名: " << fileName << endl;

            string savePath = "E:\\computer_network\\Lab3_2\\save_file\\" + fileName;
            outputFile.open(savePath, ios::binary);
            if (!outputFile) {
                cerr << "[" << getCurrentTime() << "] 无法打开文件进行写入: " << savePath << endl;
                exit(1);
            }

            Message ackMsg = {};
            ackMsg.Seq = 0;
            ackMsg.Ack = msg.Seq + 1;
            ackMsg.Flag = ACK;

            sendMessage(serverSocket, clientAddr, ackMsg);

            expectedSeq++;
            startTime = chrono::high_resolution_clock::now();
            timing = true;
            totalBytesReceived = 0;
            continue;
        }
        else if (msg.Flag & DATA) {
            if (msg.Seq != expectedSeq) {
                cout << "[" << getCurrentTime() << "] 序列号不匹配，期望: " << expectedSeq << "，实际: " << msg.Seq
                    << "，发送ACK重新确认。" << endl;

                Message ackMsg = {};
                ackMsg.Seq = 0;
                ackMsg.Ack = expectedSeq;
                ackMsg.Flag = ACK;

                sendMessage(serverSocket, clientAddr, ackMsg);
                continue;
            }

            if (outputFile.is_open()) {
                outputFile.write(msg.Data, msg.Length);
                totalBytesReceived += msg.Length;

                Message ackMsg = {};
                ackMsg.Seq = 0;
                ackMsg.Ack = msg.Seq + 1;
                ackMsg.Flag = ACK;

                sendMessage(serverSocket, clientAddr, ackMsg);

                expectedSeq++;
            }
            else {
                cerr << "[" << getCurrentTime() << "] 文件未打开，无法写入数据。" << endl;
            }
        }
        else if (msg.Flag & FIN) {
            cout << "[" << getCurrentTime() << "] 收到 FIN，发送 ACK确认..." << endl;

            Message ackMsg = {};
            ackMsg.Seq = 0;
            ackMsg.Ack = msg.Seq + 1;
            ackMsg.Flag = ACK;

            sendMessage(serverSocket, clientAddr, ackMsg);

            if (outputFile.is_open()) {
                outputFile.close();
            }

            if (timing) {
                auto endTime = chrono::high_resolution_clock::now();
                chrono::duration<double> duration = endTime - startTime;
                double durationSeconds = duration.count();

                logTransmissionStats(durationSeconds, totalBytesReceived);

                timing = false;
            }

            cout << getCurrentTime() << "等待下一个文件传输..." << endl;
            expectedSeq = 0;
            continue;
        }
        else if (msg.Flag & CLOSE) {
            cout << getCurrentTime() << "收到客户端的关闭请求，发送 ACK..." << endl;

            Message ackMsg = {};
            ackMsg.Seq = 0;
            ackMsg.Ack = msg.Seq + 1;
            ackMsg.Flag = ACK;

            sendMessage(serverSocket, clientAddr, ackMsg);

            Message finMsg = {};
            finMsg.Seq = 0;
            finMsg.Ack = 0;
            finMsg.Flag = FIN;

            sendMessage(serverSocket, clientAddr, finMsg);
            
            // 等待客户端的 ACK
            int bytesReceivedAck = recvfrom(serverSocket, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
                reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrSize);

            if (bytesReceivedAck > 0) {
                // 验证校验和
                uint16_t recvChecksum = msg.Checksum;
                msg.Checksum = 0;
                uint16_t calcChecksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));
                if (recvChecksum != calcChecksum) {
                    cerr << getCurrentTime() << "收到的消息校验和不匹配，丢弃数据。" << endl;
                }
                else if (msg.Flag & ACK) {
                    cout << getCurrentTime() << "收到客户端的 ACK，连接已关闭。" << endl;
                    break;// 退出循环，关闭套接字
                }
                else {
                    cerr << getCurrentTime() << "未收到客户端的 ACK 确认。" << endl;
                }
            }
            else {
                cerr  << getCurrentTime() << "未收到客户端的响应。" << endl;
            }
        }
        else if (msg.Flag & ACK) {
            logMessage("收到ACK", msg);
            // 可以在这里处理 ACK 消息的逻辑
        }
    }
}

int main() {
    initWinsock();
    SOCKET serverSocket = createSocket();
    sockaddr_in serverAddr = configureAddress("0.0.0.0", SERVER_PORT);
    sockaddr_in clientAddr;
    int clientAddrSize = sizeof(clientAddr);

    if (bind(serverSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        cerr << "[" << getCurrentTime() << "绑定失败: " << WSAGetLastError() << endl;
        closesocket(serverSocket);
        WSACleanup();
        exit(1);
    }

    cout << "[" << getCurrentTime() << "] 服务器正在运行，等待连接..." << endl;
    handleMessages(serverSocket, clientAddr, clientAddrSize);
    cin.get();
    closesocket(serverSocket);
    WSACleanup();
    return 0;
}
