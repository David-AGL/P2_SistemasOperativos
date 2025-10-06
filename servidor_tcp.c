// ./servidor_tcp 5555
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/file.h>

#define MAX_SALAS 10
#define MAX_USUARIOS_POR_SALA 20
#define MAX_TEXTO 256
#define MAX_NOMBRE 50
#define MAX_LINE 1024
#define MAX_CLIENTS FD_SETSIZE
#define CAPACIDAD_SALA_POR_DEFECTO 3
#define HISTORY_N 10

#define ROOMS_FILE "salas.txt"
#define LOG_DIR    "logs"

/* ========== util logs/ndjson (igual que tu versión) ========== */
static void ensure_log_dir(void){
    struct stat st; if (stat(LOG_DIR,&st)==-1) { if (mkdir(LOG_DIR,0700)==-1) perror("mkdir logs"); }
}
static void sanitize(const char* in, char* out, size_t sz){
    size_t j=0; for(size_t i=0; in[i] && j+1<sz; ++i){ unsigned char c=in[i];
        if (c=='/'||c=='\\'||c=='\n'||c=='\r'||c=='\t') continue; out[j++]=c; } out[j]='\0';
}
static void room_log_path(const char* sala, char* path, size_t sz){
    char r[MAX_NOMBRE]; sanitize(sala,r,sizeof r); snprintf(path,sz, LOG_DIR "/sala_%s.jsonl", r);
}
static void json_escape(const char* in, char* out, size_t outsz){
    size_t j=0; for(size_t i=0; in && in[i] && j+6<outsz; ++i){ unsigned char c=in[i];
        if(c=='\\'||c=='"'){ out[j++]='\\'; out[j++]=c; }
        else if(c=='\n'){ out[j++]='\\'; out[j++]='n'; }
        else if(c=='\r'){ out[j++]='\\'; out[j++]='r'; }
        else if(c=='\t'){ out[j++]='\\'; out[j++]='t'; }
        else if(c<0x20){ j+=snprintf(out+j,outsz-j,"\\u%04x",c); }
        else out[j++]=c;
    } out[j]='\0';
}
static void json_unescape(const char* in, char* out, size_t outsz){
    size_t j=0; for(size_t i=0; in&&in[i]&&j+1<outsz; ++i){
        if(in[i]=='\\'){ char c=in[++i];
            if(c=='n') out[j++]='\n'; else if(c=='r') out[j++]='\r';
            else if(c=='t') out[j++]='\t'; else if(c=='"') out[j++]='"';
            else if(c=='\\') out[j++]='\\'; else out[j++]=c;
        } else out[j++]=in[i];
    } out[j]='\0';
}
static void extract_from_text(const char* json, char* from, size_t fsz, char* text, size_t tsz){
    const char *pf=strstr(json,"\"from\":\""), *pt=strstr(json,"\"text\":\"");
    if(!pf||!pt){ from[0]=text[0]='\0'; return; }
    pf+=8; pt+=8; char bf[512]={0}, bt[1024]={0}; size_t i=0;
    while(pf[i] && pf[i]!='"' && i+1<sizeof(bf)){ if(pf[i]=='\\'&&pf[i+1]) bf[i++]=pf[i],bf[i]=pf[i]; else bf[i]=pf[i]; ++i; }
    bf[i]='\0'; i=0;
    while(pt[i] && pt[i]!='"' && i+1<sizeof(bt)){ if(pt[i]=='\\'&&pt[i+1]) bt[i++]=pt[i],bt[i]=pt[i]; else bt[i]=pt[i]; ++i; }
    bt[i]='\0'; json_unescape(bf,from,fsz); json_unescape(bt,text,tsz);
}
static void loguear_mensaje(const char* sala, const char* remitente, const char* texto){
    if(!sala||!*sala) return;
    char path[256]; room_log_path(sala,path,sizeof path);
    FILE* f=fopen(path,"a"); if(!f){ ensure_log_dir(); f=fopen(path,"a"); if(!f){ perror("fopen log"); return; } }
    int fd=fileno(f); flock(fd,LOCK_EX);
    time_t ts=time(NULL); char er[256], ef[256], et[1024];
    json_escape(sala,er,sizeof er); json_escape(remitente,ef,sizeof ef); json_escape(texto,et,sizeof et);
    fprintf(f, "{\"ts\":%ld,\"sala\":\"%s\",\"from\":\"%s\",\"text\":\"%s\"}\n", (long)ts,er,ef,et);
    fflush(f); flock(fd,LOCK_UN); fclose(f);
}
static void rooms_file_add_if_new(const char* sala){
    if(!sala||!*sala) return;
    FILE* f=fopen(ROOMS_FILE,"a+"); if(!f) return;
    fflush(f); fseek(f,0,SEEK_SET); char line[256];
    while(fgets(line,sizeof line,f)){ line[strcspn(line,"\r\n")]='\0'; if(strcmp(line,sala)==0){ fclose(f); return; } }
    fprintf(f,"%s\n",sala); fclose(f);
}
static void send_history_lastN(int cfd, const char* sala, int N){
    if(N<=0) return;
    char path[256]; room_log_path(sala,path,sizeof path);
    FILE* f=fopen(path,"r"); if(!f) return;
    char **ring=calloc(N,sizeof(char*)); if(!ring){ fclose(f); return; }
    size_t i=0,cnt=0; char *line=NULL; size_t cap=0;
    while(getline(&line,&cap,f)!=-1){ free(ring[i]); ring[i]=strdup(line); i=(i+1)%N; if(cnt<(size_t)N) cnt++; }
    free(line); fclose(f);
    size_t start=(cnt==(size_t)N)? i:0;
    for(size_t k=0;k<cnt;k++){
        const char* jsonl=ring[(start+k)%N]; if(!jsonl) continue;
        char from[MAX_NOMBRE]={0}, text[MAX_TEXTO]={0};
        extract_from_text(jsonl,from,sizeof from,text,sizeof text);
        dprintf(cfd,"FROM %s %s %s\n", sala, from, text);
    }
    for(size_t k=0;k<cnt;k++) free(ring[k]); free(ring);
}

/* ========== estructuras de salas (idéntica semántica) ========== */
struct usuario { int fd; char nombre[MAX_NOMBRE]; };
struct sala {
    char nombre[MAX_NOMBRE];
    int  num_usuarios;
    struct usuario usuarios[MAX_USUARIOS_POR_SALA];
    int  capacidad; // nuevo campo en tu sysv mejorado
    // cola de espera FIFO
    pid_t espera_fds[MAX_USUARIOS_POR_SALA*2];
    char   espera_nombres[MAX_USUARIOS_POR_SALA*2][MAX_NOMBRE];
    int espera_ini, espera_fin, espera_len;
};
static struct sala salas[MAX_SALAS];
static int num_salas = 0;

static int crear_sala(const char* nombre){
    if(num_salas>=MAX_SALAS) return -1;
    strcpy(salas[num_salas].nombre,nombre);
    salas[num_salas].num_usuarios=0;
    salas[num_salas].capacidad = CAPACIDAD_SALA_POR_DEFECTO;
    salas[num_salas].espera_ini=salas[num_salas].espera_fin=salas[num_salas].espera_len=0;
    num_salas++; return num_salas-1;
}
static int buscar_sala(const char* nombre){
    for(int i=0;i<num_salas;i++) if(strcmp(salas[i].nombre,nombre)==0) return i;
    return -1;
}
static int sala_count_fd(const char* sala){
    int idx=buscar_sala(sala); if(idx<0) return 0;
    return salas[idx].num_usuarios;
}
static int sala_add_usuario_cap(int idx, const char* nombre, int fd){
    if(idx<0||idx>=num_salas) return -1;
    struct sala *s=&salas[idx];
    // ya dentro por fd o nombre
    for(int i=0;i<s->num_usuarios;i++) if(s->usuarios[i].fd==fd) return 0;
    if(s->num_usuarios >= s->capacidad) return -2; // llena
    if(s->num_usuarios >= MAX_USUARIOS_POR_SALA) return -1; // límite duro
    s->usuarios[s->num_usuarios].fd = fd;
    strncpy(s->usuarios[s->num_usuarios].nombre, nombre, MAX_NOMBRE);
    s->num_usuarios++;
    return 1;
}
static int sala_remove_usuario(int idx, int fd){
    if(idx<0||idx>=num_salas) return -1;
    struct sala *s=&salas[idx];
    for(int i=0;i<s->num_usuarios;i++){
        if(s->usuarios[i].fd==fd){
            for(int j=i;j<s->num_usuarios-1;j++) s->usuarios[j]=s->usuarios[j+1];
            s->num_usuarios--; return 0;
        }
    }
    return -1;
}
static void sala_enqueue_espera(int idx, int fd, const char* nombre){
    struct sala *s=&salas[idx];
    // evitar duplicado
    for(int k=0;k<s->espera_len;k++){
        int pos=(s->espera_ini+k)%(MAX_USUARIOS_POR_SALA*2);
        if(s->espera_fds[pos]==fd) return;
    }
    if(s->espera_len >= (MAX_USUARIOS_POR_SALA*2)) return;
    s->espera_fds[s->espera_fin]=fd;
    strncpy(s->espera_nombres[s->espera_fin], nombre, MAX_NOMBRE);
    s->espera_fin = (s->espera_fin+1)%(MAX_USUARIOS_POR_SALA*2);
    s->espera_len++;
}
static int sala_dequeue_espera(int idx, int* out_fd, char* out_nombre){
    struct sala *s=&salas[idx];
    if(s->espera_len==0) return 0;
    *out_fd = s->espera_fds[s->espera_ini];
    strncpy(out_nombre, s->espera_nombres[s->espera_ini], MAX_NOMBRE);
    s->espera_ini = (s->espera_ini+1)%(MAX_USUARIOS_POR_SALA*2);
    s->espera_len--;
    return 1;
}
static int sala_pos_espera(int idx, int fd){
    struct sala *s=&salas[idx]; int pos=0;
    for(int k=0;k<s->espera_len;k++){
        int p=(s->espera_ini+k)%(MAX_USUARIOS_POR_SALA*2);
        pos++;
        if(s->espera_fds[p]==fd) return pos;
    }
    return -1;
}

/* ========== clientes conectados ========== */
typedef struct { int fd; int joined; char nombre[MAX_NOMBRE]; int sala_idx; } Client;
static Client clients[MAX_CLIENTS];

static int make_server(uint16_t port){
    int s=socket(AF_INET,SOCK_STREAM,0); if(s<0){perror("socket"); exit(1);}
    int yes=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons(port);
    if(bind(s,(struct sockaddr*)&a,sizeof a)<0){perror("bind"); exit(1);}
    if(listen(s,64)<0){perror("listen"); exit(1);} return s;
}
static void sendf(int fd, const char* fmt, ...){
    char b[2048]; va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof b,fmt,ap); va_end(ap);
    send(fd,b,strlen(b),0);
}
static void enviar_a_sala_menos_remitente(int idx_sala, int fd_rem, const char* remitente, const char* texto){
    if(idx_sala<0||idx_sala>=num_salas) return;
    struct sala *s=&salas[idx_sala];
    for(int i=0;i<s->num_usuarios;i++){
        int fd = s->usuarios[i].fd;
        if (fd==fd_rem) continue;
        sendf(fd, "%s: %s\n", remitente[0]?remitente:"server", texto);
        // para mantener también el formato broadcast tipo FROM, opcional:
        // sendf(fd, "FROM %s %s %s\n", s->nombre, remitente, texto);
    }
}
static int room_count_idx(int idx){
    if(idx<0||idx>=num_salas) return 0; return salas[idx].num_usuarios;
}

static void try_promote_from_wait(int idx_sala){
    struct sala *s=&salas[idx_sala];
    if (s->num_usuarios >= s->capacidad) return;
    int wfd=-1; char wname[MAX_NOMBRE]={0};
    if (!sala_dequeue_espera(idx_sala, &wfd, wname)) return;
    if (sala_add_usuario_cap(idx_sala, wname, wfd) == 1) {
        sendf(wfd, "Ya hay cupo, entraste a la sala: %s\n", s->nombre);
        send_history_lastN(wfd, s->nombre, HISTORY_N);
        enviar_a_sala_menos_remitente(idx_sala, wfd, "", "nuevo usuario entró");
    }
}

/* ========== main TCP con select() ========== */
int main(int argc, char** argv){
    uint16_t port = (argc>1)? (uint16_t)atoi(argv[1]) : 5555;
    int srv = make_server(port);
    ensure_log_dir();

    // init clients
    for(int i=0;i<MAX_CLIENTS;i++){ clients[i].fd=-1; clients[i].joined=0; clients[i].nombre[0]='\0'; clients[i].sala_idx=-1; }

    printf("Servidor de chat TCP iniciado en puerto %u\n", port);

    char buf[MAX_LINE];
    while(1){
        fd_set r; FD_ZERO(&r); FD_SET(srv,&r); int maxfd=srv;
        for(int i=0;i<MAX_CLIENTS;i++) if(clients[i].fd!=-1){ FD_SET(clients[i].fd,&r); if(clients[i].fd>maxfd) maxfd=clients[i].fd; }

        if(select(maxfd+1,&r,NULL,NULL,NULL)<0){ if(errno==EINTR) continue; perror("select"); break; }

        if(FD_ISSET(srv,&r)){ // nuevo cliente
            struct sockaddr_in cli; socklen_t cl=sizeof cli; int cfd=accept(srv,(struct sockaddr*)&cli,&cl);
            if(cfd>=0){
                int slot=-1; for(int i=0;i<MAX_CLIENTS;i++) if(clients[i].fd==-1){ slot=i; break; }
                if(slot==-1){ close(cfd); }
                else{
                    clients[slot].fd=cfd; clients[slot].joined=0; clients[slot].nombre[0]='\0'; clients[slot].sala_idx=-1;
                    sendf(cfd, "OK Bienvenido. Usa: join <sala> <usuario>\n");
                }
            }
        }

        for(int i=0;i<MAX_CLIENTS;i++){
            int fd=clients[i].fd; if(fd==-1) continue; if(!FD_ISSET(fd,&r)) continue;
            ssize_t n=recv(fd,buf,sizeof(buf)-1,0);
            if(n<=0){
                if(clients[i].joined){
                    int idx = clients[i].sala_idx;
                    enviar_a_sala_menos_remitente(idx, fd, "", "usuario salió");
                    sala_remove_usuario(idx, fd);
                    try_promote_from_wait(idx);
                }
                close(fd); clients[i].fd=-1; clients[i].joined=0; clients[i].sala_idx=-1; continue;
            }
            buf[n]='\0';

            // procesar por líneas
            char *s=buf, *e;
            while((e=strpbrk(s,"\r\n"))){
                *e='\0';
                if(*s){
                    char cmd[16]={0}; sscanf(s,"%15s",cmd);
                    for(char* p=cmd; *p; ++p) *p=tolower((unsigned char)*p);

                    if(strcmp(cmd,"join")==0){
                        char sala[MAX_NOMBRE]={0}, user[MAX_NOMBRE]={0};
                        if(sscanf(s+5,"%49s %49s", sala, user)!=2){
                            sendf(fd, "ERR uso: join <sala> <usuario>\n");
                        } else {
                            int idx = buscar_sala(sala);
                            if (idx==-1){
                                idx = crear_sala(sala);
                                if (idx==-1){ sendf(fd,"No se pudo crear la sala %s\n", sala); s=e+1; while(*s=='\r'||*s=='\n') s++; continue; }
                                rooms_file_add_if_new(sala);
                                printf("Nueva sala creada: %s\n", sala);
                            }
                            int r = sala_add_usuario_cap(idx, user, fd);
                            if (r==1){
                                clients[i].joined=1; clients[i].sala_idx=idx; strncpy(clients[i].nombre, user, MAX_NOMBRE);
                                sendf(fd, "Te has unido a la sala: %s\n", sala);
                                send_history_lastN(fd, sala, HISTORY_N);
                                // avisar a otros
                                enviar_a_sala_menos_remitente(idx, fd, "", "nuevo usuario entró");
                            } else if (r==-2){
                                // llena: a espera
                                sala_enqueue_espera(idx, fd, user);
                                int pos = sala_pos_espera(idx, fd);
                                sendf(fd, "Sala llena. Quedaste en espera (%d)\n", (pos>0)?pos:1);
                            } else if (r==0){
                                sendf(fd, "Te has unido a la sala: %s\n", sala); // ya estaba
                            } else {
                                sendf(fd, "Error al unirte a la sala %s\n", sala);
                            }
                        }
                    }
                    else if(strcmp(cmd,"leave")==0){
                        if(!clients[i].joined){ sendf(fd, "ERR no estás en sala\n"); }
                        else {
                            int idx=clients[i].sala_idx;
                            char sala[MAX_NOMBRE]; strcpy(sala, salas[idx].nombre);
                            enviar_a_sala_menos_remitente(idx, fd, "", "usuario salió");
                            sala_remove_usuario(idx, fd);
                            clients[i].joined=0; clients[i].sala_idx=-1; clients[i].nombre[0]='\0';
                            sendf(fd, "Has salido de la sala: %s\n", sala);
                            try_promote_from_wait(idx);
                        }
                    }
                    else if (strcasecmp(s,"show all users")==0){
                        FILE* rf=fopen(ROOMS_FILE,"r");
                        if(!rf){ sendf(fd,"Usuarios por sala:\n"); }
                        else{
                            char line[256]; sendf(fd,"Usuarios por sala:\n");
                            while(fgets(line,sizeof line,rf)){
                                line[strcspn(line,"\r\n")]='\0';
                                int idx=buscar_sala(line); int cnt = (idx>=0)? room_count_idx(idx):0;
                                sendf(fd,"[%s] (%d)\n", line, cnt);
                                if(idx>=0){
                                    struct sala *sala=&salas[idx];
                                    for(int u=0; u<sala->num_usuarios; u++)
                                        sendf(fd," - %s\n", sala->usuarios[u].nombre);
                                }
                            }
                            fclose(rf);
                        }
                    }
                    else if (strcasecmp(s,"show users")==0){
                        if(!clients[i].joined){ sendf(fd,"No estás en ninguna sala. Usa 'join <sala> <usuario>'.\n"); }
                        else{
                            int idx=clients[i].sala_idx; struct sala *sa=&salas[idx];
                            sendf(fd,"Sala: %s (%d usuarios)\n", sa->nombre, sa->num_usuarios);
                            for(int u=0; u<sa->num_usuarios; u++)
                                sendf(fd," - %s\n", sa->usuarios[u].nombre);
                        }
                    }
                    else if (strcasecmp(s,"show all")==0){
                        FILE* rf=fopen(ROOMS_FILE,"r");
                        if(!rf){ sendf(fd,"Salas registradas (todas):\n"); }
                        else{
                            char line[256]; sendf(fd,"Salas registradas (todas):\n");
                            while(fgets(line,sizeof line,rf)){
                                line[strcspn(line,"\r\n")]='\0';
                                int idx=buscar_sala(line); int cnt=(idx>=0)? room_count_idx(idx):0;
                                sendf(fd," - %s (%d activos)\n", line, cnt);
                            }
                            fclose(rf);
                        }
                    }
                    else if (strcasecmp(s,"show")==0){
                        // Salas activas (con usuarios)
                        int printed=0; sendf(fd,"Salas activas:\n");
                        for(int a=0;a<num_salas;a++){
                            if(salas[a].num_usuarios>0){
                                sendf(fd," - %s (%d usuarios)\n", salas[a].nombre, salas[a].num_usuarios);
                                printed=1;
                            }
                        }
                        if(!printed) sendf(fd,"(ninguna)\n");
                    }
                    else if(strcmp(cmd,"help")==0 || strcmp(cmd,"-help")==0){
                        sendf(fd,
                              "Comandos:\n"
                              " join <sala> <usuario>\n"
                              " show | show all | show users | show all users\n"
                              " leave\n"
                              " texto libre -> mensaje a tu sala\n");
                    }
                    else if(strcmp(cmd,"msg")==0){
                        if(!clients[i].joined){ sendf(fd,"ERR primero join\n"); }
                        else{
                            const char* text=s+4; if(!*text){ sendf(fd,"ERR texto vacío\n"); }
                            else{
                                struct sala *sa=&salas[clients[i].sala_idx];
                                loguear_mensaje(sa->nombre, clients[i].nombre, text);
                                enviar_a_sala_menos_remitente(clients[i].sala_idx, fd, clients[i].nombre, text);
                                // opcional: eco de OK como hacías para CMD_SEND
                                // sendf(fd,"OK enviado\n");
                            }
                        }
                    }
                    else{
                        // texto libre -> mensaje (igual que antes)
                        if(!clients[i].joined){ sendf(fd,"No estás en ninguna sala. Usa 'join <sala>' para unirte a una.\n"); }
                        else{
                            struct sala *sa=&salas[clients[i].sala_idx];
                            loguear_mensaje(sa->nombre, clients[i].nombre, s);
                            enviar_a_sala_menos_remitente(clients[i].sala_idx, fd, clients[i].nombre, s);
                        }
                    }
                }
                s = e+1; while(*s=='\r'||*s=='\n') s++;
            }
        }
    }
    close(srv); return 0;
}
