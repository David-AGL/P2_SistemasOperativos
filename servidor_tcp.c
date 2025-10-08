// ./servidor_tcp 5555
/*
 TCP server: usa sockets (socket/bind/listen/accept) y select() para manejar conexiones.
 Cada cliente se identifica por su file descriptor (fd) y se comunica con send/recv.
En cuanto a  persistencia, los mensajes se guardan en archivos JSONL por sala en LOG_DIR.
 Esta implementación reemplaza la cola IPC: el protocolo se mueve por mensajes TCP.
*/

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

/* forward declaration for sendf so functions that send can call it */
static void sendf(int fd, const char* fmt, ...);

/* ========== util logs/ndjson ========== */

 // Asegura que el directorio de logs existe; si no, lo crea con permisos 0700.
static void ensure_log_dir(void){
    struct stat st; if (stat(LOG_DIR,&st)==-1) { if (mkdir(LOG_DIR,0700)==-1) perror("mkdir logs"); }
}

// Limpia y normaliza un nombre de sala/archivo.
// - Elimina barras, saltos de línea y tabuladores para evitar entradas inválidas.
// - Resulta en un nombre seguro para usar en rutas/archivos.
static void sanitize(const char* in, char* out, size_t sz){
    size_t j=0; for(size_t i=0; in[i] && j+1<sz; ++i){ unsigned char c=in[i];
        if (c=='/'||c=='\\'||c=='\n'||c=='\r'||c=='\t') continue; out[j++]=c; } out[j]='\0';
}

// Construye la ruta del archivo de log de la sala.
// - Usa sanitize() para evitar caracteres peligrosos y genera LOG_DIR/sala_<nombre>.jsonl
static void room_log_path(const char* sala, char* path, size_t sz){
    char r[MAX_NOMBRE]; sanitize(sala,r,sizeof r); snprintf(path,sz, LOG_DIR "/sala_%s.jsonl", r);
}

// Escapa una cadena para incluirla como valor JSON entre comillas.
// - Reemplaza ", \, newline, tab, etc. por sus secuencias escapadas.
// - Evita generar JSON inválido al escribir logs.
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

// Deshace las secuencias escapadas (\n, \t, \" , \\) a sus caracteres originales.
static void json_unescape(const char* in, char* out, size_t outsz){
    size_t j=0; for(size_t i=0; in&&in[i]&&j+1<outsz; ++i){
        if(in[i]=='\\'){ char c=in[++i];
            if(c=='n') out[j++]='\n'; else if(c=='r') out[j++]='\r';
            else if(c=='t') out[j++]='\t'; else if(c=='"') out[j++]='"';
            else if(c=='\\') out[j++]='\\'; else out[j++]=c;
        } else out[j++]=in[i];
    } out[j]='\0';
}

// Extrae los campos "from" y "text" de una línea JSONL simple.
// Busca las claves en la línea y aplica json_unescape.
// - Permite reconstruir remitente y texto para enviar historial por socket.
static void extract_from_text(const char* json, char* from, size_t fsz, char* text, size_t tsz){
    const char *pf=strstr(json,"\"from\":\""), *pt=strstr(json,"\"text\":\"");
    if(!pf||!pt){ from[0]=text[0]='\0'; return; }
    pf+=8; pt+=8; char bf[512]={0}, bt[1024]={0}; size_t i=0;
    while(pf[i] && pf[i]!='"' && i+1<sizeof(bf)){ if(pf[i]=='\\'&&pf[i+1]) bf[i++]=pf[i],bf[i]=pf[i]; else bf[i]=pf[i]; ++i; }
    bf[i]='\0'; i=0;
    while(pt[i] && pt[i]!='"' && i+1<sizeof(bt)){ if(pt[i]=='\\'&&pt[i+1]) bt[i++]=pt[i],bt[i]=pt[i]; else bt[i]=pt[i]; ++i; }
    bt[i]='\0'; json_unescape(bf,from,fsz); json_unescape(bt,text,tsz);
}

// Añade un mensaje al log de la sala en formato JSONL.
// - Abre el archivo en modo append, aplica flock(LOCK_EX) para escritura atómica,
//   escapa campos, escribe una línea JSON con timestamp y cierra el fichero.
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

// Agrega el nombre de la sala a ROOMS_FILE si no existe ya.
// Mantiene un listado persistente de salas conocidas, usado para el comando show all rooms.
static void rooms_file_add_if_new(const char* sala){
    if(!sala||!*sala) return;
    FILE* f=fopen(ROOMS_FILE,"a+"); if(!f) return;
    fflush(f); fseek(f,0,SEEK_SET); char line[256];
    while(fgets(line,sizeof line,f)){ line[strcspn(line,"\r\n")]='\0'; if(strcmp(line,sala)==0){ fclose(f); return; } }
    fprintf(f,"%s\n",sala); fclose(f);
}

// Envía las últimas N líneas del log de la sala al cliente (cfd).
// Lee el archivo JSONL en un anillo para devolver solo las N últimas entradas,
// además, extrae "from" y "text" de cada JSONL y los envía vía sendf al socket del cliente.
// por último, maneja archivos inexistentes, memoria y libera todo apropiadamente.
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
    sendf(cfd, "FROM %s %s %s\n", sala, from, text);
    }
    for(size_t k=0;k<cnt;k++) free(ring[k]); free(ring);
}

/* ========== Estructuras de salas (sigue la misma lógica que IPC) ========== */

// Definición de Struct para gestionar salas de chat
// Campos:
//   - nombre: Nombre de la sala
//   - num_usuarios: Número actual de usuarios en la sala
//   - usuarios: Array de usuarios presentes en la sala (fd + nombre)
//   - capacidad: Capacidad máxima de usuarios permitida en la sala
//   - espera_fds: Array de file descriptors de usuarios en espera (cola FIFO)
//   - espera_nombres: Array de nombres correspondientes a la cola de espera
//   - espera_ini, espera_fin, espera_len: índices y longitud para gestionar la cola 
struct usuario { int fd; char nombre[MAX_NOMBRE]; };
struct sala {
    char nombre[MAX_NOMBRE];
    int  num_usuarios;
    struct usuario usuarios[MAX_USUARIOS_POR_SALA];
    int  capacidad;
    // cola de espera FIFO (adaptada del IPC)
    int espera_fds[MAX_USUARIOS_POR_SALA*2];
    char espera_nombres[MAX_USUARIOS_POR_SALA*2][MAX_NOMBRE];
    int espera_ini, espera_fin, espera_len;
};

static struct sala salas[MAX_SALAS];
static int num_salas = 0;

/* Crea una nueva sala con nombre `nombre`.
   Retorno:
     >=0 : índice de la sala creada en el array `salas`
     -1  : no pudo crear (límite MAX_SALAS alcanzado) */
static int crear_sala(const char* nombre){
    if(num_salas>=MAX_SALAS) return -1;
    strcpy(salas[num_salas].nombre,nombre);
    salas[num_salas].num_usuarios=0;
    salas[num_salas].capacidad = CAPACIDAD_SALA_POR_DEFECTO;
    salas[num_salas].espera_ini=salas[num_salas].espera_fin=salas[num_salas].espera_len=0;
    num_salas++; return num_salas-1;
}

/* Busca la sala por nombre.
   Retorno:
     >=0 : índice de la sala encontrada
     -1  : no existe la sala 
*/
static int buscar_sala(const char* nombre){
    for(int i=0;i<num_salas;i++) if(strcmp(salas[i].nombre,nombre)==0) return i;
    return -1;
}

/* Cuenta usuarios activos en la sala (por nombre).
   Retorno:
     >=0 : número de usuarios (0 si no existe o está de momento vacío) 
*/
static int sala_count_fd(const char* sala){
    int idx=buscar_sala(sala); if(idx<0) return 0;
    return salas[idx].num_usuarios;
}

/* Agrega un usuario a la sala si hay capacidad.
   Parámetros: idx = índice de sala, nombre = nombre de usuario, fd = descriptor del cliente
   Retorno:
     1  : agregado correctamente
     0  : ya estaba presente (por fd)
    -2  : sala llena (capacidad alcanzada)
    -1  : error 
 */
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

/* Elimina un usuario de la sala por su fd.
   Retorno:
     0  : eliminado correctamente
    -1  : no encontrado o error 
*/
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
/* Encola en la lista de espera (FIFO) para la sala idx.
    Retorno:
     >0  : posición (1-based) en la cola después de encolar
      0  : ya estaba en la cola
     -1  : error u overflow
*/
static int sala_enqueue_espera(int idx, int fd, const char* nombre){
    if(idx<0||idx>=num_salas) return -1;
    struct sala *s=&salas[idx];
    // evitar duplicado
    for(int k=0;k<s->espera_len;k++){
        int pos=(s->espera_ini+k)%(MAX_USUARIOS_POR_SALA*2);
        if(s->espera_fds[pos]==fd) return 0; // ya estaba
    }
    if(s->espera_len >= (MAX_USUARIOS_POR_SALA*2)) return -1; // overflow
    s->espera_fds[s->espera_fin]=fd;
    strncpy(s->espera_nombres[s->espera_fin], nombre, MAX_NOMBRE-1);
    s->espera_nombres[s->espera_fin][MAX_NOMBRE-1] = '\0';
    s->espera_fin = (s->espera_fin+1)%(MAX_USUARIOS_POR_SALA*2);
    s->espera_len++;
    return s->espera_len; // retorna posición en cola
}
/* Desencola el primer elemento de la cola de espera.
   Si out_fd/out_nombre no son nulos, devuelve los datos del primero en espera.
   Retorno:
     1  : se devolvió un elemento y se avanzó la cola
     0  : cola vacía 
*/
static int sala_dequeue_espera(int idx, int* out_fd, char* out_nombre){
    if(idx<0||idx>=num_salas) return 0;
    struct sala *s=&salas[idx];
    if(s->espera_len==0) return 0;
    if(out_fd) *out_fd = s->espera_fds[s->espera_ini];
    if(out_nombre) {
        strncpy(out_nombre, s->espera_nombres[s->espera_ini], MAX_NOMBRE-1);
        out_nombre[MAX_NOMBRE-1] = '\0';
    }
    s->espera_ini = (s->espera_ini+1)%(MAX_USUARIOS_POR_SALA*2);
    s->espera_len--;
    return 1; 
}

static int sala_ya_en_espera(int idx, int fd){
    if(idx<0||idx>=num_salas) return 0;
    struct sala *s=&salas[idx];
    for(int k=0;k<s->espera_len;k++){
        int pos=(s->espera_ini+k)%(MAX_USUARIOS_POR_SALA*2);
        if(s->espera_fds[pos]==fd) return 1;
    }
    return 0;
}

static int sala_pos_espera(int idx, int fd){
    if(idx<0||idx>=num_salas) return -1;
    struct sala *s=&salas[idx]; 
    int pos=0;
    for(int k=0;k<s->espera_len;k++){
        int p=(s->espera_ini+k)%(MAX_USUARIOS_POR_SALA*2);
        pos++;
        if(s->espera_fds[p]==fd) return pos;
    }
    return -1; // no encontrado
}

/* ========== clientes conectados ========== */
typedef struct { int fd; int joined; char nombre[MAX_NOMBRE]; int sala_idx; } Client;
static Client clients[MAX_CLIENTS];

// Crea un socket de servidor TCP que escucha en <port> y devuelve el descriptor.
// Sale del proceso si algo falla.
static int make_server(uint16_t port){
    // 1) Crear el socket TCP (IPv4)
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); exit(1); }

    // 2) Reutilizar la dirección/puerto si quedó en TIME_WAIT
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    // 3) Estructura de dirección: 0.0.0.0:<port> (todas las interfaces)
    struct sockaddr_in a = {0};
    a.sin_family      = AF_INET;             // IPv4
    a.sin_addr.s_addr = INADDR_ANY;          // 0.0.0.0 (escuchar en todas las IP locales)
    a.sin_port        = htons(port);         // puerto en orden de red

    // 4) Asociar el socket a esa dirección/puerto
    if (bind(s, (struct sockaddr*)&a, sizeof a) < 0) {
        perror("bind"); exit(1);
    }

    // 5) Poner el socket en modo escucha con backlog (cola de pendientes)
    if (listen(s, 64) < 0) {                 // backlog "64" es tamaño sugerido
        perror("listen"); exit(1);
    }

    return s;                                 // listo: devolver socket de escucha
}

// printf-like: formatea en un buffer y lo envía por el socket 'fd'.
static void sendf(int fd, const char* fmt, ...){
    char b[2048];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);

    send(fd, b, strlen(b), 0);        // enviar el contenido del buffer
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

// Función para remover de cola de espera cuando cliente se desconecta
static void remove_from_all_wait_queues(int fd){
    for(int sala_idx = 0; sala_idx < num_salas; sala_idx++){
        struct sala *s = &salas[sala_idx];
        for(int k = 0; k < s->espera_len; k++){
            int pos = (s->espera_ini + k) % (MAX_USUARIOS_POR_SALA*2);
            if(s->espera_fds[pos] == fd){
                // Encontrado, remover desplazando el resto
                for(int j = k; j < s->espera_len - 1; j++){
                    int curr_pos = (s->espera_ini + j) % (MAX_USUARIOS_POR_SALA*2);
                    int next_pos = (s->espera_ini + j + 1) % (MAX_USUARIOS_POR_SALA*2);
                    s->espera_fds[curr_pos] = s->espera_fds[next_pos];
                    strncpy(s->espera_nombres[curr_pos], s->espera_nombres[next_pos], MAX_NOMBRE);
                }
                s->espera_len--;
                s->espera_fin = (s->espera_fin - 1 + (MAX_USUARIOS_POR_SALA*2)) % (MAX_USUARIOS_POR_SALA*2);
                printf("Cliente fd=%d removido de cola de espera de sala %s\n", fd, s->nombre);
                break;
            }
        }
    }
}
static void try_promote_from_wait(int idx_sala){
    if(idx_sala<0||idx_sala>=num_salas) return;
    struct sala *s=&salas[idx_sala];
    
    // mientras haya cupo y gente esperando
    while(s->num_usuarios < s->capacidad && s->espera_len > 0) {
        int wfd=-1; 
        char wname[MAX_NOMBRE]={0};
        
        if (!sala_dequeue_espera(idx_sala, &wfd, wname)) break;

        int cliente_idx = -1;
        for(int i=0; i<MAX_CLIENTS; i++){
            if(clients[i].fd == wfd){
                cliente_idx = i;
                break;
            }
        }
        
        if(cliente_idx == -1) {
            printf("Cliente en espera (fd=%d) desconectado, promoviendo siguiente...\n", wfd);
            continue;
        }
        
        // Intentar agregar a la sala
        if (sala_add_usuario_cap(idx_sala, wname, wfd) == 1) {
            // actualiza estado del cliente promovido
            clients[cliente_idx].joined = 1;
            clients[cliente_idx].sala_idx = idx_sala;
            strncpy(clients[cliente_idx].nombre, wname, MAX_NOMBRE-1);
            clients[cliente_idx].nombre[MAX_NOMBRE-1] = '\0';
            
            sendf(wfd, "Ya hay cupo en '%s'. Te unimos a la sala.\n", s->nombre);
            send_history_lastN(wfd, s->nombre, HISTORY_N);
            
            // Notif a los otros en la sala
            enviar_a_sala_menos_remitente(idx_sala, wfd, "", "nuevo usuario entró desde cola de espera");
            
            printf("Promovido: %s (fd %d) a sala %s (%d/%d)\n", wname, wfd, s->nombre, s->num_usuarios, s->capacidad);
        } else {
            // Si no se pudo agregar entonces reencolar al inicio
            printf("Error al promover usuario %s, sala llena inesperadamente\n", wname);
            break;
        }
    }
}

/* ========== main TCP con select() ========== */
int main(int argc, char** argv){
    uint16_t port = (argc>1)? (uint16_t)atoi(argv[1]) : 5555;
    // Crea el servidor TCP con la ip local y el puerto dado
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
                    sendf(cfd, "OK Bienvenido.\n");
                }
            }
        }

        for(int i=0;i<MAX_CLIENTS;i++){
            int fd=clients[i].fd; if(fd==-1) continue; if(!FD_ISSET(fd,&r)) continue;
            ssize_t n=recv(fd,buf,sizeof(buf)-1,0);
            if(n<=0){
                // Cliente desconectado
                printf("Cliente fd=%d (%s) desconectado\n", fd, clients[i].nombre);
                
                if(clients[i].joined){
                    int idx = clients[i].sala_idx;
                    enviar_a_sala_menos_remitente(idx, fd, "", "usuario salió");
                    sala_remove_usuario(idx, fd);
                    try_promote_from_wait(idx);
                }
                
                // Remover de todas las colas de espera
                remove_from_all_wait_queues(fd);
                
                close(fd); 
                clients[i].fd=-1; 
                clients[i].joined=0; 
                clients[i].sala_idx=-1;
                clients[i].nombre[0] = '\0';
                continue;
            }
            buf[n]='\0';

            // procesar por líneas
            char *s=buf, *e;
            while((e=strpbrk(s,"\r\n"))){
                *e='\0';
                if(*s){
                    char cmd[16]={0}; sscanf(s,"%15s",cmd);
                    for(char* p=cmd; *p; ++p) *p=tolower((unsigned char)*p);

                    /* Si el cliente aún no tiene nombre, tratar la primera línea que envía
                       como su nombre de usuario (autologin desde argumento del cliente). */
                    if(clients[i].nombre[0] == '\0'){
                        char namebuf[MAX_NOMBRE];
                        // copiar la línea completa y recortar espacios
                        strncpy(namebuf, s, MAX_NOMBRE-1); namebuf[MAX_NOMBRE-1] = '\0';
                        char *start = namebuf; while(*start && isspace((unsigned char)*start)) start++;
                        // trim trailing
                        char *end = start + strlen(start) - 1; while(end > start && isspace((unsigned char)*end)) *end-- = '\0';
                        if(strlen(start) > 0){
                            strncpy(clients[i].nombre, start, MAX_NOMBRE-1);
                            clients[i].nombre[MAX_NOMBRE-1] = '\0';
                            printf("Cliente fd=%d registró nombre '%s'\n", fd, clients[i].nombre);
                        }
                        s = e+1; while(*s=='\r'||*s=='\n') s++;
                        continue;
                    }
                    if(strcmp(cmd,"join")==0){
                        char sala[MAX_NOMBRE]={0};
                        if(sscanf(s+5,"%49s", sala)!=1 || strlen(sala)==0){
                            sendf(fd, "ERR uso: join <sala>\n");
                        } else {
                            // Verificar si ya está en otra sala
                            if(clients[i].joined){
                                sendf(fd, "Ya estás en la sala '%s'. Usa 'leave' primero.\n", salas[clients[i].sala_idx].nombre);
                                s=e+1; while(*s=='\r'||*s=='\n') s++; continue;
                            }
                            const char* user = clients[i].nombre;
                            if(strlen(user) == 0){
                                sendf(fd, "ERR primero establece tu usuario con: user <nombre>\n");
                                s=e+1; while(*s=='\r'||*s=='\n') s++; continue;
                            }
                            int idx = buscar_sala(sala);
                            if(idx >= 0 && sala_ya_en_espera(idx, fd)){
                                sendf(fd, "Sala '%s' ocupada. Sigues en espera. Te avisaremos cuando haya cupo.\n", sala);
                                s=e+1; while(*s=='\r'||*s=='\n') s++; continue;
                            }
                            
                            // Crear sala si no existe
                            if (idx==-1){
                                idx = crear_sala(sala);
                                if (idx==-1){ 
                                    sendf(fd,"No se pudo crear la sala %s\n", sala); 
                                    s=e+1; while(*s=='\r'||*s=='\n') s++; continue; 
                                }
                                rooms_file_add_if_new(sala);
                                printf("Nueva sala creada: %s\n", sala);
                            }
                            
                            int r = sala_add_usuario_cap(idx, user, fd);
                            if (r==1){
                                // Entrada exitosa
                                clients[i].joined=1; 
                                clients[i].sala_idx=idx;
                                sendf(fd, "Te has unido a la sala: %s\n", sala);
                                send_history_lastN(fd, sala, HISTORY_N);
                                enviar_a_sala_menos_remitente(idx, fd, "", "nuevo usuario entró");
                                printf("Usuario %s se unió a la sala %s\n", user, sala);
                            } else if (r==-2){

                                int queue_result = sala_enqueue_espera(idx, fd, user);
                                if(queue_result > 0){
                                    sendf(fd, "Sala '%s' ocupada. Quedaste en espera (pos=%d). Te avisaremos cuando haya cupo.\n", 
                                          sala, queue_result);
                                    printf("Sala '%s' llena. %s queda en espera (pos=%d)\n", sala, user, queue_result);
                                } else {
                                    sendf(fd, "Sala '%s' está llena y la cola de espera también está completa. Intenta más tarde.\n", sala);
                                }
                            } else if (r==0){
                                // Ya estaba en la sala
                                clients[i].joined=1; 
                                clients[i].sala_idx=idx;
                                sendf(fd, "Ya estás en la sala: %s\n", sala);
                                send_history_lastN(fd, sala, HISTORY_N);
                            } else {
                                sendf(fd, "Error al unirte a la sala %s\n", sala);
                            }
                        }
                    }
                    else if(strcmp(cmd,"leave")==0){
                        char sala_especificada[MAX_NOMBRE]={0};
                        if(sscanf(s+6,"%49s", sala_especificada) != 1 || strlen(sala_especificada) == 0){
                            sendf(fd, "ERR uso: leave <sala>\n");
                            s=e+1; while(*s=='\r'||*s=='\n') s++; continue;
                        }

                        if(!clients[i].joined){ 
                            sendf(fd, "No estás en ninguna sala.\n"); 
                        } else {
                            int idx=clients[i].sala_idx;
                            char sala_actual[MAX_NOMBRE]; 
                            strcpy(sala_actual, salas[idx].nombre);
                            
                            // Validar que coincida
                            if(strcmp(sala_especificada, sala_actual) != 0){
                                sendf(fd, "No estás en la sala '%s'. Estás en '%s'. Usa 'leave %s'.\n", 
                                      sala_especificada, sala_actual, sala_actual);
                                s=e+1; while(*s=='\r'||*s=='\n') s++; continue;
                            }
                            
                            // Salir de la sala
                            enviar_a_sala_menos_remitente(idx, fd, "", "usuario salió");
                            sala_remove_usuario(idx, fd);
                            clients[i].joined=0; 
                            clients[i].sala_idx=-1;
                            sendf(fd, "Has salido de la sala: %s\n", sala_actual);
                            printf("Usuario %s salió de la sala %s\n", clients[i].nombre, sala_actual);
                            
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
                    else if (strcasecmp(s,"show all rooms")==0){
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
                    else if (strcasecmp(s,"show rooms")==0){
                        // Salas activas (con usuarios)
                        int printed=0; sendf(fd,"Salas activas:\n");
                        for(int a=0;a<num_salas;a++){
                            if(salas[a].num_usuarios>0){
                                sendf(fd," - %s (%d/%d usuarios", salas[a].nombre, salas[a].num_usuarios, salas[a].capacidad);
                                if(salas[a].espera_len > 0){
                                    sendf(fd, ", %d en espera", salas[a].espera_len);
                                }
                                sendf(fd, ")\n");
                                printed=1;
                            }
                        }
                        if(!printed) sendf(fd,"(ninguna)\n");
                    }
                    else if (strcasecmp(s,"status")==0 || strcasecmp(s,"estado")==0){
                        if(clients[i].joined){
                            sendf(fd,"Estado: En sala '%s'\n", salas[clients[i].sala_idx].nombre);
                        } else {
                            // Verificar si está en alguna cola de espera
                            int en_espera = 0;
                            for(int sala_idx = 0; sala_idx < num_salas && !en_espera; sala_idx++){
                                int pos = sala_pos_espera(sala_idx, fd);
                                if(pos > 0){
                                    sendf(fd,"Estado: En cola de espera para sala '%s' (posición %d)\n", 
                                          salas[sala_idx].nombre, pos);
                                    en_espera = 1;
                                }
                            }
                            if(!en_espera){
                                sendf(fd,"Estado: No estás en ninguna sala ni en cola de espera\n");
                            }
                        }
                    }
                    else if(strcmp(cmd,"help")==0 || strcmp(cmd,"-help")==0){
                        sendf(fd,
                              "Comandos disponibles:\n"
                              " join <sala> - Unirte a una sala (si está llena, quedas en espera)\n"
                              " leave <sala> - Salir de tu sala actual\n"
                              " show rooms - Muestra las salas activas\n"
                              " show all rooms - Muestra todas las salas registradas\n"
                              " show users - Muestra los usuarios de tu sala actual\n"
                              " show all users - Muestra los usuarios de todas las salas\n"
                              " status - Muestra tu estado actual en sala o espera\n"
                              " \n");
                    }
                    else if(strcmp(cmd,"msg")==0){
                        if(!clients[i].joined){ sendf(fd,"ERR primero join\n"); }
                        else{
                            const char* text=s+4; if(!*text){ sendf(fd,"ERR texto vacío\n"); }
                            else{
                                struct sala *sa=&salas[clients[i].sala_idx];
                                loguear_mensaje(sa->nombre, clients[i].nombre, text);
                                enviar_a_sala_menos_remitente(clients[i].sala_idx, fd, clients[i].nombre, text);
                            }
                        }
                    }
                    else{
                        // texto libre -> mensaje (igual que antes en mensajes V)
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