#include <stdio.h>

#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

void doit(int clientfd);
int parse_uri(char *uri, char *hostname, char *path, char *port);

int main(int argc, char **argv)
{
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

    if (argc != 2) {
        fprintf(stderr, "usage : %s <port>", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);
    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Proxy :: Accepted connection from (%s, %s)\n", hostname, port);
        doit(connfd);
        Close(connfd);
    }
    return 0;
}

void doit(int clientfd) {
    char buf[MAXLINE];     // 요청 줄 전체를 담을 버퍼
    char method[MAXLINE];  // "GET"
    char uri[MAXLINE];     // "http://..."
    char version[MAXLINE]; // "HTTP/1.1"

    char hostname[MAXLINE];
    char path[MAXLINE];
    char port[MAXLINE];

    rio_t rio;

    //  클라이언트 요청 라인 읽기
    Rio_readinitb(&rio, clientfd);
    Rio_readlineb(&rio, buf, MAXLINE); // ex: "GET http://www.cmu.edu/hub/index.html HTTP/1.1\r\n"

    // 요청 라인 파싱
    sscanf(buf, "%s %s %s", method, uri, version);
    printf("Proxy :: method=%s, uri=%s, version=%s\n", method, uri, version);

    // GET 이외의 지원하지 않는 메소드 처리
    if (strcasecmp(method, "GET")) {
        printf("Proxy does not implement the method : %s\n", method);
        return;
    }

    // URI 파싱 로직
    parse_uri(uri, hostname, path, port);
    printf("hostname=%s, port=%s\n", hostname, port);

    // 서버 연결하기
    int serverfd = Open_clientfd(hostname, port);  //  Open_clientfd()는 hostname과 port를 기반으로 서버와 연결된 소켓을 리턴.
    if (serverfd < 0) {
        printf("Connection failed to %s:%s\n", hostname, port);
        return;
    }

    // 요청 라인 만들기
    char request_line[MAXLINE];
    sprintf(request_line, "GET %s HTTP/1.0\r\n", path);

    // 필수 헤더 추가
    char header[MAXLINE];
    sprintf(header,
            "Host: %s\r\n"
            "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0) Gecko/20100101 Firefox/10.0\r\n"
            "Connection: close\r\n"
            "Proxy-Connection: close\r\n",
            hostname);

    while (Rio_readlineb(&rio, buf, MAXLINE) > 0) {
        if (!strcmp(buf, "\r\n")) break;
        if (strncasecmp(buf, "Host", 4) == 0) continue;
        if (strncasecmp(buf, "User-Agent", 10) == 0) continue;
        if (strncasecmp(buf, "Connection", 10) == 0) continue;
        if (strncasecmp(buf, "Proxy-Connection", 16) == 0) continue;

        strcat(header, buf);
    }

    // 빈줄 추가로 헤더의 끝 알리기
    strcat(header, "\r\n");

    // 서버에 요청 라인과 헤더 전송
    Rio_writen(serverfd, request_line, strlen(request_line));
    Rio_writen(serverfd, header, strlen(header));

    rio_t server_rio;
    int n;

    Rio_readinitb(&server_rio, serverfd);

    while ((n = Rio_readnb(&server_rio, buf, MAXBUF)) > 0) {
        Rio_writen(clientfd, buf, n);
    }
}

int parse_uri(char *uri, char *hostname, char *path, char *port) {
    char *host_begin, *host_end, *port_begin, *path_begin;

    if (strncasecmp(uri, "http://", 7) == 0)
        host_begin = uri + 7;
    else
        host_begin = uri;

    path_begin = strchr(host_begin, '/');
    if (path_begin) {
        strcpy(path, path_begin);
    } else {
        strcpy(path, "/");
    }

    host_end = path_begin ? path_begin : host_begin + strlen(host_begin);

    port_begin = strchr(host_begin, ':');
    if (port_begin && port_begin < host_end) {
        int hostname_len = port_begin - host_begin;
        strncpy(hostname, host_begin, hostname_len);
        hostname[hostname_len] = '\0';

        int port_len = host_end - port_begin - 1;
        strncpy(port, port_begin + 1, port_len);
        port[port_len] = '\0';
    } else {
        int hostname_len = host_end - host_begin;
        strncpy(hostname, host_begin, hostname_len);
        hostname[hostname_len] = '\0';
        strcpy(port, "80");
    }

    return 0;
}
