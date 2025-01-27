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
#define TIMEOUT_MS 1000 // ��ʱʱ��
#define MAX_RETRANSMISSIONS 5 //����ش�����
#define WINDOW_SIZE 20 // �̶����ڴ�С

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

//����У���
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

//winsock��ʼ��
void initWinsock() {
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        cerr << "WSAStartup failed: " << result << endl;
        exit(1);
    }
}

//�����׽���
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

//�������ݰ�
void sendMessage(SOCKET sock, const sockaddr_in& destAddr, Message& msg) {
    msg.Checksum = 0;
    msg.Checksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));

    int sendResult = sendto(sock, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
        reinterpret_cast<const sockaddr*>(&destAddr), sizeof(destAddr));
    if (sendResult == SOCKET_ERROR) {
        cerr << "��Ϣ����ʧ�ܣ�������: " << WSAGetLastError() << endl;
        closesocket(sock);
        WSACleanup();
        exit(1);
    }

    cout << "�������ݰ� - Seq:" << msg.Seq << " ,Ack: " << msg.Ack
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
    sockaddr_in destAddr = routerAddr; // ʹ��·������ַ��������

    // �������ֽ�������
    Message msg = {};
    msg.Seq = 1;
    msg.Flag = SYN;

    cout << "�������ӷ�����..." << endl;
    sendMessage(clientSocket, destAddr, msg);

    // �ȴ�SYN-ACK
    sockaddr_in from;
    int fromSize = sizeof(from);
    int bytesReceived = recvfrom(clientSocket, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
        reinterpret_cast<sockaddr*>(&from), &fromSize);
    if (bytesReceived > 0 && ((msg.Flag & SYN) && (msg.Flag & ACK))) {
        uint16_t receivedChecksum = msg.Checksum;
        msg.Checksum = 0;
        uint16_t calcChecksum = calculateChecksum(reinterpret_cast<char*>(&msg), sizeof(msg));
        if (receivedChecksum != calcChecksum) {
            cerr << "�յ���SYN-ACKУ��Ͳ�ƥ��" << endl;
            closesocket(clientSocket);
            WSACleanup();
            return 1;
        }

        cout << "�յ�SYN-ACK������ACK..." << endl;
        msg.Flag = ACK;
        sendMessage(clientSocket, destAddr, msg);
        cout << "�������ֳɹ�" << endl;
    }
    else {
        cerr << "���ӽ��������г���" << endl;
        closesocket(clientSocket);
        WSACleanup();
        return 1;
    }

    // ������ѭ�����������ļ�����
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
            uint32_t base = 0; // �������ڵĻ����к�
            uint32_t nextSeq = 0; // ��һ�����͵����к�

            // �����ļ���
            Message filenameMsg = {};
            filenameMsg.Seq = nextSeq;
            filenameMsg.Ack = 0;
            filenameMsg.Flag = FILENAME;
            filenameMsg.Length = static_cast<uint16_t>(fileName.size());
            strncpy_s(filenameMsg.Data, fileName.c_str(), BUFFER_SIZE - 1);

            // ����FILENAME��Ϣ�����뷢�ͻ�����
            map<uint32_t, Packet> sendBuffer;
            Packet pkt;
            pkt.msg = filenameMsg;
            pkt.sentTime = chrono::steady_clock::now();
            pkt.retransmissions = 0;
            sendBuffer[nextSeq] = pkt;

            sendMessage(clientSocket, destAddr, sendBuffer[nextSeq].msg);
            nextSeq++;

            // ���ļ���
            ifstream fileStream(filePath, ios::in | ios::binary);
            if (!fileStream) {
                cerr << "�޷����ļ�: " << filePath << endl;
                // �Ƴ�δ���͵��ļ�����
                sendBuffer.erase(filenameMsg.Seq);
                continue;
            }

            // �����ļ�����
            bool transmissionComplete = false;
            size_t totalBytesSent = 0;
            auto startTime = chrono::high_resolution_clock::now();

            while (!transmissionComplete || !sendBuffer.empty()) {
                // ��们������
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

                // ���ó�ʱʱ��
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(clientSocket, &readfds);
                timeval timeout;
                timeout.tv_sec = 0;
                timeout.tv_usec = 100 * 1000; // 100ms

                int selectResult = select(0, &readfds, NULL, NULL, &timeout);
                if (selectResult > 0 && FD_ISSET(clientSocket, &readfds)) {
                    // ����ACK
                    Message ackMsg = {};
                    int ackBytes = recvfrom(clientSocket, reinterpret_cast<char*>(&ackMsg), sizeof(ackMsg), 0,
                        reinterpret_cast<sockaddr*>(&from), &fromSize);
                    if (ackBytes > 0) {
                        uint16_t recvChecksum = ackMsg.Checksum;
                        ackMsg.Checksum = 0;
                        uint16_t calcChecksum = calculateChecksum(reinterpret_cast<char*>(&ackMsg), sizeof(ackMsg));
                        if (recvChecksum != calcChecksum) {
                            cerr << "�յ���ACKУ��Ͳ�ƥ�䣬�������ݰ�" << endl;
                            continue;
                        }

                        if (ackMsg.Flag & ACK) {
                            cout << "Ack: " << ackMsg.Ack << endl;
                            if (ackMsg.Ack > base) {
                                // �Ƴ���ȷ�ϵİ�
                                for (uint32_t seq = base; seq < ackMsg.Ack; ++seq) {
                                    sendBuffer.erase(seq);
                                }
                                base = ackMsg.Ack;
                            }
                        }
                    }
                }

                // ��鳬ʱ���ش�
                auto currentTime = chrono::steady_clock::now();
                for (auto& [seq, pkt] : sendBuffer) {
                    auto duration = chrono::duration_cast<chrono::milliseconds>(currentTime - pkt.sentTime).count();
                    if (duration > TIMEOUT_MS) {
                        if (pkt.retransmissions < MAX_RETRANSMISSIONS) {
                            cout << "���� " << seq << " ��ʱ�������ش�..." << endl;
                            sendMessage(clientSocket, destAddr, pkt.msg);
                            pkt.sentTime = chrono::steady_clock::now();
                            pkt.retransmissions++;
                        }
                        else {
                            cerr << "���� " << seq << " �ﵽ����ش�����������ʧ�ܡ�" << endl;
                            fileStream.close();
                            // ��շ��ͻ��������ر�����
                            sendBuffer.clear();
                            connected = false;
                            break;
                        }
                    }
                }
            }

            fileStream.close();

            // ����FIN
            Message finMsg = {};
            finMsg.Seq = nextSeq;
            finMsg.Ack = 0;
            finMsg.Flag = FIN;
            finMsg.Length = 0;

            sendMessage(clientSocket, destAddr, finMsg);

            // ������ʱ
            auto endTime = chrono::high_resolution_clock::now();
            chrono::duration<double> duration = endTime - startTime;

            // ����������
            double throughput = totalBytesSent / duration.count() / 1024; // KB/s

            cout << "�ļ����ͳɹ���" << endl;
            cout << "����ʱ��: " << duration.count() << " ��" << endl;
            cout << "������: " << throughput << " KB/s" << endl;

            // �ȴ�FIN-ACK
            bytesReceived = recvfrom(clientSocket, reinterpret_cast<char*>(&msg), sizeof(msg), 0,
                reinterpret_cast<sockaddr*>(&from), &fromSize);
            if (bytesReceived > 0 && (msg.Flag & ACK)) {
                cout << "�յ�ACK���Ĵλ��ֳɹ������ӶϿ���" << endl;
            }

            // �˴����ر����ӣ������û��������������ļ�
            // �û�����ѡ����������ļ���Ͽ�����
        }
        else if (choice == 2) {
            // ���ͶϿ����ӵ�CLOSE��Ϣ
            cout << "���ڹر�����..." << endl;
            Message closeMsg = {};
            closeMsg.Flag = CLOSE;
            closeMsg.Seq = 0;
            closeMsg.Ack = 0;
            closeMsg.Length = 0;

            sendMessage(clientSocket, destAddr, closeMsg);

            // ���շ�������ACKȷ��
            Message ackCloseMsg = {};
            bytesReceived = recvfrom(clientSocket, reinterpret_cast<char*>(&ackCloseMsg), sizeof(ackCloseMsg), 0,
                reinterpret_cast<sockaddr*>(&from), &fromSize);

            if (bytesReceived > 0) {
                // ��֤У���
                uint16_t recvChecksum = ackCloseMsg.Checksum;
                ackCloseMsg.Checksum = 0;
                uint16_t calcChecksum = calculateChecksum(reinterpret_cast<char*>(&ackCloseMsg), sizeof(ackCloseMsg));
                if (recvChecksum != calcChecksum) {
                    cerr << "�յ�����ϢУ��Ͳ�ƥ�䣬�������ݡ�" << endl;
                }
                else if (ackCloseMsg.Flag & ACK) {
                    cout << "�յ���������ACKȷ��" << endl;

                    // �ȴ�����������FIN
                    bytesReceived = recvfrom(clientSocket, reinterpret_cast<char*>(&ackCloseMsg), sizeof(ackCloseMsg), 0,
                        reinterpret_cast<sockaddr*>(&from), &fromSize);

                    if (bytesReceived > 0) {
                        // ��֤У���
                        recvChecksum = ackCloseMsg.Checksum;
                        ackCloseMsg.Checksum = 0;
                        calcChecksum = calculateChecksum(reinterpret_cast<char*>(&ackCloseMsg), sizeof(ackCloseMsg));
                        if (recvChecksum != calcChecksum) {
                            cerr << "�յ�����ϢУ��Ͳ�ƥ�䣬�������ݡ�" << endl;
                        }
                        else if (ackCloseMsg.Flag & FIN) {
                            cout << "�յ��������� FIN������ ACK ȷ��..." << endl;

                            // ����ACKȷ��
                            Message finalAck = {};
                            finalAck.Flag = ACK;
                            finalAck.Seq = 0;
                            finalAck.Ack = ackCloseMsg.Seq + 1;

                            sendMessage(clientSocket, destAddr, finalAck);

                            cout << "�Ĵλ��ֳɹ������ӶϿ�" << endl;
                            system("pause");// �˳�ѭ������������
                        }
                        else {
                            cerr << "δ�յ���������FIN��Ϣ" << endl;
                        }
                    }
                    else {
                        cerr << "δ�յ���������FIN��Ϣ" << endl;
                    }
                }
                else {
                    cerr << "δ�յ���������ACKȷ�ϡ�" << endl;
                }
            }
            else {
                cerr << "δ�յ�����������Ӧ��" << endl;
            }
        }
        else {
            cout<< "��Ч��ѡ�����������롣" << endl;
        }
        closesocket(clientSocket);
        WSACleanup();
        return 0;
    }
}
