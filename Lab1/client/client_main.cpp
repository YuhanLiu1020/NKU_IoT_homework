#include <stdio.h>
#include <WinSock2.h>
#include<iostream>
#include <thread>
#include <string>


#pragma comment(lib,"ws2_32.lib")
#pragma warning(disable:4996)


using namespace std;

string current_mode = "group";  // Ĭ��Ⱥ��ģʽ
string target_name = "";  // ˽��Ŀ��ID��Ĭ��û��

// ���շ�������Ϣ�ĺ���
void recv_func(SOCKET client_socket)
{
	while (1) {
		char rbuffer[1024] = { 0 };
		int ret = recv(client_socket, rbuffer, 1024, 0);
		if (ret == SOCKET_ERROR) {
			cout << "������Ϣʧ�ܣ�������: " << WSAGetLastError() << endl;
			break;
		}
		if (ret <= 0) {
			cout << "�������Ͽ�����" << endl;
			break;
		}
		rbuffer[ret] = '\0';  // ȷ���ַ�����β
		cout << rbuffer << endl;
	}
}



int main()
{
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
	


	//1������socket�׽���
	SOCKET  client_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (INVALID_SOCKET == client_socket)
	{
		cout << "����socketʧ�ܣ���" << endl;
		return -1;
	}


	//2�����ӷ�����
	struct sockaddr_in target;
	target.sin_family = AF_INET;
	target.sin_port = htons(8080);
	target.sin_addr.s_addr = inet_addr("127.0.0.1");

	if (-1 == connect(client_socket, (struct sockaddr*)&target, sizeof(target)))
	{
		cout << "���ӷ���ʧ�ܣ���1" << endl;
		closesocket(client_socket);
		return -1;
	}

	// �����û��������͵�������
	string username;
	cout << "**Ĭ��ΪȺ��ģʽ ********* ���� q ���˳��ͻ���**" << endl;
	cout << "�������û���: ";
	getline(cin, username);
	send(client_socket, username.c_str(), username.size(), 0);

	// ����������Ϣ���߳�
	thread recv_thread(recv_func, client_socket);
	recv_thread.detach();  // �����̣߳���������


	////3����ʼͨѶ send recv
	
	// ������Ϣ�����߳�
	//while (1)
	//{
	//	char buffer[1024] = { 0 };
	//	cin.getline(buffer, 1024);

	//	if (strcmp(buffer, "exit") == 0)  // �������exit���˳�
	//		break;

	//	send(client_socket, buffer, strlen(buffer), 0);
	//}
	while (1) {
		string input;
		getline(cin, input);

		if (input == "q") {
			cout << "�ͻ��������˳�..." << endl;
			closesocket(client_socket);  // �رտͻ���socket
			break;  // �˳�ѭ������������
		}

		if (input == "group") {
			current_mode = "group";
			cout << "�л���Ⱥ��ģʽ" << endl;
		}
		else if (input.find("private") == 0) {
			if (input.length() <= 8) {
				cout << "������˽��Ŀ���û���" << endl;
				continue;  // ���û���ṩ�û���������ʾ������
			}

			target_name = input.substr(8);  // ��ȡĿ���û���
			current_mode = "private";
			cout << "�л���˽��ģʽ��Ŀ���û�: " << target_name << endl;
		}
		else {
			// ����ģʽ������Ϣ
			if (current_mode == "group") {
				send(client_socket, input.c_str(), input.size() + 1, 0);  // ȷ�������ַ��������� '\0'
				//cout << "����Ⱥ����Ϣ: " << input << endl;  // ��ӵ������
			}
			else if (current_mode == "private") {
				string msg = "@" + target_name + " " + input;
				send(client_socket, msg.c_str(), msg.size() + 1, 0);  // ȷ�������ַ��������� '\0'
				cout << "����˽����Ϣ: " << msg << endl;  // ��ӵ������
			}
		}
	}


	//4���ر�����
	closesocket(client_socket);
	WSACleanup();

	return 0;
}




