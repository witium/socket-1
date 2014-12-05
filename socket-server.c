
#include <event.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>

#include "socket-server.h"

#define MAX_SOCKET_SHIFT 16
#define MAX_SOCKET (1 << MAX_SOCKET_SHIFT)

#define SOCKET_INVALID 		0
#define SOCKET_RESERVE 		1
#define SOCKET_LISTEN 		2
#define SOCKET_ACCEPTED     3
#define SOCKET_CONNECTED 	4
#define SOCKET_CONNECTING   5
#define SOCKET_CLOSING 		6

#define CTRL_LISTEN  'l'
#define CTRL_CONNECT 'C'
#define CTRL_SEND 	 'W'
#define CTRL_CLOSE 	 'X'


typedef void (*callback)(int, short, void *);


union sockaddr_all {
	struct sockaddr s;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
};

struct wbuffer {
	struct wbuffer * next;
	void * buffer;
	int offset;
	int size;
};

struct socket {
	int fd;
	int id;
	int type;
	int write;
	struct event wevent;
	struct event revent;
	struct wbuffer * head;
	struct wbuffer * tail;
	struct socket_server * ss;
};

struct socket_server {
	struct event_base * base;

	struct event ctrl_event;
	int recvctrl_fd;
	int sendctrl_fd;

	struct socket pool[MAX_SOCKET];
	int alloc_id;

	struct event * time_event;
	struct timeval tv;
	int recvsize;
	int sendsize;

	onmessage messagecb;
	onconnect connectcb;
	onclose closecb;
	onstart startcb;
};

struct pack_listen {
	int id;
	int fd;
};

struct pack_connect {
	int id;
	int port;
	char host[1];
};

struct pack_send {
	int id;
	char * buffer;
	int size;
};

struct pack_close {
	int id;
};

struct pack {
	uint8_t header[8];
	union {
		char buffer[64];
		struct pack_send send;
		struct pack_listen listen;
		struct pack_connect connect;
		struct pack_close close;
	}u;
};


void ev_ctrl(int, short, void *);
void ev_read(int, short, void *);
void ev_send(int, short, void *);
void ev_accept(int,short,void *);
void ev_connect(int,short,void *);
void ev_timeout(int,short,void *);
void force_close(struct socket *);

static void
socket_keepalive(int fd) {
	int keepalive = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive , sizeof(keepalive));  
}

static void
socket_nonblocking(int fd) {
	int flag = fcntl(fd, F_GETFL, 0);
	if ( -1 == flag ) {
		return;
	}
	fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

int 
ev_add(struct socket * s,int ev,callback cb,void *ctx) {
	struct event *event = NULL;
	if (ev & EV_WRITE) {
		s->write = 1;
		event = &s->wevent;
	}
	if (ev & EV_READ) {
		event = &s->revent;
	}
	event_set(event,s->fd,ev,cb,ctx);
	event_base_set(s->ss->base,event);
	return event_add(event, NULL);
}

int 
ev_del(struct socket * s,int ev) {
	struct event *event = NULL;
	if (ev & EV_WRITE) {
		s->write = 0;
		event = &s->wevent;
	}
	if (ev & EV_READ) {
		event = &s->revent;
	}
	return event_del(event);
} 

static int
reverve_id(struct socket_server *ss) {
	int i;
	for (i=0;i<MAX_SOCKET;i++) {
		int id = __sync_add_and_fetch(&(ss->alloc_id), 1);
		if (id < 0)
			id = __sync_and_and_fetch(&(ss->alloc_id), 0x7fffffff);
		struct socket *s = &ss->pool[id%MAX_SOCKET];
		if (s->type == SOCKET_INVALID) {
			if (__sync_bool_compare_and_swap(&s->type, SOCKET_INVALID, SOCKET_RESERVE))
				return id;
			else
				--i;
		}
	}
	assert(0);
}

static struct socket *
socket_init(struct socket_server *ss,int id,int fd) {
	struct socket * s = &ss->pool[id % MAX_SOCKET];
	assert(s->type == SOCKET_RESERVE);
	s->fd = fd;
	s->id = id;
	s->head = s->tail = NULL;
	return s;
}

struct socket_server *
server_new(onmessage messagecb,onconnect connectcb,onstart startcb,onclose closecb) {
	struct socket_server *ss = malloc(sizeof(*ss));
	ss->base = event_base_new();
	ss->messagecb = messagecb;
	ss->connectcb = connectcb;
	ss->closecb = closecb;
	ss->startcb = startcb;

	memset(ss->pool,0,sizeof(struct socket) * MAX_SOCKET);
	int i;
	for(i=0;i < MAX_SOCKET;i++) {
		ss->pool[i].ss = ss;
	}

	int fd[2];
	if (pipe(fd)) {
		fprintf(stderr,"socket server:create socket pair failed.\n");
		return NULL;
	}
	ss->recvctrl_fd = fd[0];
	ss->sendctrl_fd = fd[1];
	event_set(&ss->ctrl_event,ss->recvctrl_fd,EV_READ | EV_PERSIST,ev_ctrl,ss);
	event_base_set(ss->base,&ss->ctrl_event);
	event_add(&ss->ctrl_event, NULL);

	ss->tv.tv_sec = 1;
	ss->time_event = evtimer_new(ss->base,ev_timeout,ss);
	evtimer_add(ss->time_event,&ss->tv);

	return ss;
}

void
server_delete(struct socket_server *ss) {

}

void
server_loop(struct socket_server *ss) {
	event_base_dispatch(ss->base);
}

int
ctrl_listen_socket(struct socket_server *ss,struct pack_listen *pack) {
	int id = pack->id;
	int fd = pack->fd;

	struct socket * s = socket_init(ss,id,fd);
	s->type = SOCKET_LISTEN;
	ev_add(s,EV_READ | EV_PERSIST,ev_accept,s);
	return 0;
}

int
ctrl_connect_socket(struct socket_server * ss,struct pack_connect *pack) {
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	struct addrinfo *ai_ptr = NULL;
	char port[16];
	sprintf(port,"%d",pack->port);
	memset(&ai_hints,0,sizeof(ai_hints));
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_STREAM;
	ai_hints.ai_protocol = IPPROTO_TCP;

	int status = getaddrinfo(pack->host,port,&ai_hints,&ai_list);
	if (status != 0)
		goto failed;

	int fd = -1;
	for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next ) {
		fd = socket( ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol );
		if (fd < 0)
			continue;

		socket_keepalive(fd);
		socket_nonblocking(fd);

		status = connect(fd,ai_ptr->ai_addr,ai_ptr->ai_addrlen);
		if (status != 0 && errno != EINPROGRESS) {
			close(fd);
			fd = -1;
			continue;
		}
		break;
	}

	if (fd < 0)
		goto failed;

	struct socket *ns = socket_init(ss,pack->id,fd);
	assert(ns->type != SOCKET_INVALID);
	assert(ns->type != SOCKET_CLOSING);

	if (ns == NULL) {
		close(fd);
		goto failed;
	}

	if (status == 0) {
		ns->type = SOCKET_CONNECTED;
		ss->connectcb(pack->id);
		ev_add(ns,EV_READ | EV_PERSIST,ev_read,ns);
	} else {
		ns->type = SOCKET_CONNECTING;
		ev_add(ns,EV_WRITE,ev_connect,ns);
	}

	freeaddrinfo(ai_list);
	return 0;
failed:
	freeaddrinfo(ai_list);
	ss->pool[pack->id % MAX_SOCKET].type = SOCKET_INVALID;
	return -1;
}

int
append_wbuffer(struct socket_server * ss,struct socket * s,char * buffer,int size,int offset) {
	struct wbuffer *wb = malloc(sizeof(*wb));
	wb->buffer = buffer;
	wb->offset = offset;
	wb->size = size;
	wb->next = NULL;
	if (s->head == NULL) {
		s->head = s->tail = wb;
	} else {
		s->tail->next = wb;
		s->tail = wb;
	}
	if (s->write == 0) {
		ev_add(s,EV_WRITE,ev_send,s);
	}
	
	return 0;
}

int
ctrl_send_socket(struct socket_server * ss,struct pack_send *pack) {
	struct socket * s = &ss->pool[pack->id % MAX_SOCKET];
	assert(s->type != SOCKET_INVALID);
	assert(s->type != SOCKET_CLOSING);
	assert(s->type == SOCKET_CONNECTED || s->type == SOCKET_ACCEPTED);

	char * buffer = pack->buffer;
	int offset = 0;
	int size = pack->size;
	for(;;) {
		int n = write(s->fd,buffer + offset,size - offset);
		if (n < 0) {
			switch(errno) {
			case EINTR:
				continue;
			case EAGAIN:
				append_wbuffer(ss,s,buffer,size,offset);
				return 0;
			}
			break;
		}
		if (n == 0) {
			break;
		}
		s->ss->sendsize += n;
		offset += n;
		if (offset == size) {
			free(buffer);
			return 0;
		}
	}
	force_close(s);
	return -1;
}

void
force_close(struct socket * s) {
	ev_del(s,EV_READ);
	if (s->write == 1)
			ev_del(s,EV_WRITE);
	close(s->fd);
	struct wbuffer * current = s->head;
	while(current != NULL) {
		free(current->buffer);
		current = current->next;
	}
	s->type = SOCKET_INVALID;
	s->ss->closecb(s->id);
}

int
ctrl_close_socket(struct socket_server * ss,struct pack_close *pack) {
	struct socket * s = &ss->pool[pack->id % MAX_SOCKET];
	assert(s->type != SOCKET_INVALID);
	assert(s->type != SOCKET_CLOSING);
	struct wbuffer * current = s->head;
	while (current != NULL) {
		for(;;) {
			int n = write(s->fd,current->buffer + current->offset,current->size - current->offset);
			if (n < 0) {
				switch(errno) {
				case EINTR:
					continue;
				case EAGAIN:
					if (s->write == 0)
			            ev_add(s,EV_WRITE,ev_send,s);
					return 0;
				}
				goto fail;
			}
			if (n == 0)
				goto fail;

			current->offset += n;
			if (current->offset == current->size)
				break;
		}
		s->head = s->head->next;
		free(current->buffer);
		free(current);
		current = s->head;
	}
fail:
	force_close(s);
	return 0;
}

struct rbuffer *
rbuffer_new(int size) {
	struct rbuffer * rb = malloc(sizeof(*rb));
	rb->next = NULL;
	rb->size = size;
	rb->offset = 0;
	rb->buffer = malloc(rb->size);
	return rb;
}

void 
rbuffer_free(struct rbuffer *rb) {
	struct rbuffer * current = rb;
	struct rbuffer * next = current;
	while(next != NULL) {
		current = next;
		struct rbuffer * next = next->next;
		free(current->buffer);
		free(current);
	}
}

#define MINSIZE 64

void 
ev_read(int fd,short ev,void * ctx) {
	struct socket * s = ctx;

	struct rbuffer * head = rbuffer_new(MINSIZE);
	struct rbuffer * current = head;

	for(;;) {
		int n = (int)read(fd, current->buffer + current->offset, current->size - current->offset);
		if (n < 0) {
			switch(errno) {
			case EINTR:
				continue;
			case EAGAIN:
				current->buffer[current->offset] = '\0';
				s->ss->messagecb(s->id,head);
				return;
			}
			break;
		}
		if (n == 0)
			break;
		s->ss->recvsize += n;
		current->offset += n;
		if (current->offset == current->size) {
			current->buffer[current->offset] = '\0';
			struct rbuffer * nrb = rbuffer_new(current->size << 1);
			current->next = nrb;
			current = nrb;
		}
	}

	force_close(s);
	return;
}

void
ev_send(int fd,short ev,void *ctx) {
	struct socket * s = ctx;
	s->write = 0;
	struct wbuffer * current = s->head;
	while(current != NULL) {
		for(;;) {
			int n = (int)write(s->fd, current->buffer + current->offset, current->size - current->offset);
			if (n < 0) {
				switch(errno) {
				case EINTR:
					continue;
				case EAGAIN:
					ev_add(s,EV_WRITE,ev_send,s);
					return;
				}
				goto fail;
			}
			if (n == 0)
				goto fail;

			s->ss->sendsize += n;
			current->offset += n;
			if (current->offset == current->size) {
				break;
			}
		}
		
		s->head = s->head->next;
		free(current->buffer);
		free(current);
		current = s->head;
	}

	if (s->type == SOCKET_CLOSING)
		goto fail;
	return;
fail:
	force_close(s);
	return;
}

void
ev_accept(int fd,short ev,void * ctx) {
	struct socket *s = ctx;

	union sockaddr_all u;
	socklen_t len = sizeof(u);
	int clifd = accept(fd, &u.s, &len);
	if (clifd < 0)
		return;
	
	int id = reverve_id(s->ss);
	if (id < 0) {
		close(fd);
		return;
	}

	int keepalive = 1;
	setsockopt(clifd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive , sizeof(keepalive));  
	
	int flag = fcntl(clifd, F_GETFL, 0);
	if ( -1 == flag ) {
		close(fd);
		return;
	}

	fcntl(clifd, F_SETFL, flag | O_NONBLOCK);

	struct socket *ns = socket_init(s->ss,id,clifd);
	if (ns == NULL) {
		close(fd);
		return;
	}
	ns->type = SOCKET_ACCEPTED;

	ev_add(ns,EV_READ | EV_PERSIST,ev_read,ns);
	s->ss->startcb(s->id,ns->id);
}

void
ev_connect(int fd,short ev,void * ctx) {
	struct socket *ns = ctx;
	assert(ns->type == SOCKET_CONNECTING);

	int error;
	socklen_t len = sizeof(error);
	int code = getsockopt(ns->fd,SOL_SOCKET,SO_ERROR,&error,&len);
	if (code < 0 || error) {
		close(ns->fd);
		ev_del(ns,EV_WRITE);
		ns->type = SOCKET_INVALID;
	} else {
		ns->type = SOCKET_CONNECTED;
		ev_del(ns,EV_WRITE);
		ev_add(ns,EV_READ | EV_PERSIST,ev_read,ns);
		ns->ss->connectcb(ns->id);
	}
}

void
ev_timeout(int fd,short ev,void * ctx) {
	struct socket_server * ss = ctx;
	float rkb = (float)ss->recvsize / 1024;
	float skb = (float)ss->sendsize / 1024;
	printf("read %.1f kb last second.\n",rkb);
	printf("send %.1f kb last second.\n",skb);
	ss->recvsize = 0;
	ss->sendsize = 0;
	evtimer_add(ss->time_event,&ss->tv);
}

#define PIPEBUFFER 64

void 
ev_ctrl(int fd, short ev, void * args) {
	struct socket_server *ss = args;

	uint8_t header[2] = {0};
	for(;;) {
		int n = read(fd,header,2);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr,"socket server:read pipe head error %s.\n",strerror(errno));
			return;
		}
		if (n == 2)
			break;
	}

	int type = header[0];
	int len = header[1];
	uint8_t buffer[64] = {0};
	for(;;) {
		int n = read(fd,buffer,len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr,"socket server:read pipe body error %s.\n",strerror(errno));
			return;
		}
		if (n == len)
			break;
	}

	switch(type) {
	case CTRL_LISTEN:
		ctrl_listen_socket(ss,(struct pack_listen*)buffer);
		break;
	case CTRL_CONNECT:
		ctrl_connect_socket(ss,(struct pack_connect*)buffer);
		break;
	case CTRL_SEND:
		ctrl_send_socket(ss,(struct pack_send*)buffer);
		break;
	case CTRL_CLOSE:
		ctrl_close_socket(ss,(struct pack_close*)buffer);
		break;
	default:
		return;
	}
}

void
send_package(struct socket_server *ss,struct pack *p,char type,int len) {
	p->header[6] = (uint8_t)type;
	p->header[7] = (uint8_t)len;
	for (;;) {
		int n = write(ss->sendctrl_fd, &p->header[6], len+2);
		if (n<0) {
			if (errno != EINTR) {
				fprintf(stderr, "socket-server : send ctrl command error %s.\n", strerror(errno));
			}
			continue;
		}
		assert(n == len+2);
		return;
	}
}

int
socket_listen(struct socket_server *ss,const char * host,short port) {
	uint32_t addr = INADDR_ANY;
	if (host[0])
		addr = inet_addr(host);
	
	int fd = socket(AF_INET,SOCK_STREAM,0);
	if (fd < 0)
		return -1;

	int reuse = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(int))==-1) {
		close(fd);
		return -1;
	}
	
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(struct sockaddr_in));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = addr;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(struct sockaddr)) == -1) {
		close(fd);
		return -1;
	}

	if (listen(fd, 64) == -1) {
		close(fd);
		return -1;
	}
	
	struct pack pack;
	memset(&pack,0,sizeof(struct pack));
	int id = reverve_id(ss);
	pack.u.listen.id = id;
	pack.u.listen.fd = fd;
	send_package(ss,&pack,CTRL_LISTEN,sizeof(pack.u.listen));
	return id;
}

int 
socket_connect(struct socket_server *ss,const char *host,short port) {
	int len = strlen(host);

	struct pack pack;
	memset(&pack,0,sizeof(struct pack));

	int id = reverve_id(ss);
	pack.u.connect.id = id;
	pack.u.connect.port = port;
	memcpy(pack.u.connect.host,host,len);
	pack.u.connect.host[len] = '\0';
	send_package(ss,&pack,CTRL_CONNECT,sizeof(pack.u.connect) + len);
	return id;
}

int 
socket_send(struct socket_server *ss,int id,char *buffer,int size) {
	struct pack pack;
	memset(&pack,0,sizeof(struct pack));
	pack.u.send.id = id;
	pack.u.send.buffer = buffer;
	pack.u.send.size = size;
	send_package(ss,&pack,CTRL_SEND,sizeof(pack.u.send));
	return 0;
}

int 
socket_close(struct socket_server *ss,int id) {
	struct pack pack;
	memset(&pack,0,sizeof(struct pack));
	pack.u.close.id = id;
	send_package(ss,&pack,CTRL_CLOSE,sizeof(pack.u.close));
	return 0;
}
