#define _POSIX_C_SOURCE 200809L 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "lz77_compress.h"

#define BUF_SIZE 1024
#define BACKLOG 10

static void q2_handle_client(int conn_fd, const char *client_ip, int client_port){
    for(;;){
        size_t msg_len = 0;
        uint8_t *msg = q2_recv_message(conn_fd, &msg_len);
        if(!msg){
            break;
        }
        printf("(compression mode) Received message from %s:%d - %zu bytes (after decompression)\n",client_ip,client_port,msg_len);
        q2_send_stats_t stats;
        if(q2_send_message(conn_fd, msg, msg_len, &stats) < 0){
            fprintf(stderr,"Failed to echo Q2 message back to %s:%d\n",client_ip,client_port);
            free(msg);
            break;
        }
        
        printf("(compression mode) Echoed back to %s:%d - original %zu bytes, compressed %zu bytes (%s), %zu bytes sent on wire\n",client_ip,client_port,stats.original_len,stats.compressed_len,stats.compression_used ? "used" : "not used",stats.bytes_sent);
        free(msg);
    }

}


static int q2_server_main(int argc,char *argv[]){
    if(argc < 2){
        fprintf(stderr,"Usage: %s <port> --compress\n",argv[0]);
        return EXIT_FAILURE;
    }


    int port = atoi(argv[1]);
    if(port <= 0 || port > 65535){
        fprintf(stderr,"Invalid port number: %s\n",argv[1]);
        return EXIT_FAILURE;
    }

    int listen_fd = socket(AF_INET,SOCK_STREAM,0);
    if(listen_fd < 0){
        perror("socket() failed");
        return EXIT_FAILURE;
    }

    int optval = 1;
    if(setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,&optval,sizeof(optval)) < 0){
        perror("setsockopt() failed");
        close(listen_fd);
        return EXIT_FAILURE;
    }


    struct sockaddr_in server_addr;
    memset(&server_addr,0,sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons((uint16_t)port);
    if(bind(listen_fd,(struct sockaddr *)&server_addr,sizeof(server_addr)) < 0){
        perror("bind() failed");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    if(listen(listen_fd, BACKLOG) < 0){
        perror("listen() failed");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    printf("Echo server (compression mode) listening on port %d...\n", port);
    for(;;){
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
 
        int conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr,&client_len);
        if(conn_fd < 0){
            perror("accept() failed");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET,&client_addr.sin_addr,client_ip, sizeof(client_ip));
        int client_port = ntohs(client_addr.sin_port);
        printf("Accepted connection from %s:%d\n",client_ip,client_port);
        q2_handle_client(conn_fd,client_ip,client_port);
        close(conn_fd);
        printf("Client %s:%d closed connection\n",client_ip,client_port);
    }

    close(listen_fd);
    return 0;
}

static void q1_style_handle_client_plain(int conn_fd,const char *client_ip,int client_port) {
    char buffer[BUF_SIZE];
    ssize_t n;
    while((n = recv(conn_fd,buffer,sizeof(buffer) -1, 0)) > 0){
        buffer[n] = '\0';
        printf("(%s:%d) Received %zd bytes: %s\n",client_ip,client_port,n,buffer);
        ssize_t total_sent = 0;
        while(total_sent < n){
            ssize_t sent = send(conn_fd, buffer + total_sent, (size_t)(n - total_sent), 0);
            if(sent < 0){
                perror("send() failed");
                break;
            }
            total_sent += sent;
        }
    }
    if(n < 0){
        perror("recv() failed");
    }
}



typedef struct {
    int conn_fd;
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
    int compress_mode;
} q2f2_client_args_t;

static void *q2f2_client_thread(void *arg){
    q2f2_client_args_t *a = (q2f2_client_args_t *)arg;
    printf("(thread %lu) Handling client %s:%d (%s mode)\n", (unsigned long)pthread_self(),a->client_ip,a->client_port,a->compress_mode ? "compression" : "plain");
    if(a->compress_mode){
        q2_handle_client(a->conn_fd,a->client_ip,a->client_port); // same as feature 1
    }else{
        q1_style_handle_client_plain(a->conn_fd,a->client_ip,a->client_port);
    }
    printf("(thread %lu) Finished client %s:%d\n", (unsigned long)pthread_self(),a->client_ip,a->client_port);
    close(a->conn_fd);
    free(a);
    return NULL;
}


static int q2f2_multi_server_main(int argc, char *argv[]){
    if(argc < 2){
        fprintf(stderr,"Usage: %s <port> --multi [--compress]\n",argv[0]);
        return EXIT_FAILURE;
    }

    int port = atoi(argv[1]);
    if(port <= 0 || port > 65535){
        fprintf(stderr,"Invalid port number: %s\n",argv[1]);
        return EXIT_FAILURE;
    }

    int compress_mode = 0;
    for(int i = 1; i < argc; i++){
        if(strcmp(argv[i], "--compress") == 0){
            compress_mode = 1;
        }
    }

    signal(SIGPIPE,SIG_IGN);
    int listen_fd = socket(AF_INET,SOCK_STREAM,0);
    if(listen_fd < 0){
        perror("socket() failed");
        return EXIT_FAILURE;
    }

    int optval = 1;
    if(setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR, &optval,sizeof(optval)) < 0){
        perror("setsockopt() failed");
        close(listen_fd);
        return EXIT_FAILURE;
    }


    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons((uint16_t)port);
    if(bind(listen_fd, (struct sockaddr *)&server_addr,sizeof(server_addr)) < 0){
        perror("bind() failed");
        close(listen_fd);
        return EXIT_FAILURE;
    }


    if(listen(listen_fd,BACKLOG) < 0){
        perror("listen() failed");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    printf("Echo server (concurrent mode%s) listening on port %d...\n",compress_mode ? " + compression" : "",port);
    for(;;){
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int conn_fd = accept(listen_fd,(struct sockaddr *)&client_addr,&client_len);
        if(conn_fd < 0){
            perror("accept() failed");
            continue;
        }


        q2f2_client_args_t *args = (q2f2_client_args_t *)malloc(sizeof(q2f2_client_args_t));
        if(!args){
            fprintf(stderr,"malloc() failed for client args - dropping connection\n");
            close(conn_fd);
            continue;
        }


        args->conn_fd = conn_fd;
        inet_ntop(AF_INET,&client_addr.sin_addr,args->client_ip,sizeof(args->client_ip));
        args->client_port = ntohs(client_addr.sin_port);
        args->compress_mode = compress_mode;
        printf("Accepted connection from %s:%d - dispatching to a new thread\n", args->client_ip,args->client_port);
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
        int rc = pthread_create(&tid,&attr,q2f2_client_thread, args);
        pthread_attr_destroy(&attr);
        if(rc != 0){
            fprintf(stderr,"pthread_create() failed: %s - dropping connection\n",strerror(rc));
            close(conn_fd);
            free(args);
            continue;
        }

    }
    close(listen_fd);
    return 0;
}


int main(int argc, char *argv[]){
    int multi_mode = 0;
    for(int i = 1;i < argc; i++){
        if(strcmp(argv[i], "--multi") == 0){
            multi_mode = 1;
        }
    }
    if(multi_mode){
        return q2f2_multi_server_main(argc, argv);
    }


    for(int i = 1;i < argc;i++){
        if(strcmp(argv[i],"--compress") == 0){
            return q2_server_main(argc, argv);
        }
    }

    if(argc != 2){
        fprintf(stderr,"Usage: %s <port>\n",argv[0]);
        exit(EXIT_FAILURE);
    }


    int port = atoi(argv[1]);
    if(port <= 0 || port > 65535){
        fprintf(stderr,"Invalid port number: %s\n", argv[1]);
        exit(EXIT_FAILURE);
    }

    int listen_fd = socket(AF_INET,SOCK_STREAM,0);
    if(listen_fd < 0){
        perror("socket() failed");
        exit(EXIT_FAILURE);
    }

    int optval = 1;
    if(setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0){
        perror("setsockopt() failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr,0,sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons((uint16_t)port);
    if(bind(listen_fd,(struct sockaddr *)&server_addr,sizeof(server_addr)) < 0){
        perror("bind() failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    if(listen(listen_fd, BACKLOG) < 0){
        perror("listen() failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    printf("Echo server listening on port %d...\n",port);
    for(;;){
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int conn_fd = accept(listen_fd,(struct sockaddr *)&client_addr,&client_len);
        if(conn_fd < 0){
            perror("accept() failed");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET,&client_addr.sin_addr,client_ip,sizeof(client_ip));
        printf("Accepted connection from %s:%d\n",client_ip,ntohs(client_addr.sin_port));
        char buffer[BUF_SIZE];
        ssize_t n;
        while((n = recv(conn_fd, buffer, sizeof(buffer) - 1, 0)) > 0){
            buffer[n] = '\0';
            printf("Received %zd bytes: %s\n",n,buffer);
            ssize_t total_sent = 0;
            while(total_sent < n){
                ssize_t sent = send(conn_fd,buffer+total_sent,(size_t)(n - total_sent),0);
                if(sent < 0){
                    perror("send() failed");
                    break;
                }
                total_sent += sent;
            }
        }


        if(n < 0){
            perror("recv() failed");
        }else{
            printf("Client %s:%d closed connection\n",client_ip,ntohs(client_addr.sin_port));
        }
        close(conn_fd);
    }


    close(listen_fd);
    return 0;
}

