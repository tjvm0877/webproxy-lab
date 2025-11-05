#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

static void handle_client(int connfd);
static int parse_uri(const char *uri, char *host, char *port, char *path);
static void build_request(char *dst, size_t dstsz,
                          const char *path, const char *host,
                          rio_t *client_rio);
static void client_error(int fd, const char *cause, const char *errnum,
                         const char *shortmsg, const char *longmsg);

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    printf("%s\n", user_agent_hdr);

    int listenfd = Open_listenfd(argv[1]);

    while (1)
    {
        struct sockaddr_storage clientaddr;
        socklen_t clientlen = sizeof(clientaddr);
        int connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);

        char hostname[MAXLINE], port[MAXLINE];
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE,
                    port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        handle_client(connfd);
        Close(connfd);
    }
    return 0;
}

/* Handle a single HTTP transaction sequentially */
static void handle_client(int connfd)
{
    rio_t crio, srio;
    char buf[MAXLINE];
    char method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char host[MAXLINE], port[MAXLINE], path[MAXLINE];

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

    /* Parse URL -> host, port, path */
    if (parse_uri(uri, host, port, path) < 0)
    {
        client_error(connfd, uri, "400", "Bad Request",
                     "Proxy could not parse the URI");
        return;
    }

    /* Build outbound request (HTTP/1.0 + required headers) */
    char outreq[MAXBUF];
    build_request(outreq, sizeof(outreq), path, host, &crio);

    /* Connect to end server and forward */
    int serverfd = Open_clientfd(host, port);
    if (serverfd < 0)
    {
        client_error(connfd, host, "502", "Bad Gateway",
                     "Proxy could not connect to end server");
        return;
    }

    Rio_readinitb(&srio, serverfd);

    /* Send request to server */
    Rio_writen(serverfd, outreq, strlen(outreq));

    /* Relay response to client (headers + body) */
    ssize_t n;
    while ((n = Rio_readnb(&srio, buf, sizeof(buf))) > 0)
    {
        Rio_writen(connfd, buf, n);
    }

    Close(serverfd);
}

/* Parse URI of forms:
   http://host[:port]/path...
   /relative/path (rare from proxy clients, but handle conservatively)
   Returns 0 on success, -1 on failure.
*/
static int parse_uri(const char *uri, char *host, char *port, char *path)
{
    const char *u = uri;
    const char *p;

    strcpy(port, "80");

    /* Absolute URI starting with http:// or https:// (treat https like http: CONNECT not required for Part I) */
    if (!strncasecmp(u, "http://", 7))
        u += 7;
    else if (*u == '/')
        /* Only path provided; host must come from Host header later.
           For simplicity in Part I, reject if no absolute URI. */
        return -1;

    /* host[:port][/path] */
    /* Find path start */
    p = strchr(u, '/');
    if (p)
    {
        strncpy(host, u, p - u);
        host[p - u] = '\0';
        /* path includes leading '/' */
        strncpy(path, p, MAXLINE - 1);
        path[MAXLINE - 1] = '\0';
    }
    else
    {
        /* no path => "/" */
        strncpy(host, u, MAXLINE - 1);
        host[MAXLINE - 1] = '\0';
        strcpy(path, "/");
    }

    /* Extract optional :port in host */
    char *colon = strchr(host, ':');
    if (colon)
    {
        *colon = '\0';
        strncpy(port, colon + 1, MAXLINE - 1);
        port[MAXLINE - 1] = '\0';
        /* If port contains trailing garbage like path (shouldn't), truncate at first non-digit */
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

/* Build HTTP/1.0 request with required headers.
   - Start line: GET <path> HTTP/1.0
   - Host: <host>
   - User-Agent: <provided constant>
   - Connection: close
   - Proxy-Connection: close
   - Forward other headers from client except the above four (which we control)
*/
static void build_request(char *dst, size_t dstsz,
                          const char *path, const char *host,
                          rio_t *client_rio)
{
    size_t nused = 0;
    char line[MAXLINE];
    int has_host = 0;

    /* Request line */
    nused += snprintf(dst + nused, dstsz - nused,
                      "GET %s HTTP/1.0\r\n", path);

    /* We will collect other headers to append later */
    char other[MAXBUF];
    size_t other_used = 0;

    /* Read and filter client headers until empty line */
    while (Rio_readlineb(client_rio, line, sizeof(line)) > 0)
    {
        if (!strcmp(line, "\r\n"))
            break;

        if (!strncasecmp(line, "Host:", 5))
        {
            has_host = 1;
            /* Replace by our Host below; but if needed, we could keep the same value.
               For safety, ignore this one to avoid duplicates. */
            continue;
        }
        if (!strncasecmp(line, "User-Agent:", 11))
            continue;
        if (!strncasecmp(line, "Connection:", 11))
            continue;
        if (!strncasecmp(line, "Proxy-Connection:", 17))
            continue;

        /* Append untouched */
        if (other_used + strlen(line) < sizeof(other))
        {
            memcpy(other + other_used, line, strlen(line));
            other_used += strlen(line);
        }
    }
    other[other_used] = '\0';

    /* Mandatory headers */
    nused += snprintf(dst + nused, dstsz - nused,
                      "Host: %s\r\n", host);
    nused += snprintf(dst + nused, dstsz - nused, "%s", user_agent_hdr);
    nused += snprintf(dst + nused, dstsz - nused,
                      "Connection: close\r\n");
    nused += snprintf(dst + nused, dstsz - nused,
                      "Proxy-Connection: close\r\n");

    /* Other headers from client (if any) */
    if (other_used && nused + other_used < dstsz)
    {
        memcpy(dst + nused, other, other_used);
        nused += other_used;
    }

    /* End of headers */
    if (nused + 2 < dstsz)
    {
        dst[nused++] = '\r';
        dst[nused++] = '\n';
    }
    dst[nused] = '\0';
}

/* Send simple HTTP error to client */
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