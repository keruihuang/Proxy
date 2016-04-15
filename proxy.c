/*
 * proxy.c - COMP 321 Web proxy
 *
 * TEAM NAME: Kamehameha
 * TEAM MEMBERS:
 *     Kerui Huang, kh24@rice.edu 
 *     
 */ 

#include "csapp.h"

/* Task args */
struct task {
	int fd;
	struct sockaddr_in sockaddr;
};

/* Mutex */
static sem_t open_clientfd_mutex;
static sem_t log_mutex;

/* Log file */
FILE *pLog;

/*
 * Function prototypes
 */
void do_Proxy(int fd);
void read_headers(rio_t *rp, char *headers, int *length, int *chunked);
int parse_uri(char *uri, char *target_addr, char *path, int *port);
int parse_chunked_headers(char *chunked_header);
static void client_error(int fd, const char *cause, int err_num, const char *short_msg, const char *long_msg);

void *thread(void *vargp);
void Rio_writen_w(int fd, void *usrbuf, size_t n);
ssize_t Rio_readnb_w(rio_t *rp, void *usrbuf, size_t n);
ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen);
int open_clientfd_ts(char *hostname, int port);

/* 
 * main
 *
 * Requires:
 *   <to be filled in by the student(s)> 
 *
 * Effects:
 *   <to be filled in by the student(s)> 
 */
int main(int argc, char **argv)
{
    int listenfd, connfd, port;
    socklen_t clientlen;
    pthread_t tid;
    struct sockaddr_in clientaddr;

    /* Check arguments */
    if (argc != 2) {
    	fprintf(stderr, "Usage: %s <port number>\n", argv[0]);
    	exit(0);
    }
    port = atoi(argv[1]);

    /* Initial mutex */
    Sem_init(&open_clientfd_mutex, 0, 1);
    Sem_init(&log_mutex, 0, 1);

    /* Ignore SIGPIPE signals */
    Signal(SIGPIPE, SIG_IGN);

    /* Open log file */
    pLog = fopen("proxy.log", "a");

    /* Listen */
    listenfd = Open_listenfd(port);
    printf("Proxy is running...\n");
    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        /* Pass args */
        struct task *vargp= (struct task *) Malloc(sizeof(struct task));
        vargp->fd = connfd;
        vargp->sockaddr = clientaddr;
        /* Create thread */
        Pthread_create(&tid, NULL, thread, vargp);
    }
    exit(0);
}

void *thread(void *vargp)
{
	Pthread_detach(pthread_self());
	struct task *thread_task = (struct task *) vargp;
	do_Proxy(thread_task->fd);
	close(thread_task->fd);
    Free(vargp);
	return NULL;
}

/*
 * do_Proxy - handles one HTTP transaction
 */
void do_Proxy(int fd)
{
    int serverfd, port, content_length, chunked_encode, chunked_length, size = 0;
    char hostname[MAXLINE], pathname[MAXLINE], buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE], logstring[MAXLINE];
    char headers[MAXBUF], request[MAXBUF], response[MAXBUF];
    rio_t rio_client, rio_server;

    /* Read request line and headers */
    Rio_readinitb(&rio_client, fd);
    if (Rio_readlineb_w(&rio_client, buf, MAXLINE) <= 0)
    	return;

    /* Get request type*/
    sscanf(buf, "%s %s %s", method, uri, version);
    if (strcmp(method, "POST") && strcmp(method, "GET")) {
        client_error(fd, uri, 502, "Proxy error", "Proxy doesn't implement this method");
        return;
    }

    /* Get full headers */
    read_headers(&rio_client, headers, &content_length, &chunked_encode);

    /* Parse URI from request */
    if (parse_uri(uri, hostname, pathname, &port) == -1) {
        client_error(fd, uri, 502, "Proxy error", "Proxy doesn't implement this uri");
        return;
    }

    /* Build HTTP request */
    sprintf(request, "%s /%s %s\r\n%s", method, pathname, version, headers);

    /* Send HTTP resquest to the web server */
    if ((serverfd = open_clientfd_ts(hostname, port)) == -1)
    	return;
    Rio_writen_w(serverfd, request, strlen(request));
    if (strcmp(method, "POST") == 0) {	/* POST request */
    	Rio_readnb_w(&rio_client, buf, content_length);
    	Rio_writen_w(serverfd, buf, content_length);
    }

    /* Get response header */
    Rio_readinitb(&rio_server, serverfd);
    read_headers(&rio_server, response, &content_length, &chunked_encode);

    /* Send HTTP response to the client */
    Rio_writen_w(fd, response, strlen(response));

    /* Send response content to the client */
    if (chunked_encode) {	                      /* Encode with chunk */
    	Rio_readlineb_w(&rio_server, buf, MAXLINE);
    	Rio_writen_w(fd, buf, strlen(buf));
    	while ((chunked_length = parse_chunked_headers(buf)) > 0) {
            size += chunked_length;
    		Rio_readnb_w(&rio_server, buf, chunked_length);
    		Rio_writen_w(fd, buf, chunked_length);
    		Rio_readlineb_w(&rio_server, buf, MAXLINE);
    		Rio_writen_w(fd, buf, strlen(buf));
    		Rio_readlineb_w(&rio_server, buf, MAXLINE);
    		Rio_writen_w(fd, buf, strlen(buf));
    	}
    	Rio_readlineb_w(&rio_server, buf, MAXLINE);
    	Rio_writen_w(fd, buf, strlen(buf));
    } else if (content_length > 0) {				/* Define length with Content-length */
        size += content_length;
        int left_length = content_length;
        int handle_length = 0;
    	while (left_length > 0) {
            handle_length = left_length > MAXBUF ? MAXBUF : left_length;
            left_length -= handle_length;
            Rio_readnb_w(&rio_server, buf, handle_length);
            Rio_writen_w(fd, buf, handle_length);
        }
    } else {                                        /* Define length with closing connection */
    	while ((chunked_length = Rio_readlineb_w(&rio_server, buf, MAXBUF)) > 0) {
            size += chunked_length;
    		Rio_writen_w(fd, buf, chunked_length);
        }
    }

    /* Write log file */
    P(&log_mutex);

    fprintf(pLog, "%s\n", logstring);
    V(&log_mutex);

    /* Close connection to server */
    close(serverfd);
}

/*
 * client_error
 *
 * Requires:
 *   The parameter "fd" must be an open socket that is connected to the client.
 *   The parameters "cause", "short_msg", and "long_msg" must point to properly 
 *   NUL-terminated strings that describe the reason why the HTTP transaction
 *   failed.  The string "short_msg" may not exceed 32 characters in length,
 *   and the string "long_msg" may not exceed 80 characters in length.
 *
 * Effects:
 *   Constructs an HTML page describing the reason why the HTTP transaction
 *   failed, and writes an HTTP/1.0 response containing that page as the
 *   content.  The cause appearing in the HTML page is truncated if the
 *   string "cause" exceeds 2048 characters in length.
 */
 static void
client_error(int fd, const char *cause, int err_num, const char *short_msg,
    const char *long_msg)
{
	char body[MAXBUF], headers[MAXBUF], truncated_cause[2049];

	assert(strlen(short_msg) <= 32);
	assert(strlen(long_msg) <= 80);
	/* Ensure that "body" is much larger than "truncated_cause". */
	assert(sizeof(truncated_cause) < MAXBUF / 2);

	/*
	 * Create a truncated "cause" string so that the response body will not
	 * exceed MAXBUF.
	 */
	strncpy(truncated_cause, cause, sizeof(truncated_cause) - 1);
	truncated_cause[sizeof(truncated_cause) - 1] = '\0';

	/* Build the HTTP response body. */
	snprintf(body, MAXBUF,
	    "<html><title>Proxy Error</title><body bgcolor=""ffffff"">\r\n"
	    "%d: %s\r\n"
	    "<p>%s: %s\r\n"
	    "<hr><em>The COMP 321 Web proxy</em>\r\n",
	    err_num, short_msg, long_msg, truncated_cause);

	/* Build the HTTP respons	e headers. */
	snprintf(headers, MAXBUF,
	    "HTTP/1.0 %d %s\r\n"
	    "Content-type: text/html\r\n"
	    "Content-length: %d\r\n"
	    "\r\n",
	    err_num, short_msg, (int)strlen(body));

	/* Write the HTTP response. */
	if (rio_writen(fd, headers, strlen(headers)) != -1)
		rio_writen(fd, body, strlen(body));
}

/*
 * read_header - get request header
 *
 * Read headers of request. Return -1 if there is any problem.
 */
 void read_headers(rio_t *rp, char *content, int *length, int *chunked)
 {
    char buf[MAXLINE];
    *length = *chunked = 0;

    Rio_readlineb_w(rp, buf, MAXLINE);
    strcpy(content, buf);
    strcat(content, "Connection: close\r\n");
    while (strcmp(buf, "\r\n")) {
        Rio_readlineb_w(rp, buf, MAXLINE);
        /* Get 'Content-Length:' */
        if (strncasecmp(buf, "Content-Length:", 15) == 0)
            *length = atoi(buf + 15);
        /* Get 'Transfer-Encoding: chunked' */
        if (strncasecmp(buf, "Transfer-Encoding: chunked", 26) == 0)
        	*chunked = 1;
        /* Remove 'Connection' and 'Proxy-Connection' */
        if (strncasecmp(buf, "Proxy-Connection:", 17) == 0 || strncasecmp(buf, "Connection:", 11) == 0)
            continue;
        strcat(content, buf);
    }
 }

/*
 * parse_uri
 * 
 * Requires:
 *   The parameter "uri" must point to a properly NUL-terminated string.
 *
 * Effects:
 *   Given a URI from an HTTP proxy GET request (i.e., a URL), extract the
 *   host name, port, and path name.  Create strings containing the host name,
 *   port, and path name, and return them through the parameters "hostnamep",
 *   "portp", "pathnamep", respectively.  (The caller must free the memory
 *   storing these strings.)  Return -1 if there are any problems and 0
 *   otherwise.
 */
int parse_uri(char *uri, char *hostname, char *pathname, int *port)
{
    char *host_start;
    char *host_finish;
    char *path_start;
    int len;

    if (strncasecmp(uri, "http://", 7) != 0) {
    	hostname[0] = '\0';
    	return -1;
    }

    /* Extract the host name */
    host_start = uri + 7;
    host_finish = strpbrk(host_start, " :/\r\n\0");
    len = host_finish - host_start;
    strncpy(hostname, host_start, len);
    hostname[len] = '\0';

    /* Extract the port number */
    *port = 80; /* default */
    if (*host_finish == ':')   
	*port = atoi(host_finish + 1);

    /* Extract the path */
    path_start = strchr(host_start, '/');
    if (path_start == NULL)
	   pathname[0] = '\0';
    else {
	   path_start++;	
	   strcpy(pathname, path_start);
    }

    return 0;
}

/*
 * format_log_entry - Create a formatted log entry in logstring. 
 * 
 * The inputs are the socket address of the requesting client
 * (sockaddr), the URI from the request (uri), and the size in bytes
 * of the response from the server (size).
 */


/*
 * Parse chunked header - Get chunked length from header 
 */
int parse_chunked_headers(char *chunked_header)
{
	char charactor;
	int i, length = 0;
	for (i = 0; (charactor = chunked_header[i]) != '\r'; i++)
		if (isdigit(charactor))
			length = length*16 + charactor - '0';
		else if (charactor >= 'A' && charactor <= 'F')
			length = length*16 + charactor - 'A' + 10;
        else if (charactor >= 'a' && charactor <= 'f')
            length = length*16 + charactor - 'a' + 10;
		else
			return -1;
	return length;
}

/*************************************
 * Robust I/O routines wrapper for web
 *************************************/
void Rio_writen_w(int fd, void *usrbuf, size_t n) 
{
    if (rio_writen(fd, usrbuf, n) != (long)n)
        fprintf(stderr, "Rio_writen_w error: %s\n", strerror(errno));
}

ssize_t Rio_readnb_w(rio_t *rp, void *usrbuf, size_t n) 
{
    ssize_t rc;

    if ((rc = rio_readnb(rp, usrbuf, n)) < 0) {
        fprintf(stderr, "Rio_readnb_w error: %s\n", strerror(errno));
        return 0;
    }
    return rc;
}

ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen) 
{
    ssize_t rc;

    if ((rc = rio_readlineb(rp, usrbuf, maxlen)) < 0) {
        fprintf(stderr, "Rio_readlineb_w error: %s\n", strerror(errno));
        return 0;
    }
    return rc;
} 

/*
 * open_clientfd (thread safe version) - open connection to server at <hostname, port> 
 *   and return a socket descriptor ready for reading and writing.
 *   Returns -1 and sets errno on Unix error. 
 *   Returns -2 and sets h_errno on DNS (gethostbyname) error.
 */
/* $begin open_clientfd_ts */
int open_clientfd_ts(char *hostname, int port) 
{
    int clientfd;
    struct hostent *hp, *sharedp;
    struct sockaddr_in serveraddr;

    if ((clientfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    return -1; /* check errno for cause of error */

    /* Fill in the server's IP address and port */
    P(&open_clientfd_mutex);
    if ((sharedp = gethostbyname(hostname)) == NULL) {
    	V(&open_clientfd_mutex);	/* very important */
    	return -2; 					/* check h_errno for cause of error */
    }
    hp = (struct hostent*) malloc(sizeof(struct hostent));
    memcpy(hp, sharedp, sizeof(struct hostent));
    V(&open_clientfd_mutex);
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)hp->h_addr_list[0], 
      (char *)&serveraddr.sin_addr.s_addr, hp->h_length);
    serveraddr.sin_port = htons(port);
    free(hp);

    /* Establish a connection with the server */
    if (connect(clientfd, (SA *) &serveraddr, sizeof(serveraddr)) < 0)
    return -1;
    return clientfd;
}
/*
 * The last lines of this file configure the behavior of the "Tab" key in
 * emacs.  Emacs has a rudimentary understanding of C syntax and style.  In
 * particular, depressing the "Tab" key once at the start of a new line will
 * insert as many tabs and/or spaces as are needed for proper indentation.
 */

/* Local Variables: */
/* mode: c */
/* c-default-style: "bsd" */
/* c-basic-offset: 8 */
/* c-continued-statement-offset: 4 */
/* indent-tabs-mode: t */
/* End: */
