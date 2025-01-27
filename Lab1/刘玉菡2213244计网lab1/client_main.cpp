#include <stdio.h>
#include <WinSock2.h>
#include<iostream>
#include <thread>
#include <string>


#pragma comment(lib,"ws2_32.lib")
#pragma warning(disable:4996)


using namespace std;

string current_mode = "group";  // 默认群聊模式
string target_name = "";  // 私聊目标ID，默认没有

// 接收服务器消息的函数
void recv_func(SOCKET client_socket)
{
	while (1) {
		char rbuffer[1024] = { 0 };
		int ret = recv(client_socket, rbuffer, 1024, 0);
		if (ret == SOCKET_ERROR) {
			cout << "接收消息失败，错误码: " << WSAGetLastError() << endl;
			break;
		}
		if (ret <= 0) {
			cout << "服务器断开连接" << endl;
			break;
		}
		rbuffer[ret] = '\0';  // 确保字符串结尾
		cout << rbuffer << endl;
	}
}



int main()
{
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
	


	//1、创建socket套接字
	SOCKET  client_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (INVALID_SOCKET == client_socket)
	{
		cout << "创建socket失败！！" << endl;
		return -1;
	}


	//2、连接服务器
	struct sockaddr_in target;
	target.sin_family = AF_INET;
	target.sin_port = htons(8080);
	target.sin_addr.s_addr = inet_addr("127.0.0.1");

	if (-1 == connect(client_socket, (struct sockaddr*)&target, sizeof(target)))
	{
		cout << "连接服务失败！！1" << endl;
		closesocket(client_socket);
		return -1;
	}

	// 输入用户名并发送到服务器
	string username;
	cout << "**默认为群聊模式 ********* 输入 q 则退出客户端**" << endl;
	cout << "请输入用户名: ";
	getline(cin, username);
	send(client_socket, username.c_str(), username.size(), 0);

	// 启动接收消息的线程
	thread recv_thread(recv_func, client_socket);
	recv_thread.detach();  // 分离线程，独立运行


	////3、开始通讯 send recv
	
	// 发送消息的主线程
	//while (1)
	//{
	//	char buffer[1024] = { 0 };
	//	cin.getline(buffer, 1024);

	//	if (strcmp(buffer, "exit") == 0)  // 如果输入exit，退出
	//		break;

	//	send(client_socket, buffer, strlen(buffer), 0);
	//}
	while (1) {
		string input;
		getline(cin, input);

		if (input == "q") {
			cout << "客户端正在退出..." << endl;
			closesocket(client_socket);  // 关闭客户端socket
			break;  // 退出循环，结束程序
		}

		if (input == "group") {
			current_mode = "group";
			cout << "切换到群聊模式" << endl;
		}
		else if (input.find("private") == 0) {
			if (input.length() <= 8) {
				cout << "请输入私聊目标用户名" << endl;
				continue;  // 如果没有提供用户名，则提示并跳过
			}

			target_name = input.substr(8);  // 获取目标用户名
			current_mode = "private";
			cout << "切换到私聊模式，目标用户: " << target_name << endl;
		}
		else {
			// 根据模式发送消息
			if (current_mode == "group") {
				send(client_socket, input.c_str(), input.size() + 1, 0);  // 确保包含字符串结束符 '\0'
				//cout << "发送群聊消息: " << input << endl;  // 添加调试输出
			}
			else if (current_mode == "private") {
				string msg = "@" + target_name + " " + input;
				send(client_socket, msg.c_str(), msg.size() + 1, 0);  // 确保包含字符串结束符 '\0'
				cout << "发送私聊消息: " << msg << endl;  // 添加调试输出
			}
		}
	}


	//4、关闭连接
	closesocket(client_socket);
	WSACleanup();

	return 0;
}




