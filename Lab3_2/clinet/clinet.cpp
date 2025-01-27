#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstring>
#include <fstream>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#pragma comment(lib, "Ws2_32.lib")

using namespace std;

#define SERVER_PORT 3333
#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 1024
#define TIMEOUT_MS 1000 // 超时时间
#define MAX_RETRANSMISSIONS 5 //最大重传次数
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

//计算校验和
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

//winsock初始化
void initWinsock() {
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        cerr << "WSAStartup failed: " << result << endl;
        exit(1);
    }
}

//创建套接字
SOCKET createSocket() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        cerr << "Failed to create socket: " << WSAGetLastError() << endl;
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

//发送数据包
void sendMessage(SOCKET sock, const sockaddr_in& destAddr, Message& msg) {
    msg.Checksum = 0;
    msg.Checksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));

    int sendResult = sendto(sock, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
        reinterpret_cast<const sockaddr*>(&destAddr), sizeof(destAddr));
    if (sendResult == SOCKET_ERROR) {
        cerr << "消息发送失败，错误码: " << WSAGetLastError() << endl;
        closesocket(sock);
        WSACleanup();
        exit(1);
    }

    cout << "发送数据包 - Seq:" << msg.Seq << " ,Ack: " << msg.Ack
        << " ,Checksum: " << msg.Checksum << " ,Flags: " << msg.Flag << endl;
}

struct Packet {
    Message msg;
    chrono::steady_clock::time_point sentTime;
    int retransmissions;
};

int main() {
    initWinsock();
    SOCKET clientSocket = createSocket();
    sockaddr_in serverAddr = configureAddress(SERVER_IP, SERVER_PORT);

    sockaddr_in routerAddr = configureAddress(ROUTER_IP, ROUTER_PORT);
    sockaddr_in destAddr = routerAddr; // 使用路由器地址发送数据

    // 三次握手建立连接
    Message msg = {};
    msg.Seq = 1;
    msg.Flag = SYN;

    cout << "尝试连接服务器..." << endl;
    sendMessage(clientSocket, destAddr, msg);

    // 等待SYN-ACK
    sockaddr_in from;
    int fromSize = sizeof(from);
    int bytesReceived = recvfrom(clientSocket, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
        reinterpret_cast<sockaddr*>(&from), &fromSize);
    if (bytesReceived > 0 && ((msg.Flag & SYN) && (msg.Flag & ACK))) {
        uint16_t receivedChecksum = msg.Checksum;
        msg.Checksum = 0;
        uint16_t calcChecksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));
        if (receivedChecksum != calcChecksum) {
            cerr << "收到的SYN-ACK校验和不匹配" << endl;
            closesocket(clientSocket);
            WSACleanup();
            return 1;
        }

        cout << "收到SYN-ACK，发送ACK..." << endl;
        msg.Flag = ACK;
        sendMessage(clientSocket, destAddr, msg);
        cout << "三次握手成功" << endl;
    }
    else {
        cerr << "连接建立过程中出错。" << endl;
        closesocket(clientSocket);
        WSACleanup();
        return 1;
    }

    // 进入主循环，允许多次文件传输
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
            uint32_t base = 0; // 滑动窗口的基序列号
            uint32_t nextSeq = 0; // 下一个发送的序列号

            // 发送文件名
            Message filenameMsg = {};
            filenameMsg.Seq = nextSeq;
            filenameMsg.Ack = 0;
            filenameMsg.Flag = FILENAME;
            filenameMsg.Length = static_cast<uint16_t>(fileName.size());
            strncpy_s(filenameMsg.Data, fileName.c_str(), BUFFER_SIZE - 1);

            // 发送FILENAME消息并加入发送缓冲区
            map<uint32_t, Packet> sendBuffer;
            Packet pkt;
            pkt.msg = filenameMsg;
            pkt.sentTime = chrono::steady_clock::now();
            pkt.retransmissions = 0;
            sendBuffer[nextSeq] = pkt;

            sendMessage(clientSocket, destAddr, sendBuffer[nextSeq].msg);
            nextSeq++;

            // 打开文件流
            ifstream fileStream(filePath, ios::in | ios::binary);
            if (!fileStream) {
                cerr << "无法打开文件: " << filePath << endl;
                // 移除未发送的文件名包
                sendBuffer.erase(filenameMsg.Seq);
                continue;
            }

            // 发送文件内容
            bool transmissionComplete = false;
            size_t totalBytesSent = 0;
            auto startTime = chrono::high_resolution_clock::now();

            while (!transmissionComplete || !sendBuffer.empty()) {
                // 填充滑动窗口
                while ((nextSeq < base + WINDOW_SIZE) && !fileStream.eof() && !transmissionComplete) {
                    Message dataMsg = {};
                    fileStream.read(dataMsg.Data, BUFFER_SIZE);
                    size_t bytesRead = fileStream.gcount();

                    if (bytesRead > 0) {
                        dataMsg.Seq = nextSeq;
                        dataMsg.Ack = 0;
                        dataMsg.Flag = DATA;
                        dataMsg.Length = static_cast<uint16_t>(bytesRead);

                        Packet dataPkt;
                        dataPkt.msg = dataMsg;
                        dataPkt.sentTime = chrono::steady_clock::now();
                        dataPkt.retransmissions = 0;

                        sendBuffer[nextSeq] = dataPkt;
                        sendMessage(clientSocket, destAddr, sendBuffer[nextSeq].msg);
                        nextSeq++;
                        totalBytesSent += bytesRead;
                    }

                    if (fileStream.eof()) {
                        transmissionComplete = true;
                    }
                }

                // 设置超时时间
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(clientSocket, &readfds);
                timeval timeout;
                timeout.tv_sec = 0;
                timeout.tv_usec = 100 * 1000; // 100ms

                int selectResult = select(0, &readfds, NULL, NULL, &timeout);
                if (selectResult > 0 && FD_ISSET(clientSocket, &readfds)) {
                    // 接收ACK
                    Message ackMsg = {};
                    int ackBytes = recvfrom(clientSocket, reinterpret_cast<char*>(&ackMsg), sizeof(ackMsg), 0,
                        reinterpret_cast<sockaddr*>(&from), &fromSize);
                    if (ackBytes > 0) {
                        uint16_t recvChecksum = ackMsg.Checksum;
                        ackMsg.Checksum = 0;
                        uint16_t calcChecksum = calculateChecksum(reinterpret_cast<char*>(&ackMsg), sizeof(ackMsg));
                        if (recvChecksum != calcChecksum) {
                            cerr << "收到的ACK校验和不匹配，丢弃数据包" << endl;
                            continue;
                        }

                        if (ackMsg.Flag & ACK) {
                            cout << "Ack: " << ackMsg.Ack << endl;
                            if (ackMsg.Ack > base) {
                                // 移除已确认的包
                                for (uint32_t seq = base; seq < ackMsg.Ack; ++seq) {
                                    sendBuffer.erase(seq);
                                }
                                base = ackMsg.Ack;
                            }
                        }
                    }
                }

                // 检查超时并重传
                auto currentTime = chrono::steady_clock::now();
                for (auto& [seq, pkt] : sendBuffer) {
                    auto duration = chrono::duration_cast<chrono::milliseconds>(currentTime - pkt.sentTime).count();
                    if (duration > TIMEOUT_MS) {
                        if (pkt.retransmissions < MAX_RETRANSMISSIONS) {
                            cout << "序列 " << seq << " 超时，正在重传..." << endl;
                            sendMessage(clientSocket, destAddr, pkt.msg);
                            pkt.sentTime = chrono::steady_clock::now();
                            pkt.retransmissions++;
                        }
                        else {
                            cerr << "序列 " << seq << " 达到最大重传次数，传输失败。" << endl;
                            fileStream.close();
                            // 清空发送缓冲区并关闭连接
                            sendBuffer.clear();
                            connected = false;
                            break;
                        }
                    }
                }
            }

            fileStream.close();

            // 发送FIN
            Message finMsg = {};
            finMsg.Seq = nextSeq;
            finMsg.Ack = 0;
            finMsg.Flag = FIN;
            finMsg.Length = 0;

            sendMessage(clientSocket, destAddr, finMsg);

            // 结束计时
            auto endTime = chrono::high_resolution_clock::now();
            chrono::duration<double> duration = endTime - startTime;

            // 计算吞吐率
            double throughput = totalBytesSent / duration.count() / 1024; // KB/s

            cout << "文件发送成功。" << endl;
            cout << "传输时间: " << duration.count() << " 秒" << endl;
            cout << "吞吐率: " << throughput << " KB/s" << endl;

            // 等待FIN-ACK
            bytesReceived = recvfrom(clientSocket, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
                reinterpret_cast<sockaddr*>(&from), &fromSize);
            if (bytesReceived > 0 && (msg.Flag & ACK)) {
                cout << "收到ACK，四次挥手成功，连接断开。" << endl;
            }

            // 此处不关闭连接，允许用户继续发送其他文件
            // 用户可以选择继续发送文件或断开连接
        }
        else if (choice == 2) {
            // 发送断开连接的CLOSE消息
            cout << "正在关闭连接..." << endl;
            Message closeMsg = {};
            closeMsg.Flag = CLOSE;
            closeMsg.Seq = 0;
            closeMsg.Ack = 0;
            closeMsg.Length = 0;

            sendMessage(clientSocket, destAddr, closeMsg);

            // 接收服务器的ACK确认
            Message ackCloseMsg = {};
            bytesReceived = recvfrom(clientSocket, reinterpret_cast<char*>(&ackCloseMsg), sizeof(ackCloseMsg), 0,
                reinterpret_cast<sockaddr*>(&from), &fromSize);

            if (bytesReceived > 0) {
                // 验证校验和
                uint16_t recvChecksum = ackCloseMsg.Checksum;
                ackCloseMsg.Checksum = 0;
                uint16_t calcChecksum = calculateChecksum(reinterpret_cast<char*>(&ackCloseMsg), sizeof(ackCloseMsg));
                if (recvChecksum != calcChecksum) {
                    cerr << "收到的消息校验和不匹配，丢弃数据。" << endl;
                }
                else if (ackCloseMsg.Flag & ACK) {
                    cout << "收到服务器的ACK确认" << endl;

                    // 等待服务器发送FIN
                    bytesReceived = recvfrom(clientSocket, reinterpret_cast<char*>(&ackCloseMsg), sizeof(ackCloseMsg), 0,
                        reinterpret_cast<sockaddr*>(&from), &fromSize);

                    if (bytesReceived > 0) {
                        // 验证校验和
                        recvChecksum = ackCloseMsg.Checksum;
                        ackCloseMsg.Checksum = 0;
                        calcChecksum = calculateChecksum(reinterpret_cast<char*>(&ackCloseMsg), sizeof(ackCloseMsg));
                        if (recvChecksum != calcChecksum) {
                            cerr << "收到的消息校验和不匹配，丢弃数据。" << endl;
                        }
                        else if (ackCloseMsg.Flag & FIN) {
                            cout << "收到服务器的 FIN，发送 ACK 确认..." << endl;

                            // 发送ACK确认
                            Message finalAck = {};
                            finalAck.Flag = ACK;
                            finalAck.Seq = 0;
                            finalAck.Ack = ackCloseMsg.Seq + 1;

                            sendMessage(clientSocket, destAddr, finalAck);

                            cout << "四次挥手成功，连接断开" << endl;
                            system("pause");// 退出循环，结束程序
                        }
                        else {
                            cerr << "未收到服务器的FIN消息" << endl;
                        }
                    }
                    else {
                        cerr << "未收到服务器的FIN消息" << endl;
                    }
                }
                else {
                    cerr << "未收到服务器的ACK确认。" << endl;
                }
            }
            else {
                cerr << "未收到服务器的响应。" << endl;
            }
        }
        else {
            cout<< "无效的选择，请重新输入。" << endl;
        }
        closesocket(clientSocket);
        WSACleanup();
        return 0;
    }
}
