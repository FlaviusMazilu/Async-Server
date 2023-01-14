/*
 * epoll-based echo server. Uses epoll(7) to multiplex connections.
 *
 * TODO:
 *  - block data receiving when receive buffer is full (use circular buffers)
 *  - do not copy receive buffer into send buffer when send buffer data is
 *      still valid
 *
 * 2011-2017, Operating Systems
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <libaio.h>
#include <sys/eventfd.h>
#include <sys/sendfile.h>

#include "util.h"
#include "debug.h"
#include "sock_util.h"
#include "w_epoll.h"
#include "http_parser.h"
#include "aws.h"

#define ECHO_LISTEN_PORT AWS_LISTEN_PORT
static char request_path[BUFSIZ];	/* storage for request_path */
static http_parser request_parser;
static struct connection *connections[100];
static int active_cons = 0;


/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

enum fileType {
	STATIC,
	DYNAMIC,
	INVALID
};
enum connection_state {
	STATE_DATA_RECEIVED,
	STATE_DATA_SENT,
	STATE_CONNECTION_CLOSED,
	STATE_DATA_SENDING
};
#define MAX_LENGTH_PATH 100

static int on_path_cb(http_parser *p, const char *buf, size_t len)
{
	assert(p == &request_parser);
	memcpy(request_path, buf, len);

	return 0;
}

/* Use mostly null settings except for on_path callback. */
static http_parser_settings settings_on_path = {
	/* on_message_begin */ 0,
	/* on_header_field */ 0,
	/* on_header_value */ 0,
	/* on_path */ on_path_cb,
	/* on_url */ 0,
	/* on_fragment */ 0,
	/* on_query_string */ 0,
	/* on_body */ 0,
	/* on_headers_complete */ 0,
	/* on_message_complete */ 0
};

/* structure acting as a connection handler */
struct connection {
	int sockfd;
	/* buffers used for receiving messages and then echoing them back */
	char recv_buffer[BUFSIZ];
	size_t recv_len;
	char send_buffer[BUFSIZ];
	size_t send_len;
	enum connection_state state;
	enum fileType fileType;
	char path_requested[MAX_LENGTH_PATH];
	
	int eventfd;
	io_context_t *ctx;
	struct iocb *iocb;
	struct iocb **piocb;
	int file_fd;
	int aux_del;
	long long offset;
	long long size;
};

/*
 * Initialize connection structure on given socket.
 */

static struct connection *connection_create(int sockfd)
{
	struct connection *conn = malloc(sizeof(*conn));

	DIE(conn == NULL, "malloc");

	conn->sockfd = sockfd;
	memset(conn->recv_buffer, 0, BUFSIZ);
	memset(conn->send_buffer, 0, BUFSIZ);
	memset(conn->path_requested, 0, MAX_LENGTH_PATH);

	conn->ctx = NULL;
	connections[active_cons++] = conn;
	conn->aux_del = 0;
	conn->offset = 0;
	conn->ctx = malloc(sizeof(io_context_t));
    int rc = io_setup(10, conn->ctx);
	dlog(LOG_DEBUG, "--->%d %d\n", errno, rc);
	DIE (rc < 0, "io setup fail");
	
	conn->iocb = malloc(sizeof(struct iocb));
	conn->piocb = malloc(sizeof(struct iocb*));
	
	DIE (conn->iocb == NULL, "malloc");
	dlog(LOG_DEBUG, "HERE???\n");
	return conn;
}

/*
 * Copy receive buffer to send buffer (echo).
 */

// static void connection_copy_buffers(struct connection *conn)
// {
// 	conn->send_len = conn->recv_len;
// 	memcpy(conn->send_buffer, conn->recv_buffer, conn->send_len);
// }

/*
 * Remove connection handler.
 */

static void connection_remove(struct connection *conn)
{
	close(conn->sockfd);
	close(conn->file_fd);
	free(conn->iocb);
	free(conn->piocb);
	io_destroy(*conn->ctx);
	free(conn->ctx);
	conn->state = STATE_CONNECTION_CLOSED;
	for (int i = 0; i < active_cons; i++) {
		if (conn == connections[i]) {
			for (int j = i; j < active_cons - 1; j++) {
				free(conn);
				connections[j] = connections[j + 1];
			}
			connections[active_cons - 1] = 0;
			active_cons--;
			break;
		}
	}
}

/*
 * Handle a new connection request on the server socket.
 */

static void handle_new_connection(void)
{
	static int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;
	int rc;

	/* accept new connection */
	sockfd = accept(listenfd, (SSA *) &addr, &addrlen);
	DIE(sockfd < 0, "accept");

	dlog(LOG_ERR, "Accepted connection from: %s:%d\n",
		inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

	/* instantiate new connection handler */
	conn = connection_create(sockfd);

	/* add socket to epoll */
	rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
	DIE(rc < 0, "w_epoll_add_in");
}

/*
 * Receive message on socket.
 * Store message in recv_buffer in struct connection.
 */

void parse_path_request(struct connection *conn)
{
	http_parser_settings http_settings = settings_on_path;
	http_parser_init(&request_parser, HTTP_REQUEST);
	int rc = http_parser_execute(&request_parser, &http_settings, conn->recv_buffer, conn->recv_len);
	DIE(rc < 0, "http parser request path");

	memcpy(conn->path_requested, AWS_DOCUMENT_ROOT, strlen(AWS_DOCUMENT_ROOT));
	memcpy(conn->path_requested + strlen(AWS_DOCUMENT_ROOT), request_path, strlen(request_path) + 1);
	// fprintf(f_out, "path= %s\n", conn->path_requested);
	fflush(f_out);
}

static enum connection_state receive_message(struct connection *conn)
{
	ssize_t bytes_recv = 0;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}
	int aux = 0;
	// fprintf(f_out, "before recv\n");
	do {
		aux += bytes_recv;
		if (strstr(conn->recv_buffer, "\r\n\r\n") != NULL)
			break;
		bytes_recv = recv(conn->sockfd, conn->recv_buffer + aux, BUFSIZ - aux, 0);
		// fprintf(f_out, "->>>>>%s\n", conn->recv_buffer);
	} while (bytes_recv > 0);
	bytes_recv = aux;
	// fprintf(f_out, "<<<<%ld\n", bytes_recv);
	if (bytes_recv < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication from: %s\n", abuffer);
		// fprintf(f_out, "Error in communication from: %s\n", abuffer);
		goto remove_connection;
	}
	// if (bytes_recv == 0) {		/* connection closed */
		// dlog(LOG_INFO, "Connection closed from: %s\n", abuffer);
		// fprintf(f_out, "Connection closed from: %s\n", abuffer);
		// goto remove_connection;
	// }

	dlog(LOG_DEBUG, "Received message from: %s\n", abuffer);
	// fprintf(f_out, "Received message from: %s\n", abuffer);

	// fprintf(f_out, "--\n%s--\n", conn->recv_buffer);

	conn->recv_len = bytes_recv;

	// TODO handle http request-> extract path
	parse_path_request(conn);
	if (access(conn->path_requested, F_OK) == 0) {
    	// file exists
		// fprintf(f_out, "file exists\n");
		conn->state = STATE_DATA_RECEIVED;
		return STATE_DATA_RECEIVED;
	}
	// file doesn't exist
	char reply[BUFSIZ] = "HTTP/1.0 404 ERROR\r\n\r\n";
	int bytes_sent = 0;

	aux = 0;
	do {
		aux += bytes_sent;
		bytes_sent = send(conn->sockfd, reply + aux, strlen(reply) + 1, 0);
	}while (bytes_sent > 0);

	// fprintf(f_out, "file doesn't exist\n");

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

/*
 * Send message on socket.
 * Store message in send_buffer in struct connection.
 */
// http_parser http_reply;
// void parse_http_response(struct connection *conn)
// {
// 	// request_parser
// 	http_parser_init(&http_reply, HTTP_RESPONSE);
// 	http_parser_settings settings;
// 	http_parser_execute(&http_reply, &settings, NULL, NULL);
	
// }
// static enum connection_state send_sendfile(struct connection *conn)
// {

// }
static enum connection_state send_header(struct connection *conn)
{
	ssize_t bytes_sent = 0;
	int rc;
	char abuffer[64];
	char reply[BUFSIZ] = "HTTP/1.0 200 OK\r\n\r\n";
	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}

	int aux = 0;
	do {
		aux += bytes_sent;
		bytes_sent = send(conn->sockfd, reply + aux, strlen(reply) - aux, 0);
	} while (bytes_sent > 0);

	bytes_sent = aux;
	if (bytes_sent < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication to %s\n", abuffer);
		goto remove_connection;
	}
	if (bytes_sent == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed to %s\n", abuffer);
		goto remove_connection;
	}

	return STATE_DATA_SENT;
remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);
	return STATE_CONNECTION_CLOSED;
}


static enum connection_state send_message_sendfile(struct connection *conn)
{
	ssize_t bytes_sent;
	int rc;
	char abuffer[64];
	
	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}

	struct stat stat;
	int fd = open(conn->path_requested, O_RDONLY);
	DIE (fd < 0, "open");

	fstat(fd, &stat);
	fprintf(f_out, "conn->send_len = %ld\n", stat.st_size);
	fflush(f_out);
	int aux = 0;
	off_t offset = 0;
	bytes_sent = 0;
	do {
		aux += bytes_sent;
		bytes_sent = sendfile(conn->sockfd, fd, &offset, stat.st_size - offset);

	} while (bytes_sent > 0);
	
	DIE (rc < 0, "sendfile");
	bytes_sent = aux;

	if (bytes_sent < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication to %s\n", abuffer);
		goto remove_connection;
	}
	if (bytes_sent == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed to %s\n", abuffer);
		goto remove_connection;
	}

	dlog(LOG_DEBUG, "Sending message to %s\n", abuffer);

	// printf("--\n%s--\n", conn->send_buffer);

	/* all done - remove out notification */
	close(fd);
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_update_ptr_in");
	connection_remove(conn);

	return STATE_DATA_SENT;
remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);
	close(fd);
	return STATE_CONNECTION_CLOSED;
}

static enum connection_state send_message_event_sig(struct connection *conn)
{
	dlog(LOG_DEBUG, "send message event sig\n");
	// function called by eventfd, so it shouldn't block
	struct io_event revent;
	memset(&revent, 0, sizeof(struct io_event));

	io_getevents(*conn->ctx, 1, 1, &revent, NULL);

	w_epoll_remove_fd(epollfd, conn->eventfd);

	// clear the notification from epoll
	uint64_t eventfd_msg;
	int rc = read(conn->eventfd, &eventfd_msg, sizeof(uint64_t));
	close(conn->eventfd);

	ssize_t bytes_sent;
	char abuffer[64];
	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}
	// from here it has to be asyncron
	int aux = 0;
	bytes_sent = 0;
	conn->send_len = revent.res;
	do {
		aux += bytes_sent;
		bytes_sent = send(conn->sockfd, conn->send_buffer + aux, conn->send_len - aux, 0);
		
	} while (bytes_sent > 0);
	bytes_sent = aux;
	if (bytes_sent < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication to %s\n", abuffer);
		goto remove_connection;
	}
	if (bytes_sent == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed to %s\n", abuffer);
		goto remove_connection;
	}

	dlog(LOG_DEBUG, "Sending message to %s\n", abuffer);

	// printf("--\n%s--\n", conn->send_buffer);
	// dlog(LOG_DEBUG, "Sending messag%e of %lu BYTES\n", revent.res);
	conn->offset += revent.res;
	dlog(LOG_DEBUG, "BYTES = %ld OFFSET = %lld  SIZE = %lld\n", revent.res, conn->offset, conn->size);

	if (conn->offset < conn->size) {
	// 	// it means the buffer went full before reading the full file
	// 	// put it to read the rest async and notify when ready
		// dlog(LOG_DEBUG, "== BUFSIZ, get again get file in mem\n");
		conn->aux_del = 50;
		get_file_in_mem(conn);
		return STATE_DATA_SENDING;
	}
		
	/* all done - remove out notification */
	// rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	// DIE(rc < 0, "w_epoll_update_ptr_in");
	connection_remove(conn);

	return STATE_DATA_SENT;
remove_connection:
	/* remove current connection */
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

static enum connection_state send_message(struct connection *conn)
{
	if (send_header(conn) == STATE_CONNECTION_CLOSED)
		return STATE_CONNECTION_CLOSED;
	dlog(LOG_DEBUG, "Header sent\n");

	if (conn->fileType == STATIC) {
		return send_message_sendfile(conn);
	}
	if (conn->fileType == DYNAMIC) {
		get_file_in_mem(conn);
	}
	// get_file_in_mem(conn);
	// send_message_event_sig(conn);
	int rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	return STATE_DATA_SENDING;
}

/*
 * Handle a client request on a client connection.
 */


enum fileType get_fileType(char *buffer)
{
	if (strstr(buffer, "static") != NULL)
		return STATIC;
	if (strstr(buffer, "dynamic") != NULL)
		return DYNAMIC;
	return INVALID;
}

void get_file_in_mem(struct connection *conn)
{
	if (conn->aux_del == 50)
		dlog(LOG_DEBUG, "THIS IS IT\n");

	dlog(LOG_DEBUG, "get file in mem\n");
	
	conn->eventfd = eventfd(0,0);
	DIE(conn->eventfd < 0, "eventfd fail");
	set_nonblocking(conn->eventfd);
	dlog(LOG_DEBUG, "eventfd = %d\n", conn->eventfd);

	w_epoll_add_fd_in(epollfd, conn->eventfd);

	struct stat stat;
	fstat(conn->file_fd, &stat);
	conn->send_len = stat.st_size;


	io_prep_pread(conn->iocb, conn->file_fd, conn->send_buffer, BUFSIZ, conn->offset);
	io_set_eventfd(conn->iocb, conn->eventfd);

	conn->piocb[0] = conn->iocb;

	io_submit(*conn->ctx, 1, conn->piocb);
	dlog(LOG_DEBUG, "CLOSE get file in mem\n");	
}
static void handle_client_request(struct connection *conn)
{
	int rc;
	enum connection_state ret_state;
	ret_state = receive_message(conn);
	if (ret_state == STATE_CONNECTION_CLOSED)
		return;

	/* add socket to epoll for out events */
	conn->fileType = get_fileType(conn->path_requested);
	conn->file_fd = open(conn->path_requested, O_RDONLY);
	DIE(conn->file_fd < 0, "open file");
	set_nonblocking(conn->file_fd);
	struct stat stat;
	fstat(conn->file_fd, &stat);
	conn->size = stat.st_size;

	rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_add_ptr_inout");

}
int handle_eventfd(struct epoll_event rev)
{
	for (int i = 0; i < active_cons; i++) {
		if (rev.data.fd > 0 && connections[i]->eventfd == rev.data.fd) {
			dlog(LOG_DEBUG, "handle eventfd\n");
			send_message_event_sig(connections[i]);
			return 1;
		}
	}
	return 0;
}
int main(void)
{
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	/* create server socket */
	listenfd = tcp_create_listener(ECHO_LISTEN_PORT,
		DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	int	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	dlog(LOG_INFO, "Server waiting for connections on port %d\n",
		ECHO_LISTEN_PORT);
	/* server main loop */
	while (1) {
		struct epoll_event rev;
		memset(&rev, 0, sizeof(struct epoll_event));
		/* wait for events */
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		/*
		 * switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */

		if (rev.data.fd == listenfd) {
			dlog(LOG_DEBUG, "New connection\n");
			// fprintf(f_out, "new connection\n");
			// fflush(f_out);
			if (rev.events & EPOLLIN)
				handle_new_connection();
		} else {
			if (rev.events & EPOLLIN) {
				dlog(LOG_DEBUG, "New EPOLLIN\n");
				int ok = handle_eventfd(rev);
				if (ok == 1)
					continue;
				dlog(LOG_DEBUG, "New request\n");
				handle_client_request(rev.data.ptr);
			}
			if (rev.events & EPOLLOUT) {
				dlog(LOG_DEBUG, "Ready to send message\n");
				send_message(rev.data.ptr);
			}
		}
	}
	return 0;
}