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

// У��ͼ��㺯��
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

// ��ȡ��ǰʱ����ַ�����ʾ
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
        << "����״̬ | ���ڴ�С: " << windowSize
        << " | �������к�: " << expectedSeq
        << endl;
}

void logTransmissionStats(double durationSeconds, size_t totalBytes) {
    double throughput = (totalBytes / 1024.0) / durationSeconds; // KB/s
    cout << "[" << getCurrentTime() << "] "
        << "����ͳ�� | ��ʱ: " << fixed << setprecision(2) << durationSeconds << " ��"
        << " | ������: " << fixed << setprecision(2) << throughput * 10 << " KB/s"
        << endl;
}

void initWinsock() {
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        cerr << "[" << getCurrentTime() << "] [����] Winsock��ʼ��ʧ�ܣ�������: " << result << endl;
        exit(1);
    }
}

SOCKET createSocket() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        cerr << "[" << getCurrentTime() << "] [����] �׽��ִ���ʧ�ܣ�������: " << WSAGetLastError() << endl;
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
        cerr << "[" << getCurrentTime() << "] [����] ��Ϣ����ʧ�ܣ�������: " << WSAGetLastError() << endl;
    }
    else {
        logMessage("�������ݰ�", msg);
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
            cerr << "[" << getCurrentTime() << "] [����] �������ݳ���: " << WSAGetLastError() << endl;
            continue;
        }

        // У�����֤
        uint16_t receivedChecksum = msg.Checksum;
        msg.Checksum = 0;
        uint16_t calculatedChecksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));
        if (receivedChecksum != calculatedChecksum) {
            cerr << "[" << getCurrentTime() << "] [����] У��Ͳ�ƥ�䣬�������ݰ���" << endl;
            continue;
        }

        logMessage("�յ����ݰ�", msg);
        logWindowStatus(WINDOW_SIZE, expectedSeq);

        if (!handshakeComplete) {
            // ���ֹ���
            if (msg.Flag & SYN) {
                cout << "[" << getCurrentTime() << "] [���ֽ׶�] �յ� SYN���ظ� SYN-ACK..." << endl;
                memcpy(&realClientAddr, &clientAddr, sizeof(sockaddr_in));
                realClientAddrLen = clientAddrSize;

                // �ظ�SYN-ACK
                Message synAckMsg = {};
                synAckMsg.Ack = msg.Seq + 1; // Ack = �ͻ��� Seq + 1
                synAckMsg.Seq = 1;
                synAckMsg.Flag = SYN | ACK;
                sendMessage(serverSocket, clientAddr, synAckMsg);
                continue;
            }
            else if (msg.Flag & ACK) {
                cout << "[" << getCurrentTime() << "] [�������] �յ�ACK��������ɡ�" << endl;
                // expectedSeq���ݿͻ��˵�Ack����
                expectedSeq = msg.Ack;
                handshakeComplete = true;
                continue;
            }
            else {
                cerr << "[" << getCurrentTime() << "] [���ֽ׶�] ��Ԥ����Ϣ�����ԡ�" << endl;
                continue;
            }
        }

        // ������ɺ�������
        if (msg.Flag & FILENAME) {
            if (msg.Seq != expectedSeq) {
                // ��Ų��������͵�ǰ�������е�ACK
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
                cerr << "[" << getCurrentTime() << "] [����] �޷����ļ�����д��: " << savePath << endl;
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
                // ��ƥ����ţ���η�����ͬ��ACK���Դ����ͻ��˿����ش�
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
                cerr << "[" << getCurrentTime() << "] [����] �ļ�δ�򿪣��޷�д�����ݡ�" << endl;
            }
        }
        else if (msg.Flag & FIN) {
            cout << "[" << getCurrentTime() << "] [���ӹر�] �յ� FIN������ ACK ȷ��" << endl;

            // ����ACK
            Message ackMsg = {};
            ackMsg.Ack = msg.Seq + 1;
            ackMsg.Flag = ACK;
            sendMessage(serverSocket, clientAddr, ackMsg);

            // �ļ��������
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

            // ����FIN�Թر�����
            cout << "[" << getCurrentTime() << "] [���ӹر�] ����FIN���ͻ���" << endl;
            Message finMsg = {};
            finMsg.Flag = FIN;
            sendMessage(serverSocket, clientAddr, finMsg);

            // ���Եȴ��ͻ��˵�����ACK
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(serverSocket, &readfds);
            timeval timeout;
            timeout.tv_sec = 2; // �ȴ�2��
            timeout.tv_usec = 0;

            int ret = select(0, &readfds, NULL, NULL, &timeout);
            if (ret > 0) {
                Message finalMsg;
                int bytes = recvfrom(serverSocket, reinterpret_cast<char*>(&finalMsg), sizeof(finalMsg), 0,
                    reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrSize);
                cout << "[" << getCurrentTime() << "] [���ӹر�] �յ��ͻ��˵�����ACK�����ӹر�" << endl;
            }
            else {
                cerr << "[" << getCurrentTime() << "] [����] �ȴ��ͻ���ACK��ʱ���ر�����" << endl;
            }

            break;
        }
        else if (msg.Flag & CLOSE) {
            cout << "[" << getCurrentTime() << "] [״̬] �յ�CLOSE���󣬷���ACK������FIN�ر�����" << endl;

            Message ackMsg = {};
            ackMsg.Ack = msg.Seq + 1;
            ackMsg.Flag = ACK;
            sendMessage(serverSocket, clientAddr, ackMsg);

            Message finMsg = {};
            finMsg.Flag = FIN;
            sendMessage(serverSocket, clientAddr, finMsg);

            // ͬFIN�����߼����ȴ��ͻ���ACK
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
                cout << "[" << getCurrentTime() << "] [���ӹر�] �յ��ͻ���ACK���ر�����" << endl;
            }
            else {
                cerr << "[" << getCurrentTime() << "] [����] �ȴ��ͻ���ACK��ʱ���ر�����" << endl;
            }

            break;
        }
        else if (msg.Flag & ACK) {
            // ����Ƕ����ACK���������⴦��
            logMessage("ACKȷ��", msg);
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
        cerr << "[" << getCurrentTime() << "] [����] ��ʧ��: " << WSAGetLastError() << endl;
        closesocket(serverSocket);
        WSACleanup();
        exit(1);
    }

    cout << "[" << getCurrentTime() << "] [����������] ���������������ȴ��ͻ�������..." << endl;
    handleMessages(serverSocket, clientAddr, clientAddrSize);
    closesocket(serverSocket);
    WSACleanup();
    return 0;
}
