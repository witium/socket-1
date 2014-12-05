#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <event.h>

#include "socket-server.h"

static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
    if (pthread_create(thread,NULL, start_routine, arg)) {
        fprintf(stderr, "Create thread failed");
        exit(1);
    }
}

struct socket_server * ss;
static void *
_server(void *ctx) {
    ss = (struct socket_server *)ctx;
    server_loop(ss);
}

static void *
_client(void *ctx) {
    ss = (struct socket_server *)ctx;
    server_loop(ss);
}

void
_on_open(int selfid,int id) {
    printf("id:%d accept:%d\n",selfid,id);
}

void 
_on_close(int id) {
    printf("id:%d close\n",id);
}

void
_on_message(int id,struct rbuffer *rb) {
    for(;;) {
        printf("message:id:%d %s\n",id,(char*)rb->buffer);
        if (rb->next != NULL) 
            rb = rb->next;
        else
            break;
    }
}

void
_on_connect(int id) {
    printf("%d connect!\n",id);
    char *str = "12333333333333333333333333333333333333333333333333333333333333333333333333333ddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    char * buffer = malloc(strlen(str));
    memcpy(buffer,str,strlen(str));
    socket_send(ss,id,buffer,strlen(str));
}

int main(int argc, char* argv[])
{
    pthread_t pid;
    struct socket_server * sss = server_new(_on_message,_on_connect,_on_open,_on_close);
    create_thread(&pid, _server, sss);
    socket_listen(sss,"127.0.0.1",1989);
    // struct socket_server * ssc = server_new(_on_message,_on_connect,_on_open,_on_close);
    // create_thread(&pid, _client, ssc);
    // socket_connect(ssc,"127.0.0.1",1989);
    sleep(10000);
    return 0;
} 
