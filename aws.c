/**
 * Implements an asynchronous web sever for file transfer using HTTP.
 *
 * Author: Andreea Hodea, 2012
 */
 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <libaio.h>
#include <sys/eventfd.h>
#include <fcntl.h>


#include "aws.h"
#include "util.h"
#include "debug.h"
#include "sock_util.h"
#include "w_epoll.h"
#include "http_parser.h"
#include "mytests.h"

#include <sys/wait.h>

#define HTTP_404		"HTTP/1.1 404\r\n"
#define HTTP_OK			"HTTP/1.1 200 OK\r\n"
#define HTTP_LEN		"Content-Length: "
#define HTTP_CLOSE		"Connection: close\r\n"
#define FLAG_404		-2
#define FLAG_200		-1
#define static_folder		"/" AWS_REL_STATIC_FOLDER
#define dynamic_folder		"/" AWS_REL_DYNAMIC_FOLDER

static io_context_t ctx;
static int epollfd;
static int listenfd;

static void set_socket_non_blocking(int sockfd);
static void test_req(struct connection *conn);
static void async_read(struct connection *conn);
static http_parser request_parser;
static char request_path[BUFSIZ];	/* storage for request_path */


static void remove_conn(struct connection *conn) {
	int rc;
		
	/* Close HTTP connection. */
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove HTTP connection */
	connection_remove(conn);
}

/*
 * Callback is invoked by HTTP request parser when parsing request path.
 * Request path is stored in global request_path variable.
 */

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



struct connection *connection_create(int sockfd) {
	struct connection *conn = malloc(sizeof(*conn));
	DIE(conn == NULL, "malloc");

	conn->sockfd = sockfd;
	conn->event_fd = eventfd(0, EFD_NONBLOCK);
	DIE(conn->event_fd < 0, "eventfd");
	memset(conn->recv_buffer, 0, BUFSIZ);
	memset(conn->send_buffer, 0, BUFSIZ);
	conn->send_len = 0;
	conn->recv_len = -2;

	return conn;
}

/*
 * Remove connection handler.
 */
void connection_remove(struct connection *conn) {
	close(conn->sockfd);
	close(conn->event_fd);
	conn->state = STATE_CONNECTION_CLOSED;
	free(conn);
}

void handle_new_connection(void) {
	static int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;
	int rc;

	/* accept new connection */
	sockfd = accept(listenfd, (SSA *) &addr, &addrlen);
	DIE(sockfd < 0, "accept");
	
	set_socket_non_blocking(sockfd);
	
	dlog(LOG_ERR, "Accepted connection from: %s:%d\n", 
		inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

	/* instantiate new connection handler */
	conn = connection_create(sockfd);

	/* add socket to epoll */
	rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
	DIE(rc < 0, "w_epoll_add_in");
}

enum connection_state receive_message(struct connection *conn) {
	ssize_t bytes_recv;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}

	bytes_recv = recv(conn->sockfd, conn->recv_buffer, BUFSIZ, 0);
	if (bytes_recv < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication from: %s\n", abuffer);
		goto remove_connection;
	}
	if (bytes_recv == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed from: %s\n", abuffer);
		goto remove_connection;
	}

	dlog(LOG_DEBUG, "Received message from: %s\n", abuffer);

	conn->recv_len = bytes_recv;
	conn->state = STATE_DATA_RECEIVED;

	return STATE_DATA_RECEIVED;

remove_connection:
	remove_conn(conn);

	return STATE_CONNECTION_CLOSED;
}
 
void handle_client_request(struct connection *conn) {
	int rc;
	enum connection_state ret_state;

	ret_state = receive_message(conn);
	if (ret_state == STATE_CONNECTION_CLOSED)
		return;

	conn->send_len = FLAG_200;
	ret_state = prepare_response(conn);
	if (ret_state == STATE_CONNECTION_CLOSED)
		return;

	/* add socket to epoll for out events */
	rc = w_epoll_update_ptr_inout(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_update_ptr_inout");

}


static int send_chunk(struct connection *conn, struct stat buf) {
	size_t rc, count;
	int fd;
	count = buf.st_size;

	fd = open(conn->path, O_RDONLY);
	if (fd < 0)
		return -1;

	if (conn->send_len == FLAG_200) {
		sprintf(conn->send_buffer, "%s%s%u\r\n\r\n", HTTP_OK, HTTP_LEN, 
			count);
		rc = send(conn->sockfd, conn->send_buffer, 
			strlen(conn->send_buffer), 0);
		DIE(rc == -1, "send");
		if (rc == 0) {
			conn->send_len = FLAG_200;
			close(fd);
			return 0;
		}
		conn->send_len = 0;
	}

	rc = sendfile(conn->sockfd, fd, &(conn->send_len), 
		count - conn->send_len);
	DIE(rc == -1, "sendfile");

	dlog(LOG_ERR, "sent %i bytes out of %u\n", conn->send_len, 
		(unsigned int)buf.st_size);
	if (conn->send_len < buf.st_size) {		
		close(fd);
		return 0;
	}
	
	close(fd);
	
	return 1;
}

/* Find file, if it doesn't exist prepare 404 response.
 */
enum connection_state prepare_response(struct connection *conn) {
	size_t bytes_parsed;
	int rc;
	struct stat buf;
	
	http_parser_init(&request_parser, HTTP_REQUEST);

	bytes_parsed = http_parser_execute(&request_parser, &settings_on_path, 
			conn->recv_buffer, conn->recv_len);

	if (bytes_parsed <= 0) {
		goto send_404;
	}
	
	printf("path is %s\n", request_path);
	strcpy(conn->path, request_path);
	
	rc = stat(conn->path, &buf);
	if (rc == -1)
		goto send_404;

	if (strstr(conn->path, static_folder) == 0) {
		rc = send_chunk(conn, buf);
		if (rc < 0) 
			goto send_404;
		if (rc) {
			remove_conn(conn);
			dlog(LOG_ERR, "sent file %s\n", conn->path);
			return STATE_CONNECTION_CLOSED;
		}
		else
			return STATE_DATA_SENT;
	}
	else if (strstr(conn->path, dynamic_folder) == 0) {
	}
	
send_404:
	dlog(LOG_ERR, "%s\n", HTTP_404);
	sprintf(conn->send_buffer, "%s", HTTP_404);
	conn->send_len = FLAG_404;
	return STATE_DATA_SENT;
}

/**
 * Send HTTP reply. Send simple message, don't care about request content.
 *
 * Socket is closed after HTTP reply.
 */
void send_reply(struct connection *conn) {
	long bytes_sent;
	char abuffer[64];
	int rc, sockfd = conn->sockfd;
	char buffer[BUFSIZ];
	struct stat buf;
	
	dlog(LOG_ERR, "send_reply\n");
	
	if (conn->send_len == FLAG_404)
		sprintf(buffer, "%s", conn->send_buffer);
	else {
		rc = stat(conn->path, &buf);
		if (rc == -1)
			sprintf(buffer, "%s", HTTP_404);
		else {
			if (send_chunk(conn, buf) == 1) {
				remove_conn(conn);
			}
			else {
				rc = w_epoll_update_ptr_out(epollfd, 
					conn->sockfd, conn);
				DIE(rc < 0, "w_epoll_update_ptr_out");
			}
			return;
		}
	}

	rc = get_peer_address(sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}

	bytes_sent = send(sockfd, buffer, strlen(buffer), 0);
	if (bytes_sent < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication to %s\n", abuffer);
		goto remove_connection;
	}
	if (bytes_sent == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed to %s\n", abuffer);
		goto remove_connection;
	}

	dlog(LOG_INFO, "Sent message to %s (bytes: %ld)\n", abuffer, bytes_sent);

remove_connection:
	remove_conn(conn);
}

static void set_socket_non_blocking(int sockfd) {
	int flags, rc;
	
	flags = fcntl(sockfd, F_GETFL, 0);
	if (flags < 0) 
		flags = O_NONBLOCK;
	else 
		flags |= O_NONBLOCK;
	rc = fcntl(sockfd, F_SETFL, flags);
	DIE(rc != 0, "fcntl");
}

static void async_read(struct connection *conn) {
	int rc;
	struct iocb local_iocb, *piocb;	
printf("offset is %i\n", conn->send_len);
	io_prep_pread(&local_iocb, conn->async_fd, conn->recv_buffer, BUFSIZ, 
		conn->send_len);
	piocb = &local_iocb;
	io_set_eventfd(&local_iocb, conn->event_fd);
	
	rc = io_submit(ctx, 1, &piocb);
	
}

static void test_req(struct connection *conn) {
	int rc;	

	if (conn->recv_len == -2) {
printf("handle req on socket...\n");
		receive_message(conn);
		sprintf(conn->path, "/dynamic/small00.dat");
		conn->recv_len = 0;
		conn->send_len = 0;
		conn->async_fd = open(conn->path, O_RDONLY);
		DIE(conn->async_fd < 0, "open");
		async_read(conn);
		rc = w_epoll_add_ptr_in(epollfd, conn->event_fd, conn);
		DIE(rc < 0, "w_epoll_add_ptr_in");

printf("added event_fd \n");
		return;
	}
	if (strlen(conn->recv_buffer) == 0)
		return;
printf("read async %i bytes from %s\n", strlen(conn->recv_buffer), conn->path);
/*		struct io_event events[1];
	rc = io_getevents(ctx, 1, 1, events, NULL);
	DIE(rc < 0, "io_getevents");
*/
	strcpy(conn->send_buffer,conn->recv_buffer);
	conn->state = STATE_DATA_RECEIVED;
	
	if (conn->send_len <= 0) {
		rc = w_epoll_update_ptr_inout(epollfd, conn->sockfd, conn);
		DIE(rc < 0, "w_epoll_add_ptr_out");
	}
	else {
		rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
		DIE(rc < 0, "w_epoll_update_ptr_out");
	}

}

void test_send(struct connection *conn) {
	struct stat buf;
	int rc, len;
	
	rc = stat("/dynamic/small00.dat", &buf);
	DIE(rc < 0, "fstat");

	if (conn->send_len <= 0) {
printf("sending ok for async read\n");
		char buffer[BUFSIZ];
		sprintf(buffer, "%s%s%u\r\n\r\n", HTTP_OK, HTTP_LEN, buf.st_size);
		rc = send(conn->sockfd, conn->send_buffer, 
			strlen(conn->send_buffer), 0);
		DIE(rc == -1, "send");
	}
	
	len = strlen(conn->send_buffer);
	if ((rc = send(conn->sockfd, conn->send_buffer, len, 0)) < len) 
	{
		conn->send_len += rc;
		conn->state = STATE_DATA_SENT;
		strcpy(conn->send_buffer, conn->send_buffer + rc);
		rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
		DIE(rc < 0, "w_epoll_update_ptr_out");
		printf("still have to send...\n");
		return;
	}
	conn->send_len += rc;
	conn->state = STATE_DATA_SENT;
	
	printf("sent %u/%u\n", conn->send_len, buf.st_size);
	if (conn->send_len < buf.st_size) {
		async_read(conn);
		rc = w_epoll_update_ptr_in(epollfd, conn->event_fd, conn);
		DIE(rc < 0, "w_epoll_update_ptr_in");
	}
	else {
		close(conn->async_fd);
		remove_conn(conn);
	}
	
	
}

int main(void) {
	int rc;

	/* set root directory */
	rc = chroot(AWS_DOCUMENT_ROOT);
	DIE(rc != 0, "chroot");
	setenv("PWD", AWS_DOCUMENT_ROOT, 1);

	/* init multiplexing */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");
	
	/* create aio context*/
	rc = io_setup(NR_EVENTS, &ctx);
	DIE(rc < 0, "io_setup");
	
	/* create server socket */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT,
		 DEFAULT_LISTEN_BACKLOG);	
	DIE(listenfd < 0, "tcp_create_listener");
	set_socket_non_blocking(listenfd);

	/* add server socket to epoll set to listen for new connections */
	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	dlog(LOG_INFO, "Server waiting for connections on port %d\n",
		 AWS_LISTEN_PORT);

	struct epoll_event rev;

	while(1) {
		/* wait for events */
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		/* switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */
		if (rev.data.fd == listenfd) {
			dlog(LOG_DEBUG, "New connection\n");
			if (rev.events & EPOLLIN)
				handle_new_connection();
		}
		else {
			if (rev.events & EPOLLIN) {
				dlog(LOG_DEBUG, "New message\n");
				//handle_client_request(rev.data.ptr);
				test_req(rev.data.ptr);
			}
			if (rev.events & EPOLLOUT) {
				dlog(LOG_DEBUG, "Ready to send message\n");
				//send_reply(rev.data.ptr);
				test_send(rev.data.ptr);
			}
		}
	}

	/* destroy aio context */
	rc = io_destroy(ctx);
	DIE(rc < 0, "io_destroy");
 
	return 0;
}
