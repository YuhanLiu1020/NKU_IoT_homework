#include <iostream> 
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstring>
#include <fstream>
#include <chrono>
#include <map>
#include <cmath>
#pragma comment(lib, "Ws2_32.lib")

using namespace std;

#define SERVER_PORT 3333
#define BUFFER_SIZE 2048
#define TIMEOUT_MS 1000            // 超时时间（毫秒）
#define MAX_RETRANSMISSIONS 5      // 最大重传次数

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

struct Packet {
    Message msg;
    chrono::steady_clock::time_point sentTime;
    int retransmissions;
};

// 全局变量
map<uint32_t, Packet> sendBuffer;
uint32_t baseSeq = 0;
uint32_t nextSeq = 0;
bool connected = true;
bool transmissionComplete = false;

double cwnd = 1.0;            // 拥塞窗口，初始值为1
double ssthresh = 16.0;       // 慢启动阈值，初始值为16
int dupACKcount = 0;          // 重复ACK计数
bool fastRecovery = false;    // 是否处于快速恢复阶段
uint32_t lastAck = 0;         // 最后确认的ACK号

SOCKET clientSocketGlobal;
sockaddr_in destAddrGlobal;

void sendMessage(const Message& msg) {
    Message msgCopy = msg;
    msgCopy.Checksum = 0;
    msgCopy.Checksum = calculateChecksum(reinterpret_cast<const char*>(&msgCopy), sizeof(msgCopy));

    int sendResult = sendto(clientSocketGlobal, reinterpret_cast<char*>(&msgCopy), sizeof(msgCopy), 0,
        reinterpret_cast<const sockaddr*>(&destAddrGlobal), sizeof(destAddrGlobal));
    if (sendResult == SOCKET_ERROR) {
        cerr << "消息发送失败，错误码: " << WSAGetLastError() << endl;
        closesocket(clientSocketGlobal);
        WSACleanup();
        exit(1);
    }

    // 修改后的输出信息
    cout << "数据包发送完毕， Seq: " << msgCopy.Seq << " | Ack: " << msgCopy.Ack
        << " | Checksum: " << msgCopy.Checksum << " | Flags: " << msgCopy.Flag << endl;
}

void adjustForTimeout(uint32_t seq) {
    cout << "超时！ 序列号 " << seq << " 超时，重传..." << endl;
    // 超时调整拥塞控制
    ssthresh = cwnd / 2;
    if (ssthresh < 1.0) ssthresh = 1.0;
    cwnd = 1.0;
    dupACKcount = 0;
    fastRecovery = false;
    cout << "超时处理： ssthresh = " << ssthresh << "，cwnd 重置为 " << cwnd << endl;
}

// 接收ACK并更新状态的函数（非阻塞或带超时）
bool receiveACK() {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(clientSocketGlobal, &readfds);

    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 1000 * 100; // 等待100ms一轮，以便可多次尝试

    int ret = select(0, &readfds, NULL, NULL, &tv);
    if (ret > 0 && FD_ISSET(clientSocketGlobal, &readfds)) {
        sockaddr_in from;
        int fromSize = sizeof(from);
        Message ackMsg = {};
        int ackBytes = recvfrom(clientSocketGlobal, reinterpret_cast<char*>(&ackMsg), sizeof(ackMsg), 0,
            reinterpret_cast<sockaddr*>(&from), &fromSize);
        if (ackBytes > 0) {
            uint16_t recvChecksum = ackMsg.Checksum;
            ackMsg.Checksum = 0;
            uint16_t calcChecksum = calculateChecksum(reinterpret_cast<char*>(&ackMsg), sizeof(ackMsg));
            if (recvChecksum != calcChecksum) {
                cerr << "警告！ 收到的ACK校验和不匹配，丢弃数据包" << endl;
                return true; // 收到坏包，继续下一轮
            }

            if (ackMsg.Flag & ACK) {
                if (ackMsg.Ack > lastAck) {
                    // 新的ACK
                    dupACKcount = 0;
                    if (fastRecovery) {
                        cout << "快速恢复： 收到新ACK，退出快速恢复阶段" << endl;
                        fastRecovery = false;
                        cwnd = ssthresh;
                    }
                    else {
                        // 慢启动或拥塞避免
                        if (cwnd < ssthresh) {
                            cwnd += 1.0;
                            cout << "慢启动： cwnd 增至 " << cwnd << endl;
                        }
                        else {
                            cwnd += 1.0 / cwnd;
                            cout << "拥塞避免： cwnd 增至 " << cwnd << endl;
                        }
                    }

                    // 移除已确认的包
                    for (uint32_t seq = baseSeq; seq < ackMsg.Ack; ++seq) {
                        sendBuffer.erase(seq);
                    }
                    baseSeq = ackMsg.Ack;
                    lastAck = ackMsg.Ack;
                }
                else if (ackMsg.Ack == lastAck) {
                    // 重复ACK
                    dupACKcount++;
                    cout << "收到重复ACK，count: " << dupACKcount << endl;

                    if (dupACKcount == 3 && !fastRecovery) {
                        // 快速重传
                        auto it = sendBuffer.find(ackMsg.Ack);
                        if (it != sendBuffer.end()) {
                            cout << "快速重传： 序列号 " << ackMsg.Ack << " 重传" << endl;
                            sendMessage(it->second.msg);
                            it->second.sentTime = chrono::steady_clock::now();
                        }
                        ssthresh = cwnd / 2;
                        if (ssthresh < 1.0) ssthresh = 1.0;
                        cwnd = ssthresh + 3;
                        fastRecovery = true;
                        cout << "快速重传： 进入快速恢复阶段, ssthresh = " << ssthresh << ", cwnd = " << cwnd << endl;
                    }
                    else if (dupACKcount > 3 && fastRecovery) {
                        cwnd += 1.0;
                        cout << "快速恢复： 重复ACK增大，cwnd = " << cwnd << endl;
                    }
                }
            }

            if (ackMsg.Flag & FIN) {
                cout << "收到服务器的FIN，发送ACK确认..." << endl;
                Message finalAck = {};
                finalAck.Flag = ACK;
                finalAck.Seq = 0;
                finalAck.Ack = ackMsg.Seq + 1;
                sendMessage(finalAck);
                cout << "四次挥手成功，连接断开" << endl;
                connected = false;
            }

            if (ackMsg.Flag & CLOSE) {
                cout << "收到服务器的关闭请求，关闭连接" << endl;
                connected = false;
            }
        }
    }
    return true;
}

// 超时检测并重传超时的数据包
void checkTimeouts() {
    auto currentTime = chrono::steady_clock::now();
    for (auto& [seq, pkt] : sendBuffer) {
        auto duration = chrono::duration_cast<chrono::milliseconds>(currentTime - pkt.sentTime).count();
        if (duration > TIMEOUT_MS) {
            if (pkt.retransmissions < MAX_RETRANSMISSIONS) {
                adjustForTimeout(seq);
                sendMessage(pkt.msg);
                pkt.sentTime = chrono::steady_clock::now();
                pkt.retransmissions++;
            }
            else {
                cerr << "序列号 " << seq << " 达到最大重传次数，传输失败。" << endl;
                connected = false;
                break;
            }
        }
    }
}

void initWinsockFunc() {
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        cerr << "Winsock初始化失败，错误码: " << result << endl;
        exit(1);
    }
}

SOCKET createSocketFunc() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        cerr << "套接字创建失败，错误码: " << WSAGetLastError() << endl;
        WSACleanup();
        exit(1);
    }
    return sock;
}

sockaddr_in configureAddressFunc(const char* ip, int port) {
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    return addr;
}

int main() {
    initWinsockFunc();
    clientSocketGlobal = createSocketFunc();
    sockaddr_in routerAddr = configureAddressFunc(ROUTER_IP, ROUTER_PORT);
    destAddrGlobal = routerAddr;

    // 三次握手
    Message synMsg = {};
    synMsg.Seq = 1;
    synMsg.Flag = SYN;
    cout << "尝试连接服务器..." << endl;
    sendMessage(synMsg);

    {
        // 等待SYN-ACK
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(clientSocketGlobal, &readfds);
        timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        int ret = select(0, &readfds, NULL, NULL, &tv);
        if (ret <= 0) {
            cerr << "等待SYN-ACK超时或出错" << endl;
            closesocket(clientSocketGlobal);
            WSACleanup();
            return 1;
        }

        sockaddr_in from;
        int fromSize = sizeof(from);
        Message recvMsg = {};
        int bytesReceived = recvfrom(clientSocketGlobal, reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg), 0,
            reinterpret_cast<sockaddr*>(&from), &fromSize);

        if (bytesReceived > 0 && ((recvMsg.Flag & SYN) && (recvMsg.Flag & ACK))) {
            uint16_t receivedChecksum = recvMsg.Checksum;
            recvMsg.Checksum = 0;
            uint16_t calcChecksum = calculateChecksum(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
            if (receivedChecksum != calcChecksum) {
                cerr << "收到的SYN-ACK校验和不匹配" << endl;
                closesocket(clientSocketGlobal);
                WSACleanup();
                return 1;
            }

            cout << "收到SYN-ACK, 发送ACK..." << endl;
            Message ackMsg = {};
            ackMsg.Flag = ACK;
            ackMsg.Seq = 0;
            ackMsg.Ack = recvMsg.Seq + 1;
            sendMessage(ackMsg);
            baseSeq = ackMsg.Ack;
            nextSeq = baseSeq;
            lastAck = baseSeq;
            cout << "三次握手成功，连接建立" << endl;
        }
        else {
            cerr << "未收到有效的SYN-ACK" << endl;
            closesocket(clientSocketGlobal);
            WSACleanup();
            return 1;
        }
    }

    // 开始发送文件
    cout << "请输入文件路径：" << endl;
    string filePath;
    cin >> filePath;
    size_t pos = filePath.find_last_of("\\/");
    string fileName = (pos == string::npos) ? filePath : filePath.substr(pos + 1);

    // 发送文件名
    {
        Message filenameMsg = {};
        filenameMsg.Seq = nextSeq;
        filenameMsg.Flag = FILENAME;
        filenameMsg.Length = (uint16_t)fileName.size();
        strncpy_s(filenameMsg.Data, fileName.c_str(), BUFFER_SIZE - 1);

        Packet pkt;
        pkt.msg = filenameMsg;
        pkt.sentTime = chrono::steady_clock::now();
        pkt.retransmissions = 0;
        sendBuffer[nextSeq] = pkt;

        sendMessage(filenameMsg);
        nextSeq++;
    }

    ifstream fileStream(filePath, ios::in | ios::binary);
    if (!fileStream) {
        cerr << "无法打开文件: " << filePath << endl;
        connected = false;
    }

    bool fileSent = false;

    // 新增变量用于记录传输时间和吞吐率
    chrono::steady_clock::time_point startTime;
    chrono::steady_clock::time_point endTime;
    uint64_t totalBytesSent = 0; // 总发送字节数

    // 记录传输开始时间
    startTime = chrono::steady_clock::now();

    while (connected) {
        // 尝试发送数据包（根据cwnd决定可发多少）
        while (connected && !fileSent && (sendBuffer.size() < (size_t)ceil(cwnd))) {
            Message dataMsg = {};
            fileStream.read(dataMsg.Data, BUFFER_SIZE);
            size_t bytesRead = fileStream.gcount();

            if (bytesRead > 0) {
                dataMsg.Seq = nextSeq;
                dataMsg.Flag = DATA;
                dataMsg.Length = (uint16_t)bytesRead;

                Packet dataPkt;
                dataPkt.msg = dataMsg;
                dataPkt.sentTime = chrono::steady_clock::now();
                dataPkt.retransmissions = 0;
                sendBuffer[nextSeq] = dataPkt;

                sendMessage(dataMsg);
                nextSeq++;

                // 累计发送的字节数
                totalBytesSent += bytesRead;
            }
            else {
                // 文件读取完毕，发送FIN
                fileSent = true;
                break;
            }
        }

        if (fileSent && sendBuffer.empty()) {
            // 数据全部确认后发送FIN
            Message finMsg = {};
            finMsg.Seq = nextSeq;
            finMsg.Flag = FIN;
            Packet finPkt;
            finPkt.msg = finMsg;
            finPkt.sentTime = chrono::steady_clock::now();
            finPkt.retransmissions = 0;
            sendBuffer[nextSeq] = finPkt;
            sendMessage(finMsg);
            nextSeq++;
        }

        // 接收ACK
        receiveACK();

        // 检查超时并重传
        checkTimeouts();

        // 如果fin已发送且buffer为空且未收到服务器的FIN，则稍等一会再继续循环
        if (fileSent && sendBuffer.empty() && connected) {
            // 文件传完，等对方FIN或CLOSE
            Sleep(500);
        }
    }

    // 记录传输结束时间
    endTime = chrono::steady_clock::now();

    // 计算传输时间（秒）
    chrono::duration<double> elapsed = endTime - startTime;
    double transmissionTime = elapsed.count();

    // 计算吞吐率（字节/秒）
    double throughput = (double)totalBytesSent / transmissionTime;

    // 输出传输时间和吞吐率
    cout << "传输时间: " << transmissionTime << " 秒" << endl;
    cout << "吞吐率: " << throughput << " 字节/秒" << endl;

    // 清理和退出
    closesocket(clientSocketGlobal);
    WSACleanup();
    cout << "客户端已结束连接并退出。" << endl;
    return 0;
}
