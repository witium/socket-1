struct socket_server;
struct rbuffer {
	struct rbuffer * next;
	int offset;
	int size;
	char * buffer;
};

typedef void (*onmessage)(int id,struct rbuffer *rb);
typedef void (*onconnect)(int id);
typedef void (*onstart)(int id,int openid);
typedef void (*onclose)(int id);

struct socket_server * server_new(onmessage messagecb,onconnect connectcb,onstart startcb,onclose closecb);
void server_delete(struct socket_server *ss);
void server_loop(struct socket_server *ss);
int socket_listen(struct socket_server *ss,const char * host,short port);
int socket_connect(struct socket_server *ss,const char * host,short port);
int socket_close(struct socket_server *ss,int id);
int socket_send(struct socket_server *ss,int id,char *buffer,int size);
