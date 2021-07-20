#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/wait.h>
#include <stdlib.h>

#define ISspace(x) isspace((int)(x))  //检查是否为空格


#define SERVER_STRING "Server: pakchoi's http/0.1.0\r\n"//定义个人server名称


void *accept_request(void* client);//处理从套接字上监听到的一个HTTP请求
void bad_request(int);  //返回给客户端这是个错误请求，400响应码
void cat(int, FILE *); //读取服务器上某个文件写到socket套接字
void cannot_execute(int);  //处理发生在执行Cgi程序时出现的错误
void error_die(const char *); //错误perror输出
void execute_cgi(int, const char *, const char *, const char *);  //执行cgi脚本
int get_line(int, char *, int);  //读取一行HTTP报文
void headers(int, const char *);  //返回HTTP响应头
void not_found(int);  //返回找不到请求文件
void serve_file(int, const char *);  //调用 cat 把服务器文件内容返回给浏览器
int startup(u_short *);  //开启http服务，包括绑定端口，监听，开启线程处理
void unimplemented(int);  //返回给浏览器表明收到的HTTP请求所用的 method 不被支持

//执行顺序：main -> startup -> accept_request -> execute_cgi

//处理监听到的 HTTP 请求
void *accept_request(void* from_client)
{
	int client = *(int *)from_client;
	char buf[1024];
	int numchars;
	char method[255];
	char url[255];
	char path[512];
	size_t i, j;
	struct stat st; 
	int cgi = 0;    //如果服务器确定这是cgi程序就返回true 
	char *query_string = NULL;
	
	//获取一行HTTP报文数据
	numchars = get_line(client, buf, sizeof(buf));
	

	i = 0; 
	j = 0;
	//对于HTTP报文，第一行为报文的起始行，格式为 <method> <request-URL> <version>
	//每个字段用空格相连

	while (!ISspace(buf[j]) && (i < sizeof(method) - 1))
	{
		//提取其中的请求方式
		method[i] = buf[j];
		i++; 
		j++;
	}
	method[i] = '\0';

	//strcasecmp(s1,s2) 若s1 == s2，返回0
	if (strcasecmp(method, "GET") && strcasecmp(method, "POST"))
	{
		//仅实现了GET和POST
		unimplemented(client);
		return NULL;
	}

	if (strcasecmp(method, "POST") == 0)  cgi = 1;
	  
	i = 0;
	//将method后面的空白字符略过
	while (ISspace(buf[j]) && (j < sizeof(buf)))
	j++;

	//继续读取 requestURL
	while (!ISspace(buf[j]) && (i < sizeof(url) - 1) && (j < sizeof(buf)))
	{
		url[i] = buf[j];
		i++; j++;
	}
	url[i] = '\0';


	//GET请求url可能会带有?,有查询参数
	if (strcasecmp(method, "GET") == 0)
	{
		 
		query_string = url;
		while ((*query_string != '?') && (*query_string != '\0'))
			query_string++;
		
		/* 如果有?表明是动态请求, 开启cgi */
		if (*query_string == '?')
		{
			cgi = 1;
			//将解析参数截取下来
			*query_string = '\0';
			query_string++;
		}
	}
	//以上已经将起始行解析完毕

	//url中的路径格式化到path
	sprintf(path, "httpdocs%s", url);

	//若path只是一个目录，默认设置为测试页 test.html
	if (path[strlen(path) - 1] == '/')
	{
		strcat(path, "test.html");
	}

	// int stat(const char *file_name, struct stat *buf);
	// 通过文件名file_name获取文件信息，并保存在buf所指的结构体stat中
	// 成功返回0，失败返回-1
	if (stat(path, &st) == -1) {
		//如果访问的页面不存在，则不断的读取剩下的请求头信息，并丢弃
		while ((numchars > 0) && strcmp("\n", buf))  
		numchars = get_line(client, buf, sizeof(buf));
		//声明网页不存在
		not_found(client);
	}
	else
	{
		//如果访问的页面存在则进行处理
		if ((st.st_mode & S_IFMT) == S_IFDIR)//S_IFDIR代表目录
	   //如果请求参数为目录, 自动打开test.html
		{
			strcat(path, "/test.html");
		}
	
	  //文件可执行
		if ((st.st_mode & S_IXUSR) || (st.st_mode & S_IXGRP) || (st.st_mode & S_IXOTH))
		  //S_IXUSR:文件所有者具可执行权限
		  //S_IXGRP:用户组具可执行权限
		  //S_IXOTH:其他用户具可读取权限  
			cgi = 1;

		if (!cgi)
			//将静态文件返回
			serve_file(client, path);
		else
			//执行cgi动态解析
			execute_cgi(client, path, method, query_string);
  
	}

	close(client);
	//printf("connection close....client: %d \n",client);
	return NULL;
}


//返回给客户端这是个错误请求，400响应码
void bad_request(int client)
{
	char buf[1024];
	//发送400
	sprintf(buf, "HTTP/1.0 400 BAD REQUEST\r\n");
	send(client, buf, sizeof(buf), 0);
	sprintf(buf, "Content-type: text/html\r\n");
	send(client, buf, sizeof(buf), 0);
	sprintf(buf, "\r\n");
	send(client, buf, sizeof(buf), 0);
	sprintf(buf, "<P>Your browser sent a bad request, ");
	send(client, buf, sizeof(buf), 0);
	sprintf(buf, "such as a POST without a Content-Length.\r\n");
	send(client, buf, sizeof(buf), 0);
}


//读取服务器上某个文件写到socket套接字
void cat(int client, FILE *resource)
{
	//发送文件的内容
	char buf[1024];
	//读取文件到buf中
	fgets(buf, sizeof(buf), resource);
	while (!feof(resource))  //判断文件是否读取到末尾
	{
		//读取并发送文件内容
		send(client, buf, strlen(buf), 0);
		fgets(buf, sizeof(buf), resource);
	}
}


//处理发生在执行cgi程序时出现的错误
void cannot_execute(int client)
{
	char buf[1024];
	//发送500
	sprintf(buf, "HTTP/1.0 500 Internal Server Error\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "Content-type: text/html\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<P>Error prohibited CGI execution.\r\n");
	send(client, buf, strlen(buf), 0);
}


//错误perror输出
void error_die(const char *sc)
{
	perror(sc);
	exit(1);
}


//执行cgi动态解析
void execute_cgi(int client, const char *path, const char *method, const char *query_string)
{
	char buf[1024];
	//读写管道
	int cgi_output[2];
	int cgi_input[2];
	
	pid_t pid;
	int status;

	int i;
	char c;

	int numchars = 1;
	int content_length = -1;
	//默认字符
	buf[0] = 'A'; 
	buf[1] = '\0';
	
	//如果是GET请求，读取并丢弃头信息
	if (strcasecmp(method, "GET") == 0)

		while ((numchars > 0) && strcmp("\n", buf))
		{
			numchars = get_line(client, buf, sizeof(buf));
		}
	
	else  //如果是POST请求  
	{
		numchars = get_line(client, buf, sizeof(buf));
		//循环读取头信息找到Content-Length字段的值
		while ((numchars > 0) && strcmp("\n", buf))
		{
			buf[15] = '\0'; //为了截取Content-Length：（Content-Length：正好15个字符）
			if (strcasecmp(buf, "Content-Length:") == 0)
				content_length = atoi(&(buf[16])); //获取Content-Length的值
			numchars = get_line(client, buf, sizeof(buf));
		}

		if (content_length == -1) {
		    //错误请求
		    bad_request(client);
		    return;
		}
	}

	//返回正确响应码200
	sprintf(buf, "HTTP/1.0 200 OK\r\n");
	send(client, buf, strlen(buf), 0);

	//pipe(cgi_output)执行成功后，cgi_output[0]：读通道，cgi_output[1]：写通道
	if (pipe(cgi_output) < 0) {
		cannot_execute(client);
		return;
	}

	if (pipe(cgi_input) < 0) { 
		cannot_execute(client);
		return;
	}

	//fork出一个子进程运行cgi脚本
	if ( (pid = fork()) < 0 ) {
		cannot_execute(client);
		return;
	}
	if (pid == 0)  /* 子进程: 运行CGI 脚本 */
	{
		char meth_env[255];
		char query_env[255];
		char length_env[255];

		dup2(cgi_output[1], 1);//1：stdout,0:stdin,将系统标准输出重定向为cgi_output[1]
		dup2(cgi_input[0], 0);//将系统标准输入重定向为cgi_input[0]


		close(cgi_output[0]);//关闭了cgi_output中的读通道
		close(cgi_input[1]);//关闭了cgi_input中的写通道

		//存储REQUEST_METHOD
		sprintf(meth_env, "REQUEST_METHOD=%s", method);
		putenv(meth_env);

		if (strcasecmp(method, "GET") == 0) {
			//存储QUERY_STRING
			sprintf(query_env, "QUERY_STRING=%s", query_string);
			putenv(query_env);
		}
		else {   /* POST */
		    //存储CONTENT_LENGTH
			sprintf(length_env, "CONTENT_LENGTH=%d", content_length);
			putenv(length_env);
		}


		execl(path, path, NULL);//执行CGI脚本
		exit(0);
	} 
	//父进程
	else {  
		close(cgi_output[1]);//关闭cgi_output的写通道
		close(cgi_input[0]); //关闭cgi_input的读通道
		if (strcasecmp(method, "POST") == 0)

			for (i = 0; i < content_length; i++) 
			{
				//读取POST中的内容
				recv(client, &c, 1, 0);
				//将数据发送给cgi脚本
				write(cgi_input[1], &c, 1);
			}

		//读取cgi脚本返回数据
		while (read(cgi_output[0], &c, 1) > 0)
			//发送给浏览器
		{			
			send(client, &c, 1, 0);
		}

		//运行结束关闭
		close(cgi_output[0]);
		close(cgi_input[1]);

		waitpid(pid, &status, 0);
	}
}


//解析一行http报文
int get_line(int sock, char *buf, int size)
{
	int i = 0;
	char c = '\0';
	int n;

	while ((i < size - 1) && (c != '\n'))
	{
		n = recv(sock, &c, 1, 0);

		if (n > 0) 
		{
			if (c == '\r')
			{

				n = recv(sock, &c, 1, MSG_PEEK);
				if ((n > 0) && (c == '\n'))
					recv(sock, &c, 1, 0);
				else
					c = '\n';
			   }
			   buf[i] = c;
			   i++;
			   
		}
		else
		   c = '\n';
	}
	buf[i] = '\0';
	return(i);
}


//返回HTTP响应头
void headers(int client, const char *filename)
{
	
	char buf[1024];

	(void)filename;  /* could use filename to determine file type */
	//发送HTTP头
	strcpy(buf, "HTTP/1.0 200 OK\r\n");
	send(client, buf, strlen(buf), 0);
	strcpy(buf, SERVER_STRING);
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "Content-Type: text/html\r\n");
	send(client, buf, strlen(buf), 0);
	strcpy(buf, "\r\n");
	send(client, buf, strlen(buf), 0);

}


//404
void not_found(int client)
{
	char buf[1024];
	//返回404
	sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, SERVER_STRING);
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "Content-Type: text/html\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "your request because the resource specified\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "is unavailable or nonexistent.\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "</BODY></HTML>\r\n");
	send(client, buf, strlen(buf), 0);
}


//如果不是CGI文件，也就是静态文件，直接读取文件返回给请求的http客户端
//将请求的文件发送回浏览器客户端
void serve_file(int client, const char *filename)
{
	FILE *resource = NULL;
	int numchars = 1;
	char buf[1024];
	buf[0] = 'A'; 
	buf[1] = '\0';
	//将HTTP请求头读取并丢弃
	while ((numchars > 0) && strcmp("\n", buf)) 
	{
		numchars = get_line(client, buf, sizeof(buf));
	}
	
	//打开文件
	resource = fopen(filename, "r");
	if (resource == NULL)
		not_found(client);
	else
	{
		//添加HTTP头
		headers(client, filename);
		//发送文件内容
		cat(client, resource);
	}
	fclose(resource);//关闭文件句柄
}

//启动服务端
int startup(u_short *port) 
{
	int httpd = 0,option;
	struct sockaddr_in name;
	//设置http socket
	httpd = socket(PF_INET, SOCK_STREAM, 0);
	if (httpd == -1)
		error_die("socket");//连接失败
	
	socklen_t optlen;
	optlen = sizeof(option);
    option = 1;
    setsockopt(httpd, SOL_SOCKET, SO_REUSEADDR, (void *)&option, optlen);
	
	
	memset(&name, 0, sizeof(name));
	name.sin_family = AF_INET;
	name.sin_port = htons(*port);
	name.sin_addr.s_addr = htonl(INADDR_ANY);
	//绑定端口
	if (bind(httpd, (struct sockaddr *)&name, sizeof(name)) < 0)
		error_die("bind");//绑定失败
	if (*port == 0)  /*动态分配一个端口 */
	{
		socklen_t  namelen = sizeof(name);
		if (getsockname(httpd, (struct sockaddr *)&name, &namelen) == -1)
			error_die("getsockname");
		*port = ntohs(name.sin_port);
	}
	//监听连接
	if (listen(httpd, 5) < 0)
		error_die("listen");
	
	return(httpd);
}


//返回给浏览器表明收到的HTTP请求所用的 method 不被支持
void unimplemented(int client)
{
	char buf[1024];
	//发送501说明相应方法没有实现
	sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, SERVER_STRING);
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "Content-Type: text/html\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "</TITLE></HEAD>\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<BODY><P>HTTP request method not supported.\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "</BODY></HTML>\r\n");
	send(client, buf, strlen(buf), 0);
}

/**********************************************************************/

int main(void)
{
	int server_sock = -1;
	u_short port = 6660;//默认监听端口号 port 为6660
	int client_sock = -1;
	struct sockaddr_in client_name;
	socklen_t client_name_len = sizeof(client_name);
	pthread_t newthread;
	
	//启动server_sock
	server_sock = startup(&port);
 
	printf("http server_sock is %d\n", server_sock);
	printf("http running on port %d\n", port);
	
	while (1)
	{

		client_sock = accept(server_sock,
							   (struct sockaddr *)&client_name,
							   &client_name_len);
		  
		printf("New connection....  ip: %s , port: %d\n",inet_ntoa(client_name.sin_addr),ntohs(client_name.sin_port));
		if (client_sock == -1)
			error_die("accept");
		//启动线程处理新的连接
		if (pthread_create(&newthread , NULL, accept_request, (void*)&client_sock) != 0)
			perror("pthread_create");

	}
	close(server_sock);

	return(0);
}
