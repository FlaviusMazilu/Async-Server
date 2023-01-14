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
#include "aux.h"
#define ECHO_LISTEN_PORT AWS_LISTEN_PORT
#define MAX_LENGTH_PATH 100

static void set_nonblocking(int fd);

static char request_path[BUFSIZ];	/* storage for request_path */
static http_parser request_parser;
static struct connection *connections[10];
static int active_cons = 0;

FILE *f_out;
void set_nonblocking(int fd);
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
};

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
	
	int eventfd_recv;
	int eventfd_send;

	io_context_t ctx;
	struct iocb *iocb;
	int file_fd;
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

	conn->eventfd_send = eventfd(0, 0);
	conn->eventfd_recv = eventfd(0, 0);

	set_nonblocking(conn->eventfd_recv);
	set_nonblocking(conn->eventfd_send);
	int rc;
	rc = w_epoll_add_fd_in(epollfd, conn->eventfd_send);

	DIE(conn->eventfd_recv == -1 || conn->eventfd_send == -1, "eventfd fail");

	rc = io_setup(2, &conn->ctx);
	DIE(rc < 0, "io setup");

	connections[active_cons++] = conn;

	conn->iocb = malloc(sizeof(struct iocb));
	DIE (conn->iocb == NULL, "malloc");

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
	conn->state = STATE_CONNECTION_CLOSED;

	close(conn->eventfd_recv);
	close(conn->eventfd_send);
	io_destroy(conn->ctx);
	
	close(conn->file_fd);
	free(conn->iocb);

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
	} while (bytes_recv > 0);
	bytes_recv = aux;
	if (bytes_recv < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication from: %s\n", abuffer);
		goto remove_connection;
	}

	dlog(LOG_DEBUG, "Received message from: %s\n", abuffer);

	conn->recv_len = bytes_recv;

	parse_path_request(conn);
	if (access(conn->path_requested, F_OK) == 0) {
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

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

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

void get_file_in_mem(struct connection *conn)
{
	uint64_t data;
	read(conn->eventfd_send, &data, sizeof(data));	

	struct stat stat;
	fstat(conn->file_fd, &stat);
	conn->send_len = stat.st_size;

	io_prep_pread(conn->iocb, conn->file_fd, conn->send_buffer, BUFSIZ, 0);
	io_set_eventfd(conn->iocb, conn->eventfd_recv);

	w_epoll_add_fd_in(epollfd, conn->eventfd_recv);
	io_submit(conn->ctx, 1, &conn->iocb);
	// struct io_event revent;
	// io_getevents(conn->ctx, 1, 1, &revent, NULL);
	//TODO CLOSE fd, event and all
	// int aux = 0;
	// int bytes_received = 0;
	// conn->send_len = 0;
	// do {
	// 	aux += bytes_received;
	// 	bytes_received = read(file_fd, conn->send_buffer + aux, BUFSIZ - aux);
	// } while (bytes_received > 0);
	// conn->send_len = aux;
	// if (conn->send_len == BUFSIZ)
	// 	return BUFFER_FULL;
	// return BUFFER_NOT_FULL;

}
// returns the number of bytes sent
static int send_message_recv_async(struct connection *conn)
{
	uint64_t eventmsg;
	read(conn->eventfd_recv, &eventmsg, sizeof(eventmsg));

	int rc;
	char abuffer[64];
	rc = get_peer_address(conn->sockfd, abuffer, 64);
	int bytes_sent = 0;
	int aux = 0;
	
	struct io_event revent;
	io_getevents(conn->ctx, 1, 1, &revent, NULL);
	conn->send_len = revent.res;
	fprintf(f_out, "<><><><>%ld\n", revent.res);
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
	/* all done - remove out notification */
	if (conn->send_len == BUFSIZ) {
		// it means that it was not received all
		uint64_t data = 1;
		write(conn->eventfd_send, &data, sizeof(data));
		return STATE_DATA_SENT;
	}
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_update_ptr_in");
	connection_remove(conn);
	return STATE_CONNECTION_CLOSED;

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
	if (send_header(conn) == STATE_CONNECTION_CLOSED)
		return STATE_CONNECTION_CLOSED;
	char abuffer[64];
	rc = get_peer_address(conn->sockfd, abuffer, 64);
	
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

static enum connection_state send_message(struct connection *conn)
{
	if (conn->fileType == STATIC) {
		return send_message_sendfile(conn);
	}

	if (send_header(conn) == STATE_CONNECTION_CLOSED)
		return STATE_CONNECTION_CLOSED;
	w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	// send_message_recv_async(conn, file_fd);
	return STATE_DATA_SENT;
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

static void handle_client_request(struct connection *conn)
{
	int rc;
	enum connection_state ret_state;
	
	ret_state = receive_message(conn);
	if (ret_state == STATE_CONNECTION_CLOSED)
		return;
	/* add socket to epoll for out events */
	// conn->send_buffer, move here
	enum fileType f = get_fileType(conn->path_requested);
	conn->fileType = f;
	
	int file_fd = open(conn->path_requested, O_RDONLY);
	DIE(file_fd < 0, "die open");
	conn->file_fd = file_fd;

	if (f == DYNAMIC) {
		get_file_in_mem(conn);
	}
	// connection_copy_buffers(conn);
	rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_add_ptr_inout");
}

int main(void)
{
	int rc;
	f_out = fopen("plswork.txt", "a");
	if (f_out != NULL)
		printf("aici NU e problema\n");
	DIE(f_out == NULL, "fopen");

	// char buffer[100];
	// getcwd(buffer, sizeof(buffer));
	// fprintf(f_out, "%s\n", buffer);
	// fflush(f_out);
	/* init multiplexing */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	/* create server socket */
	listenfd = tcp_create_listener(ECHO_LISTEN_PORT,
		DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	rc = w_epoll_add_fd_in(epollfd, listenfd);
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
				fprintf(f_out, "here1\n");
				// an event occured
				int ok = 0;
				for (int i = 0; i < active_cons; i++) {
					if (connections[i]->eventfd_recv == rev.data.fd && connections[i]->eventfd_recv != -1) {
						ok = 1;
						send_message_recv_async(connections[i]);
						break;
					}
					if (connections[i]->eventfd_recv == rev.data.fd && connections[i]->eventfd_recv != -1) {
						ok = 1;
						get_file_in_mem(connections[i]);
						break;
					}
				}
				if (ok == 0) {
					dlog(LOG_DEBUG, "New message\n");
					fprintf(f_out, "here2\n");
					handle_client_request(rev.data.ptr);
				}
			}
			if (rev.events & EPOLLOUT) {
				dlog(LOG_DEBUG, "Ready to send message\n");
				// fprintf(f_out, "ready to send message\n");
				// fflush(f_out);
				send_message(rev.data.ptr);
				// fprintf(f_out, "message sent\n");
			}
		}
	}
	fclose(f_out);
	return 0;
}
