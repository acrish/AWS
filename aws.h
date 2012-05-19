/*
 * Asynchronous Web Server - header file (macros and structures)
 *
 * 2011, Operating Systems; Andreea Hodea, 2012
 */

#ifndef AWS_H_
#define AWS_H_		1

#ifdef __cplusplus
extern "C" {
#endif

#define AWS_LISTEN_PORT			8888
#define AWS_DOCUMENT_ROOT		"./"
#define AWS_REL_STATIC_FOLDER		"static/"
#define AWS_REL_DYNAMIC_FOLDER		"dynamic/"
#define AWS_ABS_STATIC_FOLDER		AWS_DOCUMENT_ROOT AWS_REL_STATIC_FOLDER
#define AWS_ABS_DYNAMIC_FOLDER		AWS_DOCUMENT_ROOT AWS_REL_DYNAMIC_FOLDER
#define NR_EVENTS			10

#define HTTP_RESP			200
#define HTTP_ERR			404

#ifndef BUFSIZ
#define BUFSIZ				8192
#endif

#ifdef __cplusplus
}
#endif

enum 	connection_state {
	STATE_DATA_RECEIVED,
	STATE_DATA_SENT,
	STATE_CONNECTION_CLOSED
};

/* structure acting as a connection handler */
struct connection {
	int sockfd;
	int event_fd;
	/* buffers used for receiving messages and then replying them back */
	char recv_buffer[BUFSIZ];
	size_t recv_len;
	char send_buffer[BUFSIZ];
	int send_len;
	enum connection_state state;
};

struct 	connection *connection_create(int sockfd);
void 	connection_remove(struct connection *conn);
void 	handle_new_connection(void);
void 	handle_client_request(struct connection *conn);
enum 	connection_state receive_message(struct connection *conn);
void 	send_reply(struct connection *conn);
enum 	connection_state prepare_response(struct connection *conn);

#endif /* AWS_H_ */
