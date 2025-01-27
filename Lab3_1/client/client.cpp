#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstring>
#include <fstream>
#include <chrono> // ���ڼ��㴫��ʱ��
#pragma comment(lib, "Ws2_32.lib")

using namespace std;

#define SERVER_PORT 3333
#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 1024

#define ROUTER_IP "127.0.0.1" // ·������ IP ��ַ
#define ROUTER_PORT 2222     // ·�����Ķ˿�

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

// ����У��ͺ���
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

    // �� 32 λ�� sum ת��Ϊ 16 λ
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return static_cast<uint16_t>(~sum);
}

// ��ʼ�� Winsock
void initWinsock() {
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        cerr << "WSAStartup failed: " << result << endl;
        exit(1);
    }
}

// ���� UDP �׽���
SOCKET createSocket() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        cerr << "Failed to create socket: " << WSAGetLastError() << endl;
        WSACleanup();
        exit(1);
    }
    return sock;
}

// �����׽��ֵ�ַ
sockaddr_in configureAddress(const char* ip, int port) {
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    return addr;
}

// ������Ϣ��������
void sendMessage(SOCKET sock, const sockaddr_in& destAddr, Message& msg) {
    // ����У���
    msg.Checksum = 0; // �ڼ���ǰ��У����ֶ���Ϊ 0
    msg.Checksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));

    int sendResult = sendto(sock, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
        reinterpret_cast<const sockaddr*>(&destAddr), sizeof(destAddr));
    if (sendResult == SOCKET_ERROR) {
        cerr << "����ʧ�ܣ�������: " << WSAGetLastError() << endl;
        closesocket(sock);
        WSACleanup();
        exit(1);
    }

    // ��־���
    cout << "�������ݰ� - Seq: " << msg.Seq << ", Ack: " << msg.Ack
        << ", Checksum: " << msg.Checksum << ", Flag: " << msg.Flag << endl;
}

// ����ʱ�ش��ķ��ͺ���
int sendWithTimeout(SOCKET sock, const sockaddr_in& destAddr, Message& msg, uint32_t expectedAck, int timeoutMs, int maxRetransmissions) {
    int retransmissions = 0;
    fd_set readfds;
    timeval timeout;
    sockaddr_in from;
    int fromSize = sizeof(from);
    Message recvMsg;

    while (retransmissions < maxRetransmissions) {
        // ������Ϣ
        sendMessage(sock, destAddr, msg);

        // ���ó�ʱʱ��
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;

        // �ȴ�ACK
        int selectResult = select(0, &readfds, NULL, NULL, &timeout);
        if (selectResult > 0) {
            // ����ACK
            int bytesReceived = recvfrom(sock, reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg), 0,
                reinterpret_cast<sockaddr*>(&from), &fromSize);

            if (bytesReceived > 0) {
                // ��֤У���
                uint16_t receivedChecksum = recvMsg.Checksum;
                recvMsg.Checksum = 0;
                uint16_t calculatedChecksum = calculateChecksum(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));
                if (receivedChecksum != calculatedChecksum) {
                    cerr << "�յ���ACKУ��Ͳ�ƥ�䣬�������ݡ�" << endl;
                    continue;
                }

                if ((recvMsg.Flag & ACK) && recvMsg.Ack == msg.Seq + 1) {
                    // �յ���ȷ��ACK

                    // ��־���
                    cout << "�յ�ACK - Seq: " << recvMsg.Seq << ", Ack: " << recvMsg.Ack
                        << ", Checksum: " << receivedChecksum << endl;

                    return 0;
                }
            }
        }
        else if (selectResult == 0) {
            // ��ʱ���ش�
            retransmissions++;
            cout << "��ʱ�������ش����ݰ� (" << retransmissions << "/" << maxRetransmissions << ")" << endl;
        }
        else {
            // ��������
            cerr << "Select��������: " << WSAGetLastError() << endl;
            return -1;
        }
    }

    // ��������ش�����
    cerr << "��������ش�������δ�յ�ACKȷ�ϡ�" << endl;
    return -1;
}

int main() {
    initWinsock();
    SOCKET clientSocket = createSocket();
    // ���÷�������ַ
    sockaddr_in serverAddr = configureAddress(SERVER_IP, SERVER_PORT);

    // ����·������ַ
    sockaddr_in routerAddr = configureAddress(ROUTER_IP, ROUTER_PORT);
    int routerAddrLen = sizeof(routerAddr);

    // �����Ҫͨ��·�����������ݣ��� destAddr ����Ϊ routerAddr
    sockaddr_in destAddr = routerAddr; // ����ֱ��ʹ�� routerAddr

    // ��ʼ���ӽ������������֣�
    Message msg = {};
    msg.Seq = 1;  // ��ʼ���к�
    msg.Flag = SYN;

    // ���ó�ʱ������ش�����
    int timeoutMs = 1000;  // ��ʱʱ�䣺1��
    int maxRetransmissions = 5;  // ����ش�������3��

    cout << "�������ӷ�����..." << endl;
    sendMessage(clientSocket, destAddr, msg);

    // �ȴ��������� SYN-ACK
    sockaddr_in from;
    int fromSize = sizeof(from);
    int bytesReceived = recvfrom(clientSocket, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
        reinterpret_cast<sockaddr*>(&from), &fromSize);
    if (bytesReceived > 0 && (msg.Flag & (SYN | ACK))) {
        // ��֤У���
        uint16_t receivedChecksum = msg.Checksum;
        msg.Checksum = 0;
        uint16_t calculatedChecksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));
        if (receivedChecksum != calculatedChecksum) {
            cerr << "�յ���SYN-ACKУ��Ͳ�ƥ�䡣" << endl;
            closesocket(clientSocket);
            WSACleanup();
            return 1;
        }

        cout << "�յ� SYN-ACK������ ACK..." << endl;
        msg.Flag = ACK;
        sendMessage(clientSocket, destAddr, msg);
        cout << "�������ֳɹ���" << endl;
    }
    else {
        cerr << "���ӽ��������г���" << endl;
        closesocket(clientSocket);
        WSACleanup();
        return 1;
    }

    bool connected = true;
    while (connected) {
        cout << "��ѡ�����: 1 - �����ļ�, 2 - �Ͽ�����" << endl;
        int choice;
        cin >> choice;

        if (choice == 1) {
            cout << "�������ļ�·����" << endl;
            string filePath;
            cin >> filePath;

            // ��ȡ�ļ���
            size_t pos = filePath.find_last_of("\\/");
            string fileName = (pos == string::npos) ? filePath : filePath.substr(pos + 1);

            // ��ʼ�����к�
            uint32_t sequenceNumber = 0;

            // �����ļ�����������
            msg.Seq = sequenceNumber;
            msg.Ack = 0;
            msg.Flag = FILENAME;
            msg.Length = static_cast<uint16_t>(fileName.size());
            strcpy_s(msg.Data, fileName.c_str());

            int result = sendWithTimeout(clientSocket, destAddr, msg, sequenceNumber + 1, 1000, 5);
            if (result == -1) {
                cerr << "�����ļ���ʧ�ܣ���ֹ�ļ����䡣" << endl;
                continue;
            }

            // �������к�
            sequenceNumber++;

            ifstream fileStream(filePath, ios::in | ios::binary);
            if (!fileStream) {
                cerr << "�޷����ļ�: " << filePath << endl;
                continue;
            }

            // ��ʼ��ʱ
            auto startTime = chrono::high_resolution_clock::now();
            size_t totalBytesSent = 0;

            // ��ʼ�����ļ�����
            cout << "��ʼ�����ļ�..." << endl;
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
                        cerr << "�������ݰ�ʧ�ܣ���ֹ�ļ����䡣" << endl;
                        break;
                    }

                    // �������к�
                    sequenceNumber++;
                    totalBytesSent += bytesRead;
                }
            }

            fileStream.close();

            // �ļ�������ɣ�����FIN֪ͨ������
            msg.Seq = sequenceNumber;
            msg.Ack = 0;
            msg.Flag = FIN;
            msg.Length = 0; // FIN��Ϣ��Я������

            result = sendWithTimeout(clientSocket, serverAddr, msg, 0, 1000, 5);
            if (result == -1) {
                cerr << "����FINʧ�ܣ���ֹ���ӡ�" << endl;
                continue;
            }

            // ������ʱ
            auto endTime = chrono::high_resolution_clock::now();
            chrono::duration<double> duration = endTime - startTime;

            // ����������
            double throughput = totalBytesSent / duration.count() / 1024; // KB/s

            cout << "�ļ����ͳɹ���" << endl;
            cout << "����ʱ��: " << duration.count() << " ��" << endl;
            cout << "������: " << throughput << " KB/s" << endl;

            // ѭ����������ʾ�û�ѡ����һ������
        }
        else if (choice == 2) {
            // ���ͶϿ����ӵ� CLOSE ��Ϣ
            cout << "���ڹر�����..." << endl;
            msg.Flag = CLOSE;
            msg.Seq = 0;
            msg.Ack = 0;
            msg.Length = 0;

            sendMessage(clientSocket, destAddr, msg);

            // ���շ������� ACK ȷ��
            int bytesReceived = recvfrom(clientSocket, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
                reinterpret_cast<sockaddr*>(&from), &fromSize);

            if (bytesReceived > 0) {
                // ��֤У���
                uint16_t receivedChecksum = msg.Checksum;
                msg.Checksum = 0;
                uint16_t calculatedChecksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));
                if (receivedChecksum != calculatedChecksum) {
                    cerr << "�յ�����ϢУ��Ͳ�ƥ�䣬�������ݡ�" << endl;
                }
                else if (msg.Flag & ACK) {
                    cout << "�յ��������� ACK ȷ�ϡ�" << endl;

                    // �ȴ����������� FIN
                    bytesReceived = recvfrom(clientSocket, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
                        reinterpret_cast<sockaddr*>(&from), &fromSize);

                    if (bytesReceived > 0) {
                        // ��֤У���
                        receivedChecksum = msg.Checksum;
                        msg.Checksum = 0;
                        calculatedChecksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));
                        if (receivedChecksum != calculatedChecksum) {
                            cerr << "�յ�����ϢУ��Ͳ�ƥ�䣬�������ݡ�" << endl;
                        }
                        else if (msg.Flag & FIN) {
                            cout << "�յ��������� FIN������ ACK ȷ��..." << endl;

                            // ���� ACK ȷ��
                            Message ackMsg = {};
                            ackMsg.Seq = 0;
                            ackMsg.Ack = msg.Seq + 1;
                            ackMsg.Flag = ACK;

                            sendMessage(clientSocket, serverAddr, ackMsg);

                            cout << "�Ĵλ�����ɣ������ѹرա�" << endl;
                            connected = false;  // �˳�ѭ������������
                        }
                        else {
                            cerr << "δ�յ��������� FIN ��Ϣ��" << endl;
                        }
                    }
                    else {
                        cerr << "δ�յ��������� FIN ��Ϣ��" << endl;
                    }
                }
                else {
                    cerr << "δ�յ��������� ACK ȷ�ϡ�" << endl;
                }
            }
            else {
                cerr << "δ�յ�����������Ӧ��" << endl;
            }
        }
        else {
            cout << "��Ч��ѡ�����������롣" << endl;
        }
    }

    closesocket(clientSocket);
    WSACleanup();
    return 0;
}
