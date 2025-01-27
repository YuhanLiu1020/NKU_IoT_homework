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

// 全局变量：保存所有已连接的客户端
vector<SOCKET> client_sockets;
mutex client_sockets_mutex;

// 保存客户端信息（ID 和 SOCKET）
struct ClientInfo {
	SOCKET client_socket;
	//int client_id;
	string username; // 可选，假设每个客户端有一个用户名
};

list<ClientInfo> clients;
mutex clients_mutex;

// 通过用户名查找客户端
SOCKET find_client_by_name(const string& username) {
	lock_guard<mutex> guard(clients_mutex);
	for (const auto& client : clients) {
		if (client.username == username) {
			return client.client_socket;
		}
	}
	return INVALID_SOCKET;
}

// 广播群聊消息，包含发送者名字
void broadcast_message(const string& message, const string& sender, SOCKET sender_socket) {
	lock_guard<mutex> guard(clients_mutex);
	string full_message = "☆" + sender + " 说: " + message;
	cout << "广播消息: " << full_message << endl;

	for (const auto& client : clients) {
		if (client.client_socket != sender_socket) {  // 跳过发送者
			send(client.client_socket, full_message.c_str(), full_message.size() + 1, 0);
		}
	}
}


// 私聊消息，查找目标用户并发送消息
void private_message(const string& message, const string& target_name, const string& sender) {
	SOCKET target_socket = find_client_by_name(target_name);
	if (target_socket != INVALID_SOCKET) {
		string full_message = sender + " 悄悄对你说:" + message;
		cout << "发送私聊消息给 " << target_name << ": " << full_message << endl; // 调试信息
		send(target_socket, full_message.c_str(), full_message.size() + 1, 0);
	}
	else {
		cout << "未找到目标用户: " << target_name << endl; // 调试信息
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
		if (ret <= 0) break;  // 如果接收失败或客户端断开连接

		buffer[ret] = '\0';  // 确保字符串以 '\0' 结尾

		string msg(buffer);
		cout << username << " 发送消息: " << msg << endl;

		// 广播消息给所有客户端，跳过发送者
		//broadcast_message(msg, username, client_socket);  // 传入发送者的 socket

		if (msg.find("@") == 0) {  // 私聊消息的处理逻辑
			size_t space_pos = msg.find(' ');
			if (space_pos == string::npos) {
				cout << "私聊消息格式错误" << endl;
				continue;  // 如果格式不正确，跳过本次处理
			}

			string target_name = msg.substr(1, space_pos - 1);  // 提取目标用户名
			string private_msg = msg.substr(space_pos + 1);  // 提取私聊消息内容
			cout << "目标用户: " << target_name << ", 消息内容: " << private_msg << endl;

			// 发送私聊消息
			private_message(private_msg, target_name, username);
		}
		else {
			// 群聊消息处理
			broadcast_message(msg, username, client_socket);
		}
	}

	cout << "socket:" << client_socket <<"("<<username<<")" << " 断开连接" << endl;

	// 客户端断开连接时从列表中移除
	{
		lock_guard<mutex> guard(clients_mutex);
		clients.erase(remove_if(clients.begin(), clients.end(),
			[&](const ClientInfo& client) { return client.client_socket == client_socket; }),
			clients.end());
	}

	// 关闭连接
	closesocket(client_socket);
	return 0;
}



int main()
{
	//windows上使用网络功能需要开始网络权限
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);

	//1、创建socket套接字
	SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (INVALID_SOCKET == listen_socket) {
		cout << "listen_socket创建失败！！！" << " " << GetLastError() << endl;
		return -1;
	}

	//2、给这个socket绑定一个端口号
	struct sockaddr_in local = { 0 };
	local.sin_family = AF_INET;
	local.sin_port = htons(8080);//大小端的问题 中间设备使用的是大端序（路由器）
	//local.sin_addr.s_addr = htonl(INADDR_ANY); //服务端 选项 网卡127.0.0.1（本地环回）只接受哪个网卡的数据 一般写全0地址表示全部都接受
	local.sin_addr.s_addr = inet_addr("0.0.0.0"); //字符串IP地址转换 成整数IP地址

	//int bind(int sockfd, const struct sockaddr *addr,socklen_t addrlen);
	if (-1 == bind(listen_socket, (struct sockaddr*)&local, sizeof(local)))
	{
		cout << "绑定socket失败！！错误：" << GetLastError() << endl;
		return -1;
	}


	//3、给这个socket开启监听属性
	if (-1 == listen(listen_socket, 10))
	{
		cout << "启动监听socket失败！！错误：" << GetLastError() << endl;
		return -1;
	}


	//4、等待客户端连接
	//返回的客户端socket才是跟客户端可以通讯的一个socket
	//阻塞函数，等到有客户端连接进来就接受连接，然后返回，否则就阻塞
	while (1)
	{
		SOCKET client_socket = accept(listen_socket, NULL, NULL);
		if (INVALID_SOCKET == client_socket)
			continue;

		// 获取用户名
		char name_buffer[1024] = { 0 };
		recv(client_socket, name_buffer, 1024, 0);
		string username(name_buffer);

		cout << "新的socket连接:（"<<client_socket<<")" << username << endl;

		ClientInfo* client_info = new ClientInfo{ client_socket, username };

		//SOCKET* sockfd = (SOCKET*)malloc(sizeof(SOCKET));
		//*sockfd = client_socket;
		// 在 client_info 中添加新的客户端
		{
			lock_guard<mutex> guard(clients_mutex);  // 加锁
			clients.push_back(*client_info);  // 添加新客户端到 clients 列表中
		}


		//创建线程
		//CreateThread(NULL,0,thread_func,sockfd,0,NULL);

		// 创建线程处理该客户端
		CreateThread(NULL, 0, thread_func, client_info, 0, NULL);


	}



	return 0;
}



