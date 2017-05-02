/* J. David's webserver */
/* This is a simple webserver.
 * Created November 1999 by J. David Blackstone.
 * CSE 4344 (Network concepts), Prof. Zeigler
 * University of Texas at Arlington
 */
/* This program compiles for Sparc Solaris 2.6.
 * To compile for Linux:
 *  1) Comment out the #include <pthread.h> line.
 *  2) Comment out the line that defines the variable newthread.
 *  3) Comment out the two lines that run pthread_create().
 *  4) Uncomment the line that runs accept_request().
 *  5) Remove -lsocket from the Makefile.
 */
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
#include <stdint.h>

#define ISspace(x) isspace((int)(x))

#define SERVER_STRING "Server: jdbhttpd/0.1.0\r\n"
#define STDIN   0
#define STDOUT  1
#define STDERR  2

void *accept_request(void *);		//接受客户端请求
void bad_request(int);				//400错误, 请求方法出错	
void cat(int, FILE *);				//读取文件发送给客户端
void cannot_execute(int);			//通知客户端执行cgi脚本失败
void error_die(const char *);		//错误处理
void execute_cgi(int, const char *, const char *, const char *);	//调用exec函数簇执行cgi脚本
int get_line(int, char *, int);		//读取套接字获取一行数据, 返回字符个数
void headers(int, const char *);	//发送头信息
void not_found(int);				//404错误
void serve_file(int, const char *); //返回静态页面
int startup(u_short *);				//套接字创建, 绑定, 监听
void unimplemented(int);			//请求方法未实现

/**********************************************************************/
/* A request has caused a call to accept() on the server port to
 * return.  Process the request appropriately.
 * Parameters: the socket connected to the client 
 *　处理每个客户端连接
 * */
/**********************************************************************/
void *accept_request(void *arg)
{
    int client = *(int*)arg;
    char buf[1024];
    size_t numchars;
    char method[255];
    char url[255];
    char path[512];
    size_t i, j;
    struct stat st;
    int cgi = 0;      /* becomes true if server decides this is a CGI
                       * program */
    char *query_string = NULL;
	
	/* 获取请求行，　返回字节数  eg: GET /index.html HTTP/1.1 */
    numchars = get_line(client, buf, sizeof(buf));
	/* debug */
	//printf("%s", buf);

	/* 获取请求方式, 保存在method中  GET或POST */
	i = 0; j = 0;
    while (!ISspace(buf[i]) && (i < sizeof(method) - 1))
    {
        method[i] = buf[i];
        i++;
    }
    j=i;
    method[i] = '\0';

	/* 只支持GET 和 POST 方法 */
    if (strcasecmp(method, "GET") && strcasecmp(method, "POST"))
    {
        unimplemented(client);
        return NULL;
    }

	/* 如果支持POST方法, 开启cgi */
    if (strcasecmp(method, "POST") == 0)
        cgi = 1;

    i = 0;
    while (ISspace(buf[j]) && (j < numchars))
        j++;
    while (!ISspace(buf[j]) && (i < sizeof(url) - 1) && (j < numchars))
    {
        url[i] = buf[j];
        i++; j++;
    }
	/* 保存请求的url, url上的参数也会保存 */
    url[i] = '\0';

	//printf("%s\n", url);

    if (strcasecmp(method, "GET") == 0)
    {
		/* query_string 保存请求参数 index.php?r=param  问号后面的 r=param */
        query_string = url;
        while ((*query_string != '?') && (*query_string != '\0'))
            query_string++;
		/* 如果有?表明是动态请求, 开启cgi */
        if (*query_string == '?')
        {
            cgi = 1;
            *query_string = '\0';
            query_string++;
        }
    }

//	printf("%s\n", query_string);

	/* 根目录在 htdocs 下, 默认访问当前请求下的index.html*/
    sprintf(path, "htdocs%s", url);
    if (path[strlen(path) - 1] == '/')
        strcat(path, "index.html");

	//printf("%s\n", path);
	/* 找到文件, 保存在结构体st中*/
    if (stat(path, &st) == -1) {
		/* 文件未找到, 丢弃所有http请求头信息 */
        while ((numchars > 0) && strcmp("\n", buf))  /* read & discard headers */
            numchars = get_line(client, buf, sizeof(buf));
        /* 404 no found */
		not_found(client);
    }
    else
    {

		//如果请求参数为目录, 自动打开index.html
        if ((st.st_mode & S_IFMT) == S_IFDIR)
            strcat(path, "/index.html");
		
		//文件可执行
        if ((st.st_mode & S_IXUSR) ||
                (st.st_mode & S_IXGRP) ||
                (st.st_mode & S_IXOTH)    )
            cgi = 1;
        if (!cgi)
			/* 请求静态页面 */
            serve_file(client, path);
        else
			/*　执行cgi 程序*/
            execute_cgi(client, path, method, query_string);
    }

    close(client);
	return NULL;
}

/**********************************************************************/
/* Inform the client that a request it has made has a problem.
 * Parameters: client socket */
/**********************************************************************/
void bad_request(int client)
{
    char buf[1024];

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

/**********************************************************************/
/* Put the entire contents of a file out on a socket.  This function
 * is named after the UNIX "cat" command, because it might have been
 * easier just to do something like pipe, fork, and exec("cat").
 * Parameters: the client socket descriptor
 *             FILE pointer for the file to cat */
/**********************************************************************/
void cat(int client, FILE *resource)
{
    char buf[1024];

    fgets(buf, sizeof(buf), resource);
    while (!feof(resource))
    {
        send(client, buf, strlen(buf), 0);
        fgets(buf, sizeof(buf), resource);
    }
}

/**********************************************************************/
/* Inform the client that a CGI script could not be executed.
 * Parameter: the client socket descriptor.
 * 服务器500错误
 * */
/**********************************************************************/
void cannot_execute(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 500 Internal Server Error\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<P>Error prohibited CGI execution.\r\n");
    send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Print out an error message with perror() (for system errors; based
 * on value of errno, which indicates system call errors) and exit the
 * program indicating an error. */
/**********************************************************************/
void error_die(const char *sc)
{
    perror(sc);
    exit(1);
}

/**********************************************************************/
/* Execute a CGI script.  Will need to set environment variables as
 * appropriate.
 * Parameters: client socket descriptor
 *             path to the CGI script */
/**********************************************************************/
void execute_cgi(int client, const char *path,
        const char *method, const char *query_string)
{
    char buf[1024];
    int cgi_output[2];
    int cgi_input[2];
    pid_t pid;
    int status;
    int i;
    char c;
    int numchars = 1;
    int content_length = -1;

    buf[0] = 'A'; buf[1] = '\0';
    if (strcasecmp(method, "GET") == 0)
			/* 读取和丢弃http请求头*/
        while ((numchars > 0) && strcmp("\n", buf))  /* read & discard headers */
            numchars = get_line(client, buf, sizeof(buf));
    else if (strcasecmp(method, "POST") == 0) /*POST*/
    {
        numchars = get_line(client, buf, sizeof(buf));
        while ((numchars > 0) && strcmp("\n", buf))
        {
            buf[15] = '\0';
			/* 获取http消息传输长度 */
            if (strcasecmp(buf, "Content-Length:") == 0)
                content_length = atoi(&(buf[16]));
            numchars = get_line(client, buf, sizeof(buf));
        }
        if (content_length == -1) {
            bad_request(client);
            return;
        }
    }
    else/*HEAD or other*/
    {
    }


	/* 
	 * 建立两条管道, 用于父子进程之间通信, cig使用标准输入和输出.
	 * 要获取标准输入输出, 可以把stdin重定向到cgi_input管道,  把stdout重定向到cgi_output管道
	 * 为什么使用两条管道 ? 一条管道可以看做储存一个信息, 只是一段用来读, 另一端用来写. 我们有标准输入和标准输出两个信息, 所以要两条管道
	 * */
    if (pipe(cgi_output) < 0) {
        cannot_execute(client);
        return;
    }
    if (pipe(cgi_input) < 0) {
        cannot_execute(client);
        return;
    }

	/*  创建子进程执行cgi函数, 获取cgi的标准输出通过管道传给父进程, 由父进程发给客户端. */
    if ( (pid = fork()) < 0 ) {
        cannot_execute(client);
        return;
    }
	/* 200　ok状态 */
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);

	/* 子进程执行cgi脚本 */
    if (pid == 0)  /* child: CGI script */
    {
        char meth_env[255];
        char query_env[255];
        char length_env[255];
		
        dup2(cgi_output[1], STDOUT);	//标准输出重定向到cgi_output的写端
        dup2(cgi_input[0], STDIN);		//标准输入重定向到cgi_input的读端
        close(cgi_output[0]);			//关闭cgi_output读端
        close(cgi_input[1]);			//关闭cgi_input写端
        
		/* 添加到子进程的环境变量中 */
		sprintf(meth_env, "REQUEST_METHOD=%s", method);
        putenv(meth_env);
        if (strcasecmp(method, "GET") == 0) {
			//设置QUERY_STRING环境变量
            sprintf(query_env, "QUERY_STRING=%s", query_string);
            putenv(query_env);
        }
        else {   /* POST */
            sprintf(length_env, "CONTENT_LENGTH=%d", content_length);
            putenv(length_env);
        }
		// 最后，子进程使用exec函数簇，调用外部脚本来执行
        execl(path,path,NULL);
        exit(0);
    } else {    /* parent */
		/* 父进程关闭cgi_output的写端和cgi_input的读端 */
        close(cgi_output[1]);
        close(cgi_input[0]);
		/* 如果是POST方法, 继续读取写入到cgi_input管道, 这是子进程会从此管道读取 */
        if (strcasecmp(method, "POST") == 0)
            for (i = 0; i < content_length; i++) {
                recv(client, &c, 1, 0);
                write(cgi_input[1], &c, 1);
            }
		/* 从cgi_output管道中读取子进程的输出, 发送给客户端 */
        while (read(cgi_output[0], &c, 1) > 0)
            send(client, &c, 1, 0);
		/* 关闭管道 */
        close(cgi_output[0]);
        close(cgi_input[1]);
		/* 等待子进程退出 */
        waitpid(pid, &status, 0);
    }
}

/**********************************************************************/
/* Get a line from a socket, whether the line ends in a newline,
 * carriage return, or a CRLF combination.  Terminates the string read
 * with a null character.  If no newline indicator is found before the
 * end of the buffer, the string is terminated with a null.  If any of
 * the above three line terminators is read, the last character of the
 * string will be a linefeed and the string will be terminated with a
 * null character.
 * Parameters: the socket descriptor
 *             the buffer to save the data in
 *             the size of the buffer
 * Returns: the number of bytes stored (excluding null) 
 *
 * 读取一行数据 , 以 '\n'结尾, 并在后面添加 '\0'
 *
 * */
/**********************************************************************/
int get_line(int sock, char *buf, int size)
{
    int i = 0;
    char c = '\0';
    int n;

    while ((i < size - 1) && (c != '\n'))
    {
		/* 一次接收一个字节 */
        n = recv(sock, &c, 1, 0);
        /* DEBUG printf("%02X\n", c); */
        if (n > 0)
        {
			/* \r\n结尾 */
            if (c == '\r')
            {
				//读缓冲区下一个字节, 但是不取走.
                n = recv(sock, &c, 1, MSG_PEEK);
                /* DEBUG printf("%02X\n", c); */
                if ((n > 0) && (c == '\n'))
					/* 取走 \n, 赋值给c */
                    recv(sock, &c, 1, 0);
                else
					/* 没有换行符, 则把c设置为\n */
                    c = '\n';
            }
			//存进缓冲区
            buf[i] = c;
            i++;
        }
        else
            c = '\n';
    }
	/* 结束标识 */
    buf[i] = '\0';

    return(i);
}

/**********************************************************************/
/* Return the informational HTTP headers about a file. */
/* Parameters: the socket to print the headers on
 *             the name of the file */
/**********************************************************************/
void headers(int client, const char *filename)
{
    char buf[1024];
    (void)filename;  /* could use filename to determine file type */

    strcpy(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Give a client a 404 not found status message. */
/**********************************************************************/
void not_found(int client)
{
    char buf[1024];

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

/**********************************************************************/
/* Send a regular file to the client.  Use headers, and report
 * errors to client if they occur.
 * Parameters: a pointer to a file structure produced from the socket
 *              file descriptor
 *             the name of the file to serve 
 * 静态文件　直接发送
 *             */
/**********************************************************************/
void serve_file(int client, const char *filename)
{
    FILE *resource = NULL;
    int numchars = 1;
    char buf[1024];

    buf[0] = 'A'; buf[1] = '\0';
    while ((numchars > 0) && strcmp("\n", buf))  /* read & discard headers */
        numchars = get_line(client, buf, sizeof(buf));

    resource = fopen(filename, "r");
    if (resource == NULL)
        not_found(client);
    else
    {
		//发送头信息
        headers(client, filename);
		//发送文件内容
        cat(client, resource);
    }
    fclose(resource);
}

/**********************************************************************/
/* This function starts the process of listening for web connections
 * on a specified port.  If the port is 0, then dynamically allocate a
 * port and modify the original port variable to reflect the actual
 * port.
 * Parameters: pointer to variable containing the port to connect on
 * Returns: the socket 
 * 建立socket, 绑定套接字, 并监听端口
 * */
 
/**********************************************************************/
int startup(u_short *port)
{
    int httpd = 0;
    int on = 1;
    struct sockaddr_in name;

	/* 建立套接字, 一条通信的线路 */
    httpd = socket(PF_INET, SOCK_STREAM, 0);
    if (httpd == -1)
        error_die("socket");
    memset(&name, 0, sizeof(name));				//0填充, struct sockaddr_in +实际多出来sin_zero没有用处.
    name.sin_family = AF_INET;					//IPV4协议
    name.sin_port = htons(*port);				//主机字节序转网络字节序
    name.sin_addr.s_addr = htonl(INADDR_ANY);	//监听任意IP

	/* 允许本地地址与套接字重复绑定 , 也就是说在TCP关闭连接处于TIME_OUT状态时重用socket */
    if ((setsockopt(httpd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on))) < 0)  
    {  
        error_die("setsockopt failed");
    }

	/* 用于socket信息与套接字绑定 */
    if (bind(httpd, (struct sockaddr *)&name, sizeof(name)) < 0)
        error_die("bind");

	/* 未设置端口则随机生成 */
	if (*port == 0)  /* if dynamically allocating a port */
    {
        socklen_t namelen = sizeof(name);
		/*使用次函数可回去友内核赋予该连接的端口号*/
        if (getsockname(httpd, (struct sockaddr *)&name, &namelen) == -1)
            error_die("getsockname");
        *port = ntohs(name.sin_port);
    }

	/* 使套接字处于被监听状态 */
    if (listen(httpd, 5) < 0)
        error_die("listen");
    return(httpd);
}

/**********************************************************************/
/* Inform the client that the requested web method has not been
 * implemented.
 * Parameter: the client socket 
 *
 * http 501状态码 
 * */
/**********************************************************************/
void unimplemented(int client)
{
    char buf[1024];

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
	/* 定义socket相关信息 */
    int server_sock = -1;
    u_short port = 4000;
    int client_sock = -1;
    struct sockaddr_in client_name;
    socklen_t  client_name_len = sizeof(client_name);
    pthread_t newthread;

    server_sock = startup(&port);
    printf("httpd running on port %d\n", port);

    while (1)
    {
		/* 通过accept接受客户端请求, 阻塞方式 */
        client_sock = accept(server_sock,
                (struct sockaddr *)&client_name,
                &client_name_len);
        if (client_sock == -1)
            error_die("accept");
        /* accept_request(&client_sock); */
		/* 开启线程处理客户端请求 */
        if (pthread_create(&newthread , NULL, accept_request, (void *)&client_sock) != 0)
            perror("pthread_create");
    }

    close(server_sock);

    return(0);
}
