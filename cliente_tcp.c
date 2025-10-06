// ./cliente_tcp <host> <puerto> <usuario>
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>

#define MAX_LINE 1024

static int conectar(const char* host, const char* puerto){
    struct addrinfo hints={0}, *res,*rp; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
    int rc=getaddrinfo(host,puerto,&hints,&res); if(rc!=0){ fprintf(stderr,"getaddrinfo: %s\n",gai_strerror(rc)); exit(1); }
    int s=-1; for(rp=res; rp; rp=rp->ai_next){ s=socket(rp->ai_family,rp->ai_socktype,rp->ai_protocol);
        if(s==-1) continue; if(connect(s,rp->ai_addr,rp->ai_addrlen)==0) break; close(s); s=-1; }
    freeaddrinfo(res); if(s==-1){ perror("connect"); exit(1);} return s;
}

int main(int argc, char** argv){
    if(argc<4){ fprintf(stderr,"Uso: %s <host> <puerto> <usuario>\n", argv[0]); return 1; }
    const char* host=argv[1]; const char* puerto=argv[2]; const char* user=argv[3];
    int s=conectar(host,puerto);

    printf("Bienvenido, %s. Usa <show> para ver las salas activas \n", user);
    printf("Usa <-help> para conocer los comandos y su uso \n");

    char line[MAX_LINE];
    while(1){
        fd_set r; FD_ZERO(&r); FD_SET(0,&r); FD_SET(s,&r); int maxfd=s;
        if(select(maxfd+1,&r,NULL,NULL,NULL)<0){ if(errno==EINTR) continue; perror("select"); break; }

        if(FD_ISSET(0,&r)){
            if(!fgets(line,sizeof line,stdin)) break; size_t n=strlen(line);
            if(n==0) continue; if(line[n-1]!='\n'){ line[n]='\n'; line[n+1]='\0'; }

            if(!strncasecmp(line,"join ",5)){
                char sala[128]={0}; sscanf(line+5,"%127s",sala);
                char out[MAX_LINE]; snprintf(out,sizeof out,"JOIN %s %s\n", sala, user);
                send(s,out,strlen(out),0);
            } else if(!strncasecmp(line,"show all users",14)){
                send(s,"show all users\n",15,0);
            } else if(!strncasecmp(line,"show users",10)){
                send(s,"show users\n",11,0);
            } else if(!strncasecmp(line,"show all",8)){
                send(s,"show all\n",9,0);
            } else if(!strncasecmp(line,"show",4)){
                send(s,"show\n",5,0);
            } else if(!strncasecmp(line,"leave",5)){
                send(s,"leave\n",6,0);
            } else if(!strncasecmp(line,"help",4) || !strncasecmp(line,"-help",5) || !strncasecmp(line,"/?",2) || !strncasecmp(line,"ayuda",5)){
                send(s,"help\n",5,0);
            } else {
                char out[MAX_LINE]; snprintf(out,sizeof out,"MSG %s", line);
                send(s,out,strlen(out),0);
            }
        }

        if(FD_ISSET(s,&r)){
            char b[MAX_LINE+1]; ssize_t n=recv(s,b,MAX_LINE,0);
            if(n<=0){ printf("Desconectado del servidor\n"); break; }
            b[n]='\0'; fputs(b,stdout);
        }
    }
    close(s); return 0;
}
