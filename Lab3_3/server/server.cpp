#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstring>
#include <fstream>
#include <chrono>
#include <map>
#include <iomanip> 
#include <mutex>
#include <thread>
#pragma comment(lib, "Ws2_32.lib")

using namespace std;

#define SERVER_PORT 3333
#define BUFFER_SIZE 2048
#define WINDOW_SIZE 1

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
        << "传输统计 | 耗时: " << fixed << setprecision(2) << durationSeconds << " 秒"
        << " | 吞吐率: " << fixed << setprecision(2) << throughput * 10 << " KB/s"
        << endl;
}

void initWinsock() {
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        cerr << "[" << getCurrentTime() << "] [错误] Winsock初始化失败，错误码: " << result << endl;
        exit(1);
    }
}

SOCKET createSocket() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        cerr << "[" << getCurrentTime() << "] [错误] 套接字创建失败，错误码: " << WSAGetLastError() << endl;
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
        cerr << "[" << getCurrentTime() << "] [错误] 消息发送失败，错误码: " << WSAGetLastError() << endl;
    }
    else {
        logMessage("发送数据包", msg);
    }
}

void handleMessages(SOCKET serverSocket, sockaddr_in& clientAddr, int& clientAddrSize) {
    Message msg;
    ofstream outputFile;
    string fileName;

    sockaddr_in realClientAddr;
    int realClientAddrLen = sizeof(realClientAddr);

    uint32_t expectedSeq = 0;
    size_t totalBytesReceived = 0;
    auto startTime = chrono::high_resolution_clock::now();
    bool timing = false;
    bool handshakeComplete = false;

    while (true) {
        int bytesReceived = recvfrom(serverSocket, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
            reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrSize);
        if (bytesReceived == SOCKET_ERROR) {
            cerr << "[" << getCurrentTime() << "] [错误] 接收数据出错: " << WSAGetLastError() << endl;
            continue;
        }

        // 校验和验证
        uint16_t receivedChecksum = msg.Checksum;
        msg.Checksum = 0;
        uint16_t calculatedChecksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));
        if (receivedChecksum != calculatedChecksum) {
            cerr << "[" << getCurrentTime() << "] [警告] 校验和不匹配，丢弃数据包。" << endl;
            continue;
        }

        logMessage("收到数据包", msg);
        logWindowStatus(WINDOW_SIZE, expectedSeq);

        if (!handshakeComplete) {
            // 握手过程
            if (msg.Flag & SYN) {
                cout << "[" << getCurrentTime() << "] [握手阶段] 收到 SYN，回复 SYN-ACK..." << endl;
                memcpy(&realClientAddr, &clientAddr, sizeof(sockaddr_in));
                realClientAddrLen = clientAddrSize;

                // 回复SYN-ACK
                Message synAckMsg = {};
                synAckMsg.Ack = msg.Seq + 1; // Ack = 客户端 Seq + 1
                synAckMsg.Seq = 1;
                synAckMsg.Flag = SYN | ACK;
                sendMessage(serverSocket, clientAddr, synAckMsg);
                continue;
            }
            else if (msg.Flag & ACK) {
                cout << "[" << getCurrentTime() << "] [握手完成] 收到ACK，握手完成。" << endl;
                // expectedSeq根据客户端的Ack设置
                expectedSeq = msg.Ack;
                handshakeComplete = true;
                continue;
            }
            else {
                cerr << "[" << getCurrentTime() << "] [握手阶段] 非预期消息，忽略。" << endl;
                continue;
            }
        }

        // 握手完成后处理数据
        if (msg.Flag & FILENAME) {
            if (msg.Seq != expectedSeq) {
                // 序号不符，发送当前期望序列的ACK
                Message ackMsg = {};
                ackMsg.Ack = expectedSeq;
                ackMsg.Flag = ACK;
                sendMessage(serverSocket, clientAddr, ackMsg);
                continue;
            }

            fileName = string(msg.Data, msg.Length);
            string savePath = "E:\\computer_network\\Lab3_3\\save_file\\" + fileName;
            outputFile.open(savePath, ios::binary);
            if (!outputFile) {
                cerr << "[" << getCurrentTime() << "] [错误] 无法打开文件进行写入: " << savePath << endl;
                exit(1);
            }

            Message ackMsg = {};
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
                // 不匹配序号，多次发送相同的ACK，以触发客户端快速重传
                Message ackMsg = {};
                ackMsg.Ack = expectedSeq;
                ackMsg.Flag = ACK;
                sendMessage(serverSocket, clientAddr, ackMsg);
                continue;
            }

            if (outputFile.is_open()) {
                outputFile.write(msg.Data, msg.Length);
                totalBytesReceived += msg.Length;

                Message ackMsg = {};
                ackMsg.Ack = msg.Seq + 1;
                ackMsg.Flag = ACK;
                sendMessage(serverSocket, clientAddr, ackMsg);

                expectedSeq++;
            }
            else {
                cerr << "[" << getCurrentTime() << "] [错误] 文件未打开，无法写入数据。" << endl;
            }
        }
        else if (msg.Flag & FIN) {
            cout << "[" << getCurrentTime() << "] [连接关闭] 收到 FIN，发送 ACK 确认" << endl;

            // 发送ACK
            Message ackMsg = {};
            ackMsg.Ack = msg.Seq + 1;
            ackMsg.Flag = ACK;
            sendMessage(serverSocket, clientAddr, ackMsg);

            // 文件传输结束
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

            // 发送FIN以关闭连接
            cout << "[" << getCurrentTime() << "] [连接关闭] 发送FIN给客户端" << endl;
            Message finMsg = {};
            finMsg.Flag = FIN;
            sendMessage(serverSocket, clientAddr, finMsg);

            // 尝试等待客户端的最终ACK
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(serverSocket, &readfds);
            timeval timeout;
            timeout.tv_sec = 2; // 等待2秒
            timeout.tv_usec = 0;

            int ret = select(0, &readfds, NULL, NULL, &timeout);
            if (ret > 0) {
                Message finalMsg;
                int bytes = recvfrom(serverSocket, reinterpret_cast<char*>(&finalMsg), sizeof(finalMsg), 0,
                    reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrSize);
                cout << "[" << getCurrentTime() << "] [连接关闭] 收到客户端的最终ACK，连接关闭" << endl;
            }
            else {
                cerr << "[" << getCurrentTime() << "] [警告] 等待客户端ACK超时，关闭连接" << endl;
            }

            break;
        }
        else if (msg.Flag & CLOSE) {
            cout << "[" << getCurrentTime() << "] [状态] 收到CLOSE请求，发送ACK并发送FIN关闭连接" << endl;

            Message ackMsg = {};
            ackMsg.Ack = msg.Seq + 1;
            ackMsg.Flag = ACK;
            sendMessage(serverSocket, clientAddr, ackMsg);

            Message finMsg = {};
            finMsg.Flag = FIN;
            sendMessage(serverSocket, clientAddr, finMsg);

            // 同FIN接收逻辑，等待客户端ACK
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(serverSocket, &readfds);
            timeval timeout;
            timeout.tv_sec = 2;
            timeout.tv_usec = 0;

            int ret = select(0, &readfds, NULL, NULL, &timeout);
            if (ret > 0) {
                Message finalMsg;
                int bytes = recvfrom(serverSocket, reinterpret_cast<char*>(&finalMsg), sizeof(finalMsg), 0,
                    reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrSize);
                cout << "[" << getCurrentTime() << "] [连接关闭] 收到客户端ACK，关闭连接" << endl;
            }
            else {
                cerr << "[" << getCurrentTime() << "] [警告] 等待客户端ACK超时，关闭连接" << endl;
            }

            break;
        }
        else if (msg.Flag & ACK) {
            // 如果是额外的ACK，不需特殊处理
            logMessage("ACK确认", msg);
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
        cerr << "[" << getCurrentTime() << "] [错误] 绑定失败: " << WSAGetLastError() << endl;
        closesocket(serverSocket);
        WSACleanup();
        exit(1);
    }

    cout << "[" << getCurrentTime() << "] [服务器启动] 服务器已启动，等待客户端连接..." << endl;
    handleMessages(serverSocket, clientAddr, clientAddrSize);
    closesocket(serverSocket);
    WSACleanup();
    return 0;
}
