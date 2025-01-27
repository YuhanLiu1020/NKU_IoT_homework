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
#define TIMEOUT_MS 1000            // ��ʱʱ�䣨���룩
#define MAX_RETRANSMISSIONS 5      // ����ش�����

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

// ȫ�ֱ���
map<uint32_t, Packet> sendBuffer;
uint32_t baseSeq = 0;
uint32_t nextSeq = 0;
bool connected = true;
bool transmissionComplete = false;

double cwnd = 1.0;            // ӵ�����ڣ���ʼֵΪ1
double ssthresh = 16.0;       // ��������ֵ����ʼֵΪ16
int dupACKcount = 0;          // �ظ�ACK����
bool fastRecovery = false;    // �Ƿ��ڿ��ٻָ��׶�
uint32_t lastAck = 0;         // ���ȷ�ϵ�ACK��

SOCKET clientSocketGlobal;
sockaddr_in destAddrGlobal;

void sendMessage(const Message& msg) {
    Message msgCopy = msg;
    msgCopy.Checksum = 0;
    msgCopy.Checksum = calculateChecksum(reinterpret_cast<const char*>(&msgCopy), sizeof(msgCopy));

    int sendResult = sendto(clientSocketGlobal, reinterpret_cast<char*>(&msgCopy), sizeof(msgCopy), 0,
        reinterpret_cast<const sockaddr*>(&destAddrGlobal), sizeof(destAddrGlobal));
    if (sendResult == SOCKET_ERROR) {
        cerr << "��Ϣ����ʧ�ܣ�������: " << WSAGetLastError() << endl;
        closesocket(clientSocketGlobal);
        WSACleanup();
        exit(1);
    }

    // �޸ĺ�������Ϣ
    cout << "���ݰ�������ϣ� Seq: " << msgCopy.Seq << " | Ack: " << msgCopy.Ack
        << " | Checksum: " << msgCopy.Checksum << " | Flags: " << msgCopy.Flag << endl;
}

void adjustForTimeout(uint32_t seq) {
    cout << "��ʱ�� ���к� " << seq << " ��ʱ���ش�..." << endl;
    // ��ʱ����ӵ������
    ssthresh = cwnd / 2;
    if (ssthresh < 1.0) ssthresh = 1.0;
    cwnd = 1.0;
    dupACKcount = 0;
    fastRecovery = false;
    cout << "��ʱ���� ssthresh = " << ssthresh << "��cwnd ����Ϊ " << cwnd << endl;
}

// ����ACK������״̬�ĺ����������������ʱ��
bool receiveACK() {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(clientSocketGlobal, &readfds);

    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 1000 * 100; // �ȴ�100msһ�֣��Ա�ɶ�γ���

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
                cerr << "���棡 �յ���ACKУ��Ͳ�ƥ�䣬�������ݰ�" << endl;
                return true; // �յ�������������һ��
            }

            if (ackMsg.Flag & ACK) {
                if (ackMsg.Ack > lastAck) {
                    // �µ�ACK
                    dupACKcount = 0;
                    if (fastRecovery) {
                        cout << "���ٻָ��� �յ���ACK���˳����ٻָ��׶�" << endl;
                        fastRecovery = false;
                        cwnd = ssthresh;
                    }
                    else {
                        // ��������ӵ������
                        if (cwnd < ssthresh) {
                            cwnd += 1.0;
                            cout << "�������� cwnd ���� " << cwnd << endl;
                        }
                        else {
                            cwnd += 1.0 / cwnd;
                            cout << "ӵ�����⣺ cwnd ���� " << cwnd << endl;
                        }
                    }

                    // �Ƴ���ȷ�ϵİ�
                    for (uint32_t seq = baseSeq; seq < ackMsg.Ack; ++seq) {
                        sendBuffer.erase(seq);
                    }
                    baseSeq = ackMsg.Ack;
                    lastAck = ackMsg.Ack;
                }
                else if (ackMsg.Ack == lastAck) {
                    // �ظ�ACK
                    dupACKcount++;
                    cout << "�յ��ظ�ACK��count: " << dupACKcount << endl;

                    if (dupACKcount == 3 && !fastRecovery) {
                        // �����ش�
                        auto it = sendBuffer.find(ackMsg.Ack);
                        if (it != sendBuffer.end()) {
                            cout << "�����ش��� ���к� " << ackMsg.Ack << " �ش�" << endl;
                            sendMessage(it->second.msg);
                            it->second.sentTime = chrono::steady_clock::now();
                        }
                        ssthresh = cwnd / 2;
                        if (ssthresh < 1.0) ssthresh = 1.0;
                        cwnd = ssthresh + 3;
                        fastRecovery = true;
                        cout << "�����ش��� ������ٻָ��׶�, ssthresh = " << ssthresh << ", cwnd = " << cwnd << endl;
                    }
                    else if (dupACKcount > 3 && fastRecovery) {
                        cwnd += 1.0;
                        cout << "���ٻָ��� �ظ�ACK����cwnd = " << cwnd << endl;
                    }
                }
            }

            if (ackMsg.Flag & FIN) {
                cout << "�յ���������FIN������ACKȷ��..." << endl;
                Message finalAck = {};
                finalAck.Flag = ACK;
                finalAck.Seq = 0;
                finalAck.Ack = ackMsg.Seq + 1;
                sendMessage(finalAck);
                cout << "�Ĵλ��ֳɹ������ӶϿ�" << endl;
                connected = false;
            }

            if (ackMsg.Flag & CLOSE) {
                cout << "�յ��������Ĺر����󣬹ر�����" << endl;
                connected = false;
            }
        }
    }
    return true;
}

// ��ʱ��Ⲣ�ش���ʱ�����ݰ�
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
                cerr << "���к� " << seq << " �ﵽ����ش�����������ʧ�ܡ�" << endl;
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
        cerr << "Winsock��ʼ��ʧ�ܣ�������: " << result << endl;
        exit(1);
    }
}

SOCKET createSocketFunc() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        cerr << "�׽��ִ���ʧ�ܣ�������: " << WSAGetLastError() << endl;
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

    // ��������
    Message synMsg = {};
    synMsg.Seq = 1;
    synMsg.Flag = SYN;
    cout << "�������ӷ�����..." << endl;
    sendMessage(synMsg);

    {
        // �ȴ�SYN-ACK
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(clientSocketGlobal, &readfds);
        timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        int ret = select(0, &readfds, NULL, NULL, &tv);
        if (ret <= 0) {
            cerr << "�ȴ�SYN-ACK��ʱ�����" << endl;
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
                cerr << "�յ���SYN-ACKУ��Ͳ�ƥ��" << endl;
                closesocket(clientSocketGlobal);
                WSACleanup();
                return 1;
            }

            cout << "�յ�SYN-ACK, ����ACK..." << endl;
            Message ackMsg = {};
            ackMsg.Flag = ACK;
            ackMsg.Seq = 0;
            ackMsg.Ack = recvMsg.Seq + 1;
            sendMessage(ackMsg);
            baseSeq = ackMsg.Ack;
            nextSeq = baseSeq;
            lastAck = baseSeq;
            cout << "�������ֳɹ������ӽ���" << endl;
        }
        else {
            cerr << "δ�յ���Ч��SYN-ACK" << endl;
            closesocket(clientSocketGlobal);
            WSACleanup();
            return 1;
        }
    }

    // ��ʼ�����ļ�
    cout << "�������ļ�·����" << endl;
    string filePath;
    cin >> filePath;
    size_t pos = filePath.find_last_of("\\/");
    string fileName = (pos == string::npos) ? filePath : filePath.substr(pos + 1);

    // �����ļ���
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
        cerr << "�޷����ļ�: " << filePath << endl;
        connected = false;
    }

    bool fileSent = false;

    // �����������ڼ�¼����ʱ���������
    chrono::steady_clock::time_point startTime;
    chrono::steady_clock::time_point endTime;
    uint64_t totalBytesSent = 0; // �ܷ����ֽ���

    // ��¼���俪ʼʱ��
    startTime = chrono::steady_clock::now();

    while (connected) {
        // ���Է������ݰ�������cwnd�����ɷ����٣�
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

                // �ۼƷ��͵��ֽ���
                totalBytesSent += bytesRead;
            }
            else {
                // �ļ���ȡ��ϣ�����FIN
                fileSent = true;
                break;
            }
        }

        if (fileSent && sendBuffer.empty()) {
            // ����ȫ��ȷ�Ϻ���FIN
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

        // ����ACK
        receiveACK();

        // ��鳬ʱ���ش�
        checkTimeouts();

        // ���fin�ѷ�����bufferΪ����δ�յ���������FIN�����Ե�һ���ټ���ѭ��
        if (fileSent && sendBuffer.empty() && connected) {
            // �ļ����꣬�ȶԷ�FIN��CLOSE
            Sleep(500);
        }
    }

    // ��¼�������ʱ��
    endTime = chrono::steady_clock::now();

    // ���㴫��ʱ�䣨�룩
    chrono::duration<double> elapsed = endTime - startTime;
    double transmissionTime = elapsed.count();

    // ���������ʣ��ֽ�/�룩
    double throughput = (double)totalBytesSent / transmissionTime;

    // �������ʱ���������
    cout << "����ʱ��: " << transmissionTime << " ��" << endl;
    cout << "������: " << throughput << " �ֽ�/��" << endl;

    // ������˳�
    closesocket(clientSocketGlobal);
    WSACleanup();
    cout << "�ͻ����ѽ������Ӳ��˳���" << endl;
    return 0;
}
