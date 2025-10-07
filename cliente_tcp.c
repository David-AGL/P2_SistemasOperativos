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

static char sala_actual[128] = "";  // Estado de sala actual
static int en_sala = 0;             // Flag si estaen alguna sala
static int en_espera = 0;            // Flag si esta en cola de espera
static char sala_espera[128] = "";   // Sala para la que está esperando

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

    printf("Conectado como %s.\n", user);
    printf("Usa <show> para ver las salas activas \n");
    printf("Usa <-help> para conocer los comandos y su uso \n");
    
    char setup_cmd[MAX_LINE];
    snprintf(setup_cmd, sizeof setup_cmd, "%s\n", user);
    send(s, setup_cmd, strlen(setup_cmd), 0);

    char line[MAX_LINE];
    while(1){
        fd_set r; FD_ZERO(&r); FD_SET(0,&r); FD_SET(s,&r); int maxfd=s;
        if(select(maxfd+1,&r,NULL,NULL,NULL)<0){ if(errno==EINTR) continue; perror("select"); break; }

        if(FD_ISSET(0,&r)){
            if(!fgets(line,sizeof line,stdin)) break; size_t n=strlen(line);
            if(n==0) continue; if(line[n-1]!='\n'){ line[n]='\n'; line[n+1]='\0'; }
 
            if(!strncasecmp(line,"join ",5)){
                char sala[128]={0}; sscanf(line+5,"%127s",sala);
                if(strlen(sala)==0){
                    printf("Uso: join <nombre_sala>\n");
                    continue;
                }
                
                // Validar estado actual
                if(en_sala && strlen(sala_actual) > 0){
                    printf("Ya estás en la sala '%s'. Usa 'leave' primero.\n", sala_actual);
                    continue;
                }
                
                if(en_espera && strlen(sala_espera) > 0){
                    printf("Ya estás en cola de espera para la sala '%s'. Usa 'status' para ver tu posición.\n", sala_espera);
                    continue;
                }
                
                char out[MAX_LINE]; snprintf(out,sizeof out,"join %s\n", sala);
                send(s,out,strlen(out),0);
            } else if(!strncasecmp(line,"show all users",14)){
                send(s,"show all users\n",15,0);
            } else if(!strncasecmp(line,"show users",10)){
                send(s,"show users\n",11,0);
            } else if(!strncasecmp(line,"show all",8)){
                send(s,"show all\n",9,0);
            } else if(!strncasecmp(line,"show",4)){
                send(s,"show\n",5,0);
            } else if(!strncasecmp(line,"leave ",6)){
                char sala[128]={0}; sscanf(line+6,"%127s",sala);
                if(strlen(sala)==0){
                    printf("Uso: leave <nombre_sala>\n");
                    continue;
                }
                if(!en_sala){
                    printf("No estás en ninguna sala.\n");
                    continue;
                }
                char out[MAX_LINE]; snprintf(out,sizeof out,"leave %s\n", sala);
                send(s,out,strlen(out),0);
            } else if(!strncasecmp(line,"leave",5)){
                // Require explicit room: do not send bare 'leave'
                printf("Uso: leave <nombre_sala>\n");
                continue;
            } else if(!strncasecmp(line,"status",6) || !strncasecmp(line,"estado",6)){
                send(s,"status\n",7,0);
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
            b[n]='\0'; 
            // Procesar respuestas  para actualizar estado
            if(strstr(b, "Te has unido a la sala:")){
                char *sala_start = strstr(b, "Te has unido a la sala: ");
                if(sala_start){
                    sala_start += 24; // longitud de "Te has unido a la sala: "
                    char *sala_end = strchr(sala_start, '\n');
                    if(sala_end) *sala_end = '\0';
                    // Limpiar espacios al final
                    char *end = sala_start + strlen(sala_start) - 1;
                    while(end > sala_start && (*end == ' ' || *end == '\t' || *end == '\r')) end--;
                    *(end + 1) = '\0';
                    strncpy(sala_actual, sala_start, sizeof(sala_actual)-1);
                    sala_actual[sizeof(sala_actual)-1] = '\0';
                    en_sala = 1;
                    en_espera = 0;
                    sala_espera[0] = '\0';
                }
            } else if(strstr(b, "Has salido de la sala:")){
                sala_actual[0] = '\0';
                en_sala = 0;
            } else if(strstr(b, "Ya hay cupo en '") && strstr(b, "Te unimos a la sala")){
                char *start = strstr(b, "Ya hay cupo en '");
                if(start){
                    start += 16; // longitud de "Ya hay cupo en 
                    char *end = strstr(start, "'. Te unimos");
                    if(end){
                        size_t len = end - start;
                        if(len < sizeof(sala_actual)){
                            strncpy(sala_actual, start, len);
                            sala_actual[len] = '\0';
                            en_sala = 1;
                            en_espera = 0;
                            sala_espera[0] = '\0';
                        }
                    }
                }
            } else if(strstr(b, "Quedaste en espera")){
                // Detectar cuando se agrega a cola de espera
                char *sala_start = strstr(b, "Sala '");
                if(sala_start){
                    sala_start += 6; 
                    char *sala_end = strstr(sala_start, "' ocupada");
                    if(sala_end){
                        size_t len = sala_end - sala_start;
                        if(len < sizeof(sala_espera)){
                            strncpy(sala_espera, sala_start, len);
                            sala_espera[len] = '\0';
                            en_espera = 1;
                        }
                    }
                }
            }
            
            fputs(b,stdout);
        }
    }
    close(s); return 0;
}