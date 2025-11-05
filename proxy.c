#include <stdio.h>
#include "csapp.h"
#include "sbuf.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define CACHE_LINE 10

#define NTHREADS 4
#define SBUFSIZE 16

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

/* Cache structure */
typedef struct
{
    char buf[MAX_OBJECT_SIZE];
    char url[MAXLINE];
    int size;
    int valid;
    int timestamp;
} cacheLine;

typedef struct
{
    cacheLine line[CACHE_LINE];
    int readcnt;
    int current_time;
    sem_t mutex;  /* Protects readcnt */
    sem_t writer; /* Protects cache writes */
} cache_t;

cache_t cache;

/* Function prototypes */
static void handle_client(int connfd);
static int parse_uri(const char *uri, char *host, char *port, char *path);
static void build_request(char *dst, size_t dstsz,
                          const char *path, const char *host,
                          rio_t *client_rio);
static void client_error(int fd, const char *cause, const char *errnum,
                         const char *shortmsg, const char *longmsg);
void *thread(void *vargp);

/* Cache functions */
void cache_init();
int find_cache_hit(char *url);
void write_cache(char *buf, char *url, int size);
void read_cache(int idx, char *buf);

sbuf_t sbuf;

int main(int argc, char **argv)
{
    int listenfd, connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    printf("%s\n", user_agent_hdr);
    listenfd = Open_listenfd(argv[1]);
    sbuf_init(&sbuf, SBUFSIZE);
    cache_init();

    /* Create worker threads */
    for (int i = 0; i < NTHREADS; ++i)
        Pthread_create(&tid, NULL, thread, NULL);

    while (1)
    {
        clientlen = sizeof(struct sockaddr_storage);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        sbuf_insert(&sbuf, connfd);
    }
    return 0;
}

void *thread(void *vargp)
{
    Pthread_detach(pthread_self());
    while (1)
    {
        int connfd = sbuf_remove(&sbuf);
        handle_client(connfd);
        Close(connfd);
    }
}

/* Initialize cache */
void cache_init()
{
    cache.readcnt = 0;
    cache.current_time = 0;
    Sem_init(&cache.mutex, 0, 1);
    Sem_init(&cache.writer, 0, 1);
    for (int i = 0; i < CACHE_LINE; i++)
    {
        cache.line[i].valid = 0;
        cache.line[i].timestamp = 0;
        cache.line[i].size = 0;
    }
}

/* Find cache hit - returns index if found, -1 otherwise */
int find_cache_hit(char *url)
{
    int ret = -1;
    for (int i = 0; i < CACHE_LINE; i++)
    {
        if (cache.line[i].valid && !strcmp(cache.line[i].url, url))
        {
            ret = i;
            break;
        }
    }
    return ret;
}

/* Read from cache with readers-writers protocol */
void read_cache(int idx, char *buf)
{
    P(&cache.mutex);
    cache.readcnt++;
    if (cache.readcnt == 1)
        P(&cache.writer);
    V(&cache.mutex);

    /* Critical section - reading */
    memcpy(buf, cache.line[idx].buf, cache.line[idx].size);

    /* Update timestamp (use trywait to avoid blocking) */
    if (sem_trywait(&cache.writer) == 0)
    {
        cache.line[idx].timestamp = ++cache.current_time;
        V(&cache.writer);
    }

    P(&cache.mutex);
    cache.readcnt--;
    if (cache.readcnt == 0)
        V(&cache.writer);
    V(&cache.mutex);
}

/* Write to cache with LRU eviction */
void write_cache(char *buf, char *url, int size)
{
    if (size > MAX_OBJECT_SIZE)
        return;

    P(&cache.writer);

    int idx = -1;
    /* Find empty slot */
    for (int i = 0; i < CACHE_LINE; i++)
    {
        if (cache.line[i].valid == 0)
        {
            idx = i;
            break;
        }
    }

    /* LRU eviction if no empty slot */
    if (idx == -1)
    {
        int max_time = 0;
        for (int i = 0; i < CACHE_LINE; i++)
        {
            if (cache.line[i].valid &&
                cache.current_time - cache.line[i].timestamp > max_time)
            {
                max_time = cache.current_time - cache.line[i].timestamp;
                idx = i;
            }
        }
    }

    /* Write to cache */
    memcpy(cache.line[idx].buf, buf, size);
    strcpy(cache.line[idx].url, url);
    cache.line[idx].size = size;
    cache.line[idx].timestamp = ++cache.current_time;
    cache.line[idx].valid = 1;

    V(&cache.writer);
}

/* Handle a single HTTP transaction with caching */
static void handle_client(int connfd)
{
    rio_t crio, srio;
    char buf[MAXLINE];
    char method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char host[MAXLINE], port[MAXLINE], path[MAXLINE];
    char cache_buf[MAX_OBJECT_SIZE];
    int object_size = 0;

    Rio_readinitb(&crio, connfd);

    /* Read request line */
    if (Rio_readlineb(&crio, buf, MAXLINE) <= 0)
        return;

    if (sscanf(buf, "%s %s %s", method, uri, version) != 3)
    {
        client_error(connfd, "bad request line", "400", "Bad Request",
                     "Proxy could not parse the request line");
        return;
    }

    if (strcasecmp(method, "GET"))
    {
        client_error(connfd, method, "501", "Not Implemented",
                     "Proxy does not implement this method");
        return;
    }

    /* Check cache first */
    int cache_idx = find_cache_hit(uri);
    if (cache_idx != -1)
    {
        char cached_response[MAX_OBJECT_SIZE];
        read_cache(cache_idx, cached_response);
        Rio_writen(connfd, cached_response, cache.line[cache_idx].size);
        return;
    }

    /* Parse URL */
    if (parse_uri(uri, host, port, path) < 0)
    {
        client_error(connfd, uri, "400", "Bad Request",
                     "Proxy could not parse the URI");
        return;
    }

    /* Build outbound request */
    char outreq[MAXBUF];
    build_request(outreq, sizeof(outreq), path, host, &crio);

    /* Connect to end server */
    int serverfd = Open_clientfd(host, port);
    if (serverfd < 0)
    {
        client_error(connfd, host, "502", "Bad Gateway",
                     "Proxy could not connect to end server");
        return;
    }

    Rio_readinitb(&srio, serverfd);
    Rio_writen(serverfd, outreq, strlen(outreq));

    /* Relay response and accumulate for caching */
    ssize_t n;
    while ((n = Rio_readnb(&srio, buf, sizeof(buf))) > 0)
    {
        Rio_writen(connfd, buf, n);

        /* Accumulate in cache buffer if within size limit */
        if (object_size + n <= MAX_OBJECT_SIZE)
        {
            memcpy(cache_buf + object_size, buf, n);
            object_size += n;
        }
        else
        {
            object_size = MAX_OBJECT_SIZE + 1; /* Mark as too large */
        }
    }

    /* Cache the object if it's within size limit */
    if (object_size > 0 && object_size <= MAX_OBJECT_SIZE)
    {
        write_cache(cache_buf, uri, object_size);
    }

    Close(serverfd);
}

/* Parse URI */
static int parse_uri(const char *uri, char *host, char *port, char *path)
{
    const char *u = uri;
    const char *p;

    strcpy(port, "80");

    if (!strncasecmp(u, "http://", 7))
        u += 7;
    else if (*u == '/')
        return -1;

    p = strchr(u, '/');
    if (p)
    {
        strncpy(host, u, p - u);
        host[p - u] = '\0';
        strncpy(path, p, MAXLINE - 1);
        path[MAXLINE - 1] = '\0';
    }
    else
    {
        strncpy(host, u, MAXLINE - 1);
        host[MAXLINE - 1] = '\0';
        strcpy(path, "/");
    }

    char *colon = strchr(host, ':');
    if (colon)
    {
        *colon = '\0';
        strncpy(port, colon + 1, MAXLINE - 1);
        port[MAXLINE - 1] = '\0';
        for (char *q = port; *q; ++q)
        {
            if (*q < '0' || *q > '9')
            {
                *q = '\0';
                break;
            }
        }
        if (port[0] == '\0')
            strcpy(port, "80");
    }

    if (host[0] == '\0')
        return -1;
    if (path[0] == '\0')
        strcpy(path, "/");
    return 0;
}

/* Build HTTP/1.0 request */
static void build_request(char *dst, size_t dstsz,
                          const char *path, const char *host,
                          rio_t *client_rio)
{
    size_t nused = 0;
    char line[MAXLINE];

    nused += snprintf(dst + nused, dstsz - nused,
                      "GET %s HTTP/1.0\r\n", path);

    char other[MAXBUF];
    size_t other_used = 0;

    while (Rio_readlineb(client_rio, line, sizeof(line)) > 0)
    {
        if (!strcmp(line, "\r\n"))
            break;

        if (!strncasecmp(line, "Host:", 5))
            continue;
        if (!strncasecmp(line, "User-Agent:", 11))
            continue;
        if (!strncasecmp(line, "Connection:", 11))
            continue;
        if (!strncasecmp(line, "Proxy-Connection:", 17))
            continue;

        if (other_used + strlen(line) < sizeof(other))
        {
            memcpy(other + other_used, line, strlen(line));
            other_used += strlen(line);
        }
    }
    other[other_used] = '\0';

    nused += snprintf(dst + nused, dstsz - nused,
                      "Host: %s\r\n", host);
    nused += snprintf(dst + nused, dstsz - nused, "%s", user_agent_hdr);
    nused += snprintf(dst + nused, dstsz - nused,
                      "Connection: close\r\n");
    nused += snprintf(dst + nused, dstsz - nused,
                      "Proxy-Connection: close\r\n");

    if (other_used && nused + other_used < dstsz)
    {
        memcpy(dst + nused, other, other_used);
        nused += other_used;
    }

    if (nused + 2 < dstsz)
    {
        dst[nused++] = '\r';
        dst[nused++] = '\n';
    }
    dst[nused] = '\0';
}

/* Send HTTP error to client */
static void client_error(int fd, const char *cause, const char *errnum,
                         const char *shortmsg, const char *longmsg)
{
    char body[MAXBUF], hdr[MAXBUF];

    snprintf(body, sizeof(body),
             "<html><title>Proxy Error</title>"
             "<body bgcolor=\"ffffff\">\r\n"
             "%s: %s\r\n"
             "<p>%s\r\n"
             "<hr><em>CS:APP Proxy</em>\r\n"
             "</body></html>",
             errnum, shortmsg, longmsg);

    snprintf(hdr, sizeof(hdr),
             "HTTP/1.0 %s %s\r\n"
             "Content-type: text/html\r\n"
             "Content-length: %zu\r\n\r\n",
             errnum, shortmsg, strlen(body));

    Rio_writen(fd, hdr, strlen(hdr));
    Rio_writen(fd, body, strlen(body));
}
