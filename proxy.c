/*
 * proxy.c - COMP 321 Web proxy
 *
 * TEAM NAME: Kamehameha
 * TEAM MEMBERS:
 *     Kerui Huang, kh24@rice.edu
 *     Nam Hee Kim, nk17@rice.edu
 *
 */

#include "csapp.h"
#include <assert.h>
#include <malloc.h>

#define BUF_SIZE 128	/* Per-connection internal buffer size. */

/* Task args */
struct task {
	int fd;
	struct sockaddr_in sockaddr;
};

/* List structure for multiple buffer parsing consequences */

struct List {
	struct List *next;
	char *str;
	unsigned long char_count;
};

/* Mutex */
static sem_t open_clientfd_mutex;
static sem_t log_mutex;

/* Request counter */
static int reqcount;

/* Log file */
FILE *pLog;

/*
 * Function prototypes
 */

void do_Proxy(struct task *thread_task, const int reqnum);
void read_headers(rio_t *rp, char *headers, int *length, int *chunked);
int parse_uri(char *uri, char *target_addr, char *path, int *port);
int parse_chunked_headers(char *chunked_header);
static void client_error(int fd, const char *cause,
	int err_num, const char *short_msg, const char *long_msg);

static char    *create_log_entry(const struct sockaddr_in *sockaddr,
		    const char *uri, int size);

void *thread(void *vargp);
ssize_t Rio_writen_w(int fd, void *usrbuf, size_t n);
ssize_t Rio_readnb_w(rio_t *rp, void *usrbuf, size_t n);
ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen);
int open_clientfd_ts(char *hostname, int port);

/* For list interface */
struct List* list_create(void);
void list_insert(struct List* lp, char* newelem);
void list_destroy(struct List* lp);
char* list_totalstring(struct List* lp);

/*
 * main
 *
 * Requires:
 *   Only one argument, the port number, must be specified.
 *
 * Effects:
 *   Runs a master proxy server that handles different requests from
 *   various locations concurrently.
 */
int main(int argc, char **argv)
{
    int listenfd, connfd, port;
    socklen_t clientlen;
    pthread_t tid;

		// pid_t pid;
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


    /* Listen */
    listenfd = Open_listenfd(argv[1]);
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

/*
	Individual thread behavior definition
	Requires:
		"vargp" must point to a valid argument structure,
		namely "struct task."
	Effects:
		For this thread, execute the proxy task, close all open file descriptors,
		free the argument object, and terminate the thread.
*/
void *thread(void *vargp)
{
	Pthread_detach(pthread_self());
	struct task *thread_task = (struct task *) vargp;
	do_Proxy(thread_task, reqcount++);
	close(thread_task->fd);
  Free(vargp);
	return(NULL);
}

/*
 * do_Proxy - handles one HTTP transaction
	Requires:
		"thread_task" must be a valid argument object.
		"reqnum" must be an integer greater than 0, identifying the request
		this thread deals with 1-on-1.
	Effects:
		Execute the proxy task by
			1. reading in the input request
			2. delivering the request to the server
			3. retrieving the response
			4. delivering the response to the client
		and then log the result.
 */
void do_Proxy(struct task *thread_task, const int reqnum)
{
    int serverfd, port, content_length, chunked_encode, chunked_length;
		int size = 0;
		int fd;
		char hostname[MAXLINE], pathname[MAXLINE], buf[MAXLINE], method[MAXLINE];
		char uri[MAXLINE], version[MAXLINE];
    char headers[MAXBUF], request[MAXBUF], response[MAXBUF];
		char *logstring;
		rio_t rio_client, rio_server;

		fd = thread_task->fd;

		/* Client tracking */
		char client_ip_dec[INET_ADDRSTRLEN]; // client's IP address string
		struct sockaddr_in *sockaddr = &thread_task->sockaddr; // client's socket

		// printf("reqnum: %d\n", reqnum);
		if (reqnum < 0) {
			printf("Corrupt request: %d\n", reqnum);
			return;
		}
    /* Read request line and headers */
    Rio_readinitb(&rio_client, fd);

		ssize_t readcount;

		// read all the chunked input
		struct List *readlist;
		readlist = list_create();
		while ((readcount = Rio_readlineb_w(&rio_client, buf, MAXLINE)) >= 0) {
			if (readcount <= 0) {
				printf("Request %d: EOF reached\n", reqnum);
				return;
			}
			printf("%d bytes were read as a result of request parsing\n",
							(int)readcount);
			char* dbuf = Malloc(sizeof(char) * readcount + 1); //dynamic buffer
			strcpy(dbuf, buf);
			list_insert(readlist, dbuf);
			if (readcount <= MAXLINE) {
				break;
			}
		}
		// we have list of strings
		char* totalbuf = list_totalstring(readlist);
		printf("total string: %s", totalbuf);

    /* Get request type*/
    sscanf(totalbuf, "%s %s %s", method, uri, version);
    if (strcmp(method, "POST") && strcmp(method, "GET")) {
        client_error(fd, uri, 502, "Proxy error",
					"Proxy doesn't implement this method");
        return;
    }


		list_destroy(readlist);

    /* Get full headers */
    read_headers(&rio_client, headers, &content_length, &chunked_encode);

    /* Parse URI from request */
    if (parse_uri(uri, hostname, pathname, &port) == -1) {
        client_error(fd, uri, 502,
					"Proxy error", "Proxy doesn't implement this uri");
        return;
    }

	Inet_ntop(AF_INET, &sockaddr->sin_addr, client_ip_dec, INET_ADDRSTRLEN);
	printf("Request %d: Received request from %s:\n", reqnum, client_ip_dec);
	printf("%s%s", buf, headers);
	printf("*** End of Request ***\n");

	if (strcmp(method, "GET") != 0) {
		printf("Request %d: Received non-GET request\n", reqnum);
	}

    /* Build HTTP request */
    sprintf(request, "%s /%s %s\r\n%s", method, pathname, version, headers);

    /* Send HTTP resquest to the web server */
    if ((serverfd = open_clientfd_ts(hostname, port)) == -1) {
			return;
	}
		
    if (Rio_writen_w(serverfd, request, strlen(request)) < 0) {
		/* Writing to server fails */
		client_error(fd, uri, 504, "Gateway Timeout", "Unrecognized host name or port");
		close(serverfd);
		return;
	}
	
    if (strcmp(method, "POST") == 0) {	/* POST request */
    	Rio_readnb_w(&rio_client, buf, content_length);
    	Rio_writen_w(serverfd, buf, content_length);
    }

		/** End of Request Handling **/




		/** Handle Responses **/

    /* Get response header */
    Rio_readinitb(&rio_server, serverfd);
    read_headers(&rio_server, response, &content_length, &chunked_encode);

    /* Send HTTP response to the client */
    Rio_writen_w(fd, response, strlen(response));
		size = strlen(response);

    /* Send response content to the client */
    if (chunked_encode) {
      /* Encode with chunk */
		printf("chunked case\n");
		if (Rio_readlineb_w(&rio_server, buf, MAXLINE) <= 0) {
			printf("error after chunked encode\n");
			close(serverfd);
			return;
		}
		Rio_writen_w(fd, buf, strlen(buf));
    	while ((chunked_length = parse_chunked_headers(buf)) > 0) {
			size += chunked_length;
    		Rio_readnb_w(&rio_server, buf, chunked_length);
			printf("chunk fd: %d\n", fd);
			Rio_writen_w(fd, buf, chunked_length);
			if (Rio_readlineb_w(&rio_server, buf, MAXLINE) <= 0) {
				printf("error after first one in the while loop\n");
				close(serverfd);
				return;
			}
    		Rio_writen_w(fd, buf, strlen(buf));
			if (Rio_readlineb_w(&rio_server, buf, MAXLINE) <= 0) {
				printf("error after second one in the while loop\n");
				close(serverfd);
				return;
			}
			Rio_writen_w(fd, buf, strlen(buf));
    	}
			if (Rio_readlineb_w(&rio_server, buf, MAXLINE) <= 0) {
				printf("error after third one in the while loop\n");
				close(serverfd);
				return;
			}
			Rio_writen_w(fd, buf, strlen(buf));
    } 
	else if (content_length > 0) {
		/* Define length with Content-length */
		printf("Content-length case\n");
        int left_length = content_length;
        int handle_length = 0;
    	while (left_length > 0) {
        handle_length = left_length > MAXBUF ? MAXBUF : left_length;
        left_length -= handle_length;
		size += handle_length;
		Rio_readnb_w(&rio_server, buf, handle_length);
        Rio_writen_w(fd, buf, handle_length);
      }
    } 
	else { /* Define length with closing connection */
		while ((chunked_length = Rio_readlineb_w(&rio_server, buf, MAXBUF)) > 0) {
			size += chunked_length;
			  Rio_writen_w(fd, buf, chunked_length);
		}
    }

	printf("Request %d: Forwarded %d bytes from end server to client\n", reqnum, size);

    /* Write log file */
    P(&log_mutex);

    /* Open log file */
    pLog = fopen("proxy.log", "a");
	logstring = create_log_entry(sockaddr, uri, size);
    printf("log entry generated: %s\n", logstring);

    fprintf(pLog, "%s\n", logstring);
    fclose(pLog);
    V(&log_mutex);

    /* Close connection to server */
    close(serverfd);

	/* Free dynamic variables */
	free(logstring);

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
	// char body[MAXBUF], headers[MAXBUF], truncated_cause[2049];
	char body[MAXBUF], headers[MAXBUF], truncated_cause[129];

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

	if (Rio_readlineb_w(rp, buf, MAXLINE) <= 0) {
		printf("error while reading header\n");
		return;
	}
	strcpy(content, buf);
	
	// tack on the connection: closed as per HTTP/1.1
    strcat(content, "Connection: close\r\n");
    while (strcmp(buf, "\r\n")) {
		if (Rio_readlineb_w(rp, buf, MAXLINE) <= 0) {
			printf("error in the header's while loop\n");
			return;
		}
		/* Get 'Content-Length:' */
        if (strncasecmp(buf, "Content-Length:", 15) == 0)
            *length = atoi(buf + 15);
        /* Get 'Transfer-Encoding: chunked' */
        if (strncasecmp(buf, "Transfer-Encoding: chunked", 26) == 0)
        	*chunked = 1;
        /* Remove 'Connection' and 'Proxy-Connection' */
        if (strncasecmp(buf, "Proxy-Connection:", 17) == 0
							|| strncasecmp(buf, "Connection:", 11) == 0)
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
    host_start = uri + 7; // since "http://" is 7 characters
	// locate the first occurrence of :, /, \r, \n or \0
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
 * Parse chunked header - Get chunked length from header
		Requires:
			"chunked_header" points to a valid response header
		Effects:
			Reads in the header and adds the lengths the chunked header remakrs.
			Returns the total length.
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


/*
	Rio_writen_w
	The custom wrapper function for rio_writen
	Requires:
		The arguments are same as rio_writen
	Effects:
		Execute rio_writen using the given arguments and return the number of
		bytes successfully written.
*/
ssize_t Rio_writen_w(int fd, void *usrbuf, size_t n)
{
	if (fd < 0) {
		printf("Corrupt file descriptor: %d\n", fd);
		return (-1);
	}
		printf("Rio_writen_w fd: %d\n", fd);
    if (rio_writen(fd, usrbuf, n) != (long)n) {
        fprintf(stderr, "Rio_writen_w error: %s\n", strerror(errno));
				return (-1);
	}
	return n;
}

/*
	Custom rio_readnb wrapper function
	Requires:
		Arguments are valid for rio_readnb
	Effects:
		Execute rio_readnb and return number of bytes successfully read
*/
ssize_t Rio_readnb_w(rio_t *rp, void *usrbuf, size_t n)
{
    ssize_t rc;

    if ((rc = rio_readnb(rp, usrbuf, n)) < 0) {
        fprintf(stderr, "Rio_readnb_w error: %s\n", strerror(errno));
        return 0;
    }
    return rc;
}

/*
	Custom rio_readlineb wrapper function
	Requires:
		Arguments are valid for rio_readlineb
	Effects:
		Execute rio_readlineb and return number of bytes successfully read
*/
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
 * open_clientfd (thread safe version) - open connection to server
 *	 at <hostname, port>
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
 * create_log_entry
 *
 * Requires:
 *   The parameter "sockaddr" must point to a valid sockaddr_in structure.  The
 *   parameter "uri" must point to a properly NUL-terminated string.
 *
 * Effects:
 *   Returns a string containing a properly formatted log entry.  This log
 *   entry is based upon the socket address of the requesting client
 *   ("sockaddr"), the URI from the request ("uri"), and the size in bytes of
 *   the response from the server ("size").
 */
static char *
create_log_entry(const struct sockaddr_in *sockaddr, const char *uri, int size)
{

	/*
	 * Create a large enough array of characters to store a log entry.
	 * Although the length of the URI can exceed MAXLINE, the combined
	 * lengths of the other fields and separators cannot.
	 */
	const size_t log_maxlen = MAXLINE + strlen(uri);
	char *const log_str = Malloc(log_maxlen + 1);

	/* Get a formatted time string. */
	time_t now = time(NULL);
	int log_strlen = strftime(log_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z: ", localtime(&now));

	/*
	 * Convert the IP address in network byte order to dotted decimal
	 * form.
	 */
	Inet_ntop(AF_INET, &sockaddr->sin_addr, &log_str[log_strlen], INET_ADDRSTRLEN);
	log_strlen += strlen(&log_str[log_strlen]);

	/*
	 * Assert that the time and IP address fields occupy less than half of
	 * the space that is reserved for the non-URI fields.
	 */
	assert(log_strlen < MAXLINE / 2);

	/*
	 * Add the URI and response size onto the end of the log entry.
	 */
	snprintf(&log_str[log_strlen], log_maxlen - log_strlen, " %s %d", uri, size);

	return (log_str);
}

/*
	Requires:
		"newelem" is a legitimate string
 	Effects:
		Create a singly linked list with only the specified element as the entry
 */
struct List* list_create(void)
{
	struct List* dummy = Malloc(sizeof(struct List));
	dummy->str = NULL;
	dummy->next = NULL;
	dummy->char_count = 0;
	return dummy;
}

/*
	List insertion
	Requires:
		"lp" must point to a valid list.
		"newelem" must point to a valid dynamically allocated string.
	Effects:
		Insert a new element containing string to the end of the list.
*/
void list_insert(struct List* lp, char* newelem)
{
	struct List* cur;
	struct List* new_list;
	unsigned long cc;

	/* find the previous last element */
	cur = lp;
	while (cur->next != NULL) {
		cur = cur->next;
	}
	/* make a new element */
	cc = strlen(newelem);
	new_list = Malloc(sizeof(struct List));
	new_list->char_count = cc;
	new_list->next = NULL;
	new_list->str = newelem;
	cur->next = new_list;
}

/*
	List destruction
	Requires:
		"lp" points to a valid dynamically constructed struct List
	Effects:
		Frees the variables involved including the list itself.
*/

void list_destroy(struct List* lp)
{
	if (lp->next != NULL) {
		list_destroy(lp->next);
	}
	if (lp->str != NULL) {
		free(lp->str);
	}
	if (lp != NULL) {
		free(lp);
	}
}

/*
	Total string concatenation
	Requires:
		"lp" must point to a valid list of the type struct List
	Effects:
		Iterates over the list and concatenates the whole list into
		one big string. This is our proposed solution for handling
		exceptionally long request strings.
*/

char* list_totalstring(struct List* lp)
{
	unsigned long total_char_count;
	struct List* cur;
	char* totalstring;

	/* first figure out the size of the string */
	total_char_count = 0;

	// go through the list
	cur = lp;
	while (cur != NULL) {
		total_char_count += cur->char_count;
		cur = cur->next;
	}

	// now we have the total size (and terminating string)
	totalstring = Calloc(total_char_count + 1, sizeof(char));

	// concatenate into the total string
	cur = lp;
	while (cur != NULL) {
		if (cur->char_count > 0) { // ignore dummy
			strcat(totalstring, cur->str);
		}
		cur = cur->next;
	}
	return totalstring;
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
