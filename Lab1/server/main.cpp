#include <stdio.h>
#include <WinSock2.h>
#pragma comment(lib,"ws2_32.lib ")
#include <iostream>
#include <vector>
#include <mutex>
#include<string>
#include <list>

#pragma warning(disable:4996)
using namespace std;

// ȫ�ֱ������������������ӵĿͻ���
vector<SOCKET> client_sockets;
mutex client_sockets_mutex;

// ����ͻ�����Ϣ��ID �� SOCKET��
struct ClientInfo {
	SOCKET client_socket;
	//int client_id;
	string username; // ��ѡ������ÿ���ͻ�����һ���û���
};

list<ClientInfo> clients;
mutex clients_mutex;

// ͨ���û������ҿͻ���
SOCKET find_client_by_name(const string& username) {
	lock_guard<mutex> guard(clients_mutex);
	for (const auto& client : clients) {
		if (client.username == username) {
			return client.client_socket;
		}
	}
	return INVALID_SOCKET;
}

// �㲥Ⱥ����Ϣ����������������
void broadcast_message(const string& message, const string& sender, SOCKET sender_socket) {
	lock_guard<mutex> guard(clients_mutex);
	string full_message = "��" + sender + " ˵: " + message;
	cout << "�㲥��Ϣ: " << full_message << endl;

	for (const auto& client : clients) {
		if (client.client_socket != sender_socket) {  // ����������
			send(client.client_socket, full_message.c_str(), full_message.size() + 1, 0);
		}
	}
}


// ˽����Ϣ������Ŀ���û���������Ϣ
void private_message(const string& message, const string& target_name, const string& sender) {
	SOCKET target_socket = find_client_by_name(target_name);
	if (target_socket != INVALID_SOCKET) {
		string full_message = sender + " ���Ķ���˵:" + message;
		cout << "����˽����Ϣ�� " << target_name << ": " << full_message << endl; // ������Ϣ
		send(target_socket, full_message.c_str(), full_message.size() + 1, 0);
	}
	else {
		cout << "δ�ҵ�Ŀ���û�: " << target_name << endl; // ������Ϣ
	}
}



DWORD thread_func(LPVOID lpThreadParameter)
{
	ClientInfo* client_info = (ClientInfo*)lpThreadParameter;
	SOCKET client_socket = client_info->client_socket;
	string username = client_info->username;

	while (1) {
		char buffer[1024] = { 0 };
		int ret = recv(client_socket, buffer, 1024, 0);
		if (ret <= 0) break;  // �������ʧ�ܻ�ͻ��˶Ͽ�����

		buffer[ret] = '\0';  // ȷ���ַ����� '\0' ��β

		string msg(buffer);
		cout << username << " ������Ϣ: " << msg << endl;

		// �㲥��Ϣ�����пͻ��ˣ�����������
		//broadcast_message(msg, username, client_socket);  // ���뷢���ߵ� socket

		if (msg.find("@") == 0) {  // ˽����Ϣ�Ĵ����߼�
			size_t space_pos = msg.find(' ');
			if (space_pos == string::npos) {
				cout << "˽����Ϣ��ʽ����" << endl;
				continue;  // �����ʽ����ȷ���������δ���
			}

			string target_name = msg.substr(1, space_pos - 1);  // ��ȡĿ���û���
			string private_msg = msg.substr(space_pos + 1);  // ��ȡ˽����Ϣ����
			cout << "Ŀ���û�: " << target_name << ", ��Ϣ����: " << private_msg << endl;

			// ����˽����Ϣ
			private_message(private_msg, target_name, username);
		}
		else {
			// Ⱥ����Ϣ����
			broadcast_message(msg, username, client_socket);
		}
	}

	cout << "socket:" << client_socket <<"("<<username<<")" << " �Ͽ�����" << endl;

	// �ͻ��˶Ͽ�����ʱ���б����Ƴ�
	{
		lock_guard<mutex> guard(clients_mutex);
		clients.erase(remove_if(clients.begin(), clients.end(),
			[&](const ClientInfo& client) { return client.client_socket == client_socket; }),
			clients.end());
	}

	// �ر�����
	closesocket(client_socket);
	return 0;
}



int main()
{
	//windows��ʹ�����繦����Ҫ��ʼ����Ȩ��
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);

	//1������socket�׽���
	SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (INVALID_SOCKET == listen_socket) {
		cout << "listen_socket����ʧ�ܣ�����" << " " << GetLastError() << endl;
		return -1;
	}

	//2�������socket��һ���˿ں�
	struct sockaddr_in local = { 0 };
	local.sin_family = AF_INET;
	local.sin_port = htons(8080);//��С�˵����� �м��豸ʹ�õ��Ǵ����·������
	//local.sin_addr.s_addr = htonl(INADDR_ANY); //����� ѡ�� ����127.0.0.1�����ػ��أ�ֻ�����ĸ����������� һ��дȫ0��ַ��ʾȫ��������
	local.sin_addr.s_addr = inet_addr("0.0.0.0"); //�ַ���IP��ַת�� ������IP��ַ

	//int bind(int sockfd, const struct sockaddr *addr,socklen_t addrlen);
	if (-1 == bind(listen_socket, (struct sockaddr*)&local, sizeof(local)))
	{
		cout << "��socketʧ�ܣ�������" << GetLastError() << endl;
		return -1;
	}


	//3�������socket������������
	if (-1 == listen(listen_socket, 10))
	{
		cout << "��������socketʧ�ܣ�������" << GetLastError() << endl;
		return -1;
	}


	//4���ȴ��ͻ�������
	//���صĿͻ���socket���Ǹ��ͻ��˿���ͨѶ��һ��socket
	//�����������ȵ��пͻ������ӽ����ͽ������ӣ�Ȼ�󷵻أ����������
	while (1)
	{
		SOCKET client_socket = accept(listen_socket, NULL, NULL);
		if (INVALID_SOCKET == client_socket)
			continue;

		// ��ȡ�û���
		char name_buffer[1024] = { 0 };
		recv(client_socket, name_buffer, 1024, 0);
		string username(name_buffer);

		cout << "�µ�socket����:��"<<client_socket<<")" << username << endl;

		ClientInfo* client_info = new ClientInfo{ client_socket, username };

		//SOCKET* sockfd = (SOCKET*)malloc(sizeof(SOCKET));
		//*sockfd = client_socket;
		// �� client_info ������µĿͻ���
		{
			lock_guard<mutex> guard(clients_mutex);  // ����
			clients.push_back(*client_info);  // ����¿ͻ��˵� clients �б���
		}


		//�����߳�
		//CreateThread(NULL,0,thread_func,sockfd,0,NULL);

		// �����̴߳���ÿͻ���
		CreateThread(NULL, 0, thread_func, client_info, 0, NULL);


	}



	return 0;
}



