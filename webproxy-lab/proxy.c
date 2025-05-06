#include <stdio.h>
#include <pthread.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

typedef struct CacheBlock {
    char *uri;                 // 요청 URI (키 역할)
    char *data;                // 실제 컨텐츠
    int size;                  // data의 바이트 크기
    struct CacheBlock *prev;  // 이전 캐시 블록
    struct CacheBlock *next;  // 다음 캐시 블록
} CacheBlock;

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

CacheBlock *head = NULL;
CacheBlock *tail = NULL;
size_t current_cache_size = 0;
// pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_rwlock_t cache_rwlock = PTHREAD_RWLOCK_INITIALIZER;


void doit(int clientfd);
int parse_uri(char *uri, char *hostname, char *path, char *port);
void *thread(void *vargp);
void evict_lru(int required_size);
void insert_cache(char *uri, char *data, int size);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

int main(int argc, char **argv)
{
    int listenfd, *connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "usage : %s <port>", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);

    while (1) {
        clientlen = sizeof(clientaddr);
        //connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        connfd = Malloc(sizeof(int));
        *connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        // printf("Proxy :: Accepted connection from (%s, %s)\n", hostname, port);
        // doit(connfd);
        // Close(connfd);
        pthread_create(&tid, NULL, thread, connfd);
    }
    return 0;
}

CacheBlock *find_cache(char *uri) {
    pthread_rwlock_rdlock(&cache_rwlock);

    CacheBlock *curr = head;
    while (curr) {
        if (strcmp(uri, curr->uri) == 0) {
            // 캐시 hit → LRU 정책을 위해 head로 이동 예정
            // move_to_head(curr); → 이건 캐시 정책 함수에서 처리할 수도 있음
            pthread_rwlock_unlock(&cache_rwlock);
            return curr;
        }
        curr = curr->next;
    }

    pthread_rwlock_unlock(&cache_rwlock);
    return NULL;  // 캐시 미스
}

void evict_lru(int required_size) {
    while (current_cache_size + required_size > MAX_CACHE_SIZE) {
        if (tail == NULL) break;
        current_cache_size -= tail->size;
        free(tail->data);
        free(tail->uri);
        free(tail);
        tail = tail->prev;
        if (tail != NULL)
            tail->next = NULL;
        else
            head = NULL;
    }
}

void move_to_head(CacheBlock *cb) {
    if (cb == head) return;

    if (cb->prev) cb->prev->next = cb->next;
    if (cb->next) cb->next->prev = cb->prev;

    if (cb == tail) tail = cb->prev;

    cb->prev = NULL;
    cb->next = head;
    if (head) head->prev = cb;
    head = cb;
}


void insert_cache(char *uri, char *data, int size) {
    pthread_rwlock_wrlock(&cache_rwlock);

    if (current_cache_size + size > MAX_CACHE_SIZE) {
        evict_lru(size);
    }

    CacheBlock *newCache = Malloc(sizeof(CacheBlock));
    newCache->data = Malloc(size);
    memcpy(newCache->data, data, size);

    newCache->size = size;

    newCache->uri = Malloc(strlen(uri) + 1);
    strcpy(newCache->uri, uri);

    if (head == NULL) {
        head = newCache;
        tail = newCache;
        newCache->prev = NULL;
        newCache->next = NULL;
        current_cache_size += size;
        pthread_rwlock_unlock(&cache_rwlock);
        return;
    }

    newCache->prev = NULL;
    newCache->next = head;
    head->prev = newCache;
    head = newCache;

    current_cache_size += size;
    pthread_rwlock_unlock(&cache_rwlock);
}

void *thread(void *vargp) {
    int connfd = *((int *)vargp);
    Pthread_detach(pthread_self());
    Free(vargp);
    doit(connfd);
    Close(connfd);
    return NULL;
}

void doit(int clientfd) {
    // 클라이언트 요청을 읽기 위한 버퍼들
    char buf[MAXBUF];     // 요청 라인 전체를 담을 버퍼
    char method[MAXLINE];  // HTTP 메소드 (예 : "GET")
    char uri[MAXLINE];     // 요청 URI (예 :"http://...")
    char version[MAXLINE]; // HTTP 버전 (예 :"HTTP/1.1")

    // URI에서 추출한 정보들
    char hostname[MAXLINE];  // 호스트 이름 (예: www.example.com)
    char path[MAXLINE];      // 경로 부분 (예: /index.html)
    char port[MAXLINE];      // 포트 번호 (예: 80, 1234)

    rio_t rio;               // robust I/O용 구조체 (클라이언트 -> 프록시)

    //  클라이언트 소켓을 rio 구조체에 연결
    Rio_readinitb(&rio, clientfd);

    //  클라이언트가 보낸 요청의 첫 줄 읽기 (예: GET http://... HTTP/1.1)
    Rio_readlineb(&rio, buf, MAXLINE); //

    // 요청 라인 파싱하여 method, uri, version 분리
    sscanf(buf, "%s %s %s", method, uri, version);
    printf("Proxy :: method=%s, uri=%s, version=%s\n", method, uri, version);

    CacheBlock *cb = find_cache(uri);
    if (cb) {
        move_to_head(cb);
        Rio_writen(clientfd, cb->data, cb->size);
        Close(clientfd);
        return;
    }

    // GET 이외의 지원하지 않는 메소드 처리
    if (strcasecmp(method, "GET")) {
        clienterror(clientfd, method, "501", "Not Implemented",
                "Proxy does not implement this method");
        return;
    }

    // URI에서 hostname, path, port를 추출
    parse_uri(uri, hostname, path, port);
    printf("hostname=%s, port=%s\n", hostname, port);

    // 실제 서버에 연결시도
    int serverfd = Open_clientfd(hostname, port);  //  Open_clientfd()는 hostname과 port를 기반으로 서버와 연결된 소켓을 리턴.
    if (serverfd < 0) {
        // 서버 연결 실패 시 로그 출력 후 함수 종료
        clienterror(clientfd, hostname, "502", "Bad Gateway",
                "Proxy couldn't connect to the server");
        return;
    }

    // HTTP 요청 라인 구성 (HTTP/1.0 사용해 간단하게 처리)
    char request_line[MAXLINE];
    sprintf(request_line, "GET %s HTTP/1.0\r\n", path);

    // 기본 필수 헤더 추가 (Host, User-Agent, Connection, Proxy-Connection)
    char header[MAXLINE];
    sprintf(header,
            "Host: %s\r\n"
            "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0) Gecko/20100101 Firefox/10.0\r\n"
            "Connection: close\r\n"
            "Proxy-Connection: close\r\n",
            hostname);


    //  클라이언트 요청의 나머지 헤더들을 읽어들임
    while (Rio_readlineb(&rio, buf, MAXLINE) > 0) {
        // 빈 줄은 헤더의 끝이므로 반복 종료
        if (!strcmp(buf, "\r\n")) break;

        // 중복 방지를 위해서 미리 위에서 설정한 헤더들은 무시하고 넘어감
        if (strncasecmp(buf, "Host", 4) == 0) continue;
        if (strncasecmp(buf, "User-Agent", 10) == 0) continue;
        if (strncasecmp(buf, "Connection", 10) == 0) continue;
        if (strncasecmp(buf, "Proxy-Connection", 16) == 0) continue;

        // 나머지 커스텀 헤더들은 그대로 추가
        strcat(header, buf);
    }

    // 빈줄 추가로 헤더의 끝 알리기 (HTTP 규격 상 필수)
    strcat(header, "\r\n");

    // 서버에 요청 라인과 헤더 전송
    Rio_writen(serverfd, request_line, strlen(request_line));
    Rio_writen(serverfd, header, strlen(header));

    // 서버로부터 응답을 읽을 rio 초기화
    rio_t server_rio;
    int n;
    Rio_readinitb(&server_rio, serverfd);

    char obj_buf[MAX_OBJECT_SIZE];
    int total_size = 0;

    // 서버에서 받은 데이터를 프록시 클라이언트에게 전달
    while ((n = Rio_readnb(&server_rio, buf, MAXBUF)) > 0) {
        Rio_writen(clientfd, buf, n);  //  프록시 -> 클라이언트

        if (total_size + n <= MAX_OBJECT_SIZE) {
            memcpy(obj_buf + total_size, buf, n);
            total_size += n;
        }
    }
    if (total_size <= MAX_OBJECT_SIZE) {
        insert_cache(uri, obj_buf, total_size);
    }
}

int parse_uri(char *uri, char *hostname, char *path, char *port) {
    //  URI 파싱에 필요한 포인터들 선언
    char *host_begin, *host_end, *port_begin, *path_begin;

    //  URI가 "http://"로 시작하면 그 뒤부터 host 부분 시작
    if (strncasecmp(uri, "http://", 7) == 0)
        host_begin = uri + 7;  // "http://"는 건너뜀
    else
        host_begin = uri;      //  "http://"가 없으면 그냥 uri 전체를 host_begin으로 취급

    // '/' 문자를 기준으로 path 시작 지점 찾기
    path_begin = strchr(host_begin, '/');
    if (path_begin) {
        // path가 존재하면 path 버퍼에 복사
        strcpy(path, path_begin);
    } else {
        // path가 없으면 "/"로 설정 (루트 경로 요청)
        strcpy(path, "/");
    }

    // host_end는 host부분의 끝 = path 시작 직전까지
    if (path_begin) {
        host_end = path_begin;
    } else {
        host_end = host_begin + strlen(host_begin);
    }

    // ':' 문자를 기준으로 포트 번호 시작 위치 찾기
    port_begin = strchr(host_begin, ':');

    // 포트가 명시된 경우 (예: 127.0.0.1:1234)
    if (port_begin && port_begin < host_end) {
        int hostname_len = port_begin - host_begin;     // 호스트 이름 길이 계산
        strncpy(hostname, host_begin, hostname_len);    // host 이름 복사
        hostname[hostname_len] = '\0';                  // 널 종료자 붙이기

        int port_len = host_end - port_begin - 1;       // 포트 번호 길이 계산
        strncpy(port, port_begin + 1, port_len);        // ':' 이후부터 포트 복사
        port[port_len] = '\0';                          // 널 종료자 붙이기
    } else {
        // 포트가 없는 경우 (기본 HTTP 포트인 80 사용)
        int hostname_len = host_end - host_begin;       // host 길이 계산
        strncpy(hostname, host_begin, hostname_len);
        hostname[hostname_len] = '\0';
        strcpy(port, "80");                             // 기본 포트 설정
    }

    return 0;
}

void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg) {
    char buf[MAXLINE], body[MAXBUF];

    // HTTP 응답 body 구성
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body + strlen(body), "<body bgcolor=\"ffffff\">\r\n");
    sprintf(body + strlen(body), "%s: %s\r\n", errnum, shortmsg);
    sprintf(body + strlen(body), "<p>%s: %s\r\n", longmsg, cause);
    sprintf(body + strlen(body), "<hr><em>The Tiny Web proxy</em>\r\n");

    // HTTP 응답 헤더
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %lu\r\n\r\n", strlen(body));
    Rio_writen(fd, buf, strlen(buf));

    // 본문 전송
    Rio_writen(fd, body, strlen(body));
}
