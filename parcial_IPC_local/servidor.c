// Incluye estandares de C y librerias de IPC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/file.h>

// Definicion de constantes
#define MAX_SALAS 10
#define MAX_USUARIOS_POR_SALA 20
#define MAX_TEXTO 256
#define MAX_NOMBRE 50
#define LOG_DIR "logs" // Directorio para logs (Persistencia de mensajes)
#define CAPACIDAD_SALA_POR_DEFECTO 3
#define MAX_EN_ESPERA 64

// Definicion de mtype (tipo de mensaje) necesario para la clasificación de mensajes
// que se envian a la cola de mensajes global. Cada tipo de mensaje representa una acción
// o comando específico que el servidor debe manejar.
#define MT_GLOBAL_JOIN 1L
#define MT_GLOBAL_SEND 3L
#define MT_GLOBAL_SHOW 5L
#define MT_GLOBAL_SHOW_ALL 7L
#define MT_GLOBAL_LEAVE 9L
#define MT_GLOBAL_SHOW_USERS 11L
#define MT_GLOBAL_SHOW_ALL_USERS 13L

// Definicion de comandos (cmd) que indican la acción específica que se debe realizar
#define CMD_JOIN 1
#define CMD_SEND 2
#define CMD_SHOW 3
#define CMD_INFO 4
#define CMD_HISTORY 6
#define CMD_SHOW_ALL 8
#define CMD_LEAVE 9
#define CMD_SHOW_USERS 11
#define CMD_SHOW_ALL_USERS 13

// Definición de estruct de mensaje
// Campos:
//      - mtype: Tipo de mensaje (usado por la cola de mensajes)
//      - cmd: Comando específico (acción a realizar)
//      - client_qid: ID de la cola de mensajes del cliente
//      - pid: ID del proceso del cliente que envía el mensaje
//      - sala_qid: ID de la cola de mensajes de la sala (usado en respuestas JOIN y en la promoción desde espera)
//      - remitente: Nombre del usuario que envía el mensaje
//      - texto: Contenido del mensaje
//      - sala: Nombre de la sala a la que se refiere el mensaje
struct mensaje {
    long mtype;
    int cmd;
    pid_t pid;
    int client_qid;
    int sala_qid;   // qid de la sala (respuesta JOIN)
    char remitente[MAX_NOMBRE];
    char texto[MAX_TEXTO];
    char sala[MAX_NOMBRE]; // nombre de sala
};

// Tamaño del mensaje sin incluir mtype
#define MSGSIZE (sizeof(struct mensaje) - sizeof(long))

// Definición de Struct para gestionar usuarios 
// Campos:
//      - pid: ID del proceso del usuario
//      - nombre: Nombre del usuario

struct usuario {
    pid_t pid;
    char nombre[MAX_NOMBRE];
};

// Definición de Struct para gestionar salas de chat
// Campos:
//      - nombre: Nombre de la sala
//      - cola_id: ID de la cola de mensajes asociada a la sala
//      - num_usuarios: Número actual de usuarios en la sala
//      - usuarios: Array de usuarios en la sala
//      - capacidad: Capacidad máxima de usuarios en la sala
//      - espera_pids: Array de PIDs de usuarios en espera para entrar a la sala
//      - espera_nombres: Array de nombres de usuarios en espera
//      - espera_ini, espera_fin, espera_len: Variables para gestionar la cola circular de espera
struct sala {
    char nombre[MAX_NOMBRE];
    int  cola_id;
    int  num_usuarios;
    struct usuario usuarios[MAX_USUARIOS_POR_SALA];
    int  capacidad;
    pid_t espera_pids[MAX_EN_ESPERA];
    char  espera_nombres[MAX_EN_ESPERA][MAX_NOMBRE];
    int   espera_ini, espera_fin, espera_len;
};

// Array de salas y contador de salas
struct sala salas[MAX_SALAS];
int num_salas = 0;

// Sala_existe_archivo: Verifica si una sala ya existe en el archivo "salas.txt"
// Parametros:
//      - nombre: Nombre de la sala a verificar
// Retorna:
//      - true si la sala existe en el archivo "salas.txt", false en caso
static bool sala_existe_archivo(const char *nombre) {
    FILE *f = fopen("salas.txt", "r");
    if (!f) return false;
    char linea[MAX_NOMBRE + 8];
    bool existe = false;
    while (fgets(linea, sizeof(linea), f)) {
        linea[strcspn(linea, "\r\n")] = '\0';
        if (strcmp(linea, nombre) == 0) { existe = true; break; }
    }
    fclose(f);
    return existe;
}

// guardar_sala_si_nueva: Guarda el nombre de una sala en el archivo "salas.txt" si no existe ya
// Parametros:
//      - nombre: Nombre de la sala a guardar
static void guardar_sala_si_nueva(const char *nombre) {
    if (sala_existe_archivo(nombre)) return;
    FILE *f = fopen("salas.txt", "a");
    if (!f) { perror("guardar_sala_si_nueva fopen"); return; }
    fprintf(f, "%s\n", nombre);
    fclose(f);
}


static void ruta_log_sala(const char* nombre_sala, char* path, size_t sz) {
    snprintf(path, sz, LOG_DIR "/sala_%s.jsonl", nombre_sala);
}

static void json_escape(const char* in, char* out, size_t outsz) {
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 6 < outsz; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\\' || c == '\"') { out[j++]='\\'; out[j++]=c; }
        else if (c == '\n') { out[j++]='\\'; out[j++]='n'; }
        else if (c == '\r') { out[j++]='\\'; out[j++]='r'; }
        else if (c == '\t') { out[j++]='\\'; out[j++]='t'; }
        else if (c < 0x20) { j += snprintf(out+j, outsz-j, "\\u%04x", c); }
        else { out[j++] = c; }
    }
    out[j] = '\0';
}

static void loguear_mensaje(const char* sala, const char* remitente, const char* texto) {
    if (!sala || !*sala) return;
    char path[512]; ruta_log_sala(sala, path, sizeof(path));

    FILE* f = fopen(path, "a");
    if (!f) {
        f = fopen(path, "a");
        if (!f) { perror("fopen log"); return; }
    }

    int fd = fileno(f);
    flock(fd, LOCK_EX);

    time_t ts = time(NULL);
    char esc_sala[256], esc_from[256], esc_text[1024];
    json_escape(sala,      esc_sala, sizeof esc_sala);
    json_escape(remitente, esc_from, sizeof esc_from);
    json_escape(texto,     esc_text, sizeof esc_text);

    fprintf(f, "{\"ts\":%ld,\"sala\":\"%s\",\"from\":\"%s\",\"text\":\"%s\"}\n",
            (long)ts, esc_sala, esc_from, esc_text);

    fflush(f);
    flock(fd, LOCK_UN);
    fclose(f);
}

static void json_unescape(const char* in, char* out, size_t outsz){
    size_t j=0;
    for (size_t i=0; in && in[i] && j+1 < outsz; ++i){
        if (in[i] == '\\'){
            char c = in[++i];
            if      (c == 'n')  out[j++] = '\n';
            else if (c == 'r')  out[j++] = '\r';
            else if (c == 't')  out[j++] = '\t';
            else if (c == '\\') out[j++] = '\\';
            else if (c == '\"') out[j++] = '\"';
            else                out[j++] = c;
        } else {
            out[j++] = in[i];
        }
    }
    out[j]='\0';
}

static void extraer_from_y_text(const char* json,
                                char* from, size_t fromsz,
                                char* text, size_t textsz)
{
    const char *pf = strstr(json, "\"from\":\"");
    const char *pt = strstr(json, "\"text\":\"");
    if (!pf || !pt){ from[0]='\0'; text[0]='\0'; return; }

    pf += 8; pt += 8;
    char bufF[512]={0}, bufT[1024]={0};
    size_t i=0;
    for (; pf[i] && pf[i] != '\"'; ++i){
        if (pf[i]=='\\' && pf[i+1]) bufF[i++] = pf[i], bufF[i]=pf[i];
        else bufF[i] = pf[i];
        if (i+1 >= sizeof(bufF)) break;
    }
    bufF[i]='\0';

    i=0;
    for (; pt[i] && pt[i] != '\"'; ++i){
        if (pt[i]=='\\' && pt[i+1]) bufT[i++] = pt[i], bufT[i]=pt[i];
        else bufT[i] = pt[i];
        if (i+1 >= sizeof(bufT)) break;
    }
    bufT[i]='\0';

    json_unescape(bufF, from, fromsz);
    json_unescape(bufT, text, textsz);
}

static void enviar_historial_ultimos(const char* sala, int qid_sala, pid_t pid_dest, int N){
    if (!sala || !*sala || N <= 0) return;

    char path[512];
    ruta_log_sala(sala, path, sizeof(path));

    FILE* f = fopen(path, "r");
    if (!f) return;

    char **ring = calloc(N, sizeof(char*));
    if (!ring){ fclose(f); return; }
    size_t ring_i = 0, count = 0;

    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) != -1){
        free(ring[ring_i]);
        ring[ring_i] = strdup(line);
        ring_i = (ring_i + 1) % N;
        if (count < (size_t)N) count++;
    }
    free(line);
    fclose(f);

    size_t start = (count == (size_t)N) ? ring_i : 0;
    for (size_t k = 0; k < count; ++k){
        const char* jsonl = ring[(start + k) % N];
        if (!jsonl) continue;

        char autor[MAX_NOMBRE] = {0};
        char texto[MAX_TEXTO]  = {0};
        extraer_from_y_text(jsonl, autor, sizeof autor, texto, sizeof texto);

        struct mensaje out = {0};
        out.mtype = (long)pid_dest;
        out.cmd   = CMD_HISTORY;
        strncpy(out.remitente, autor, sizeof(out.remitente)-1);
        strncpy(out.texto,     texto, sizeof(out.texto)-1);

        msgsnd(qid_sala, &out, MSGSIZE, 0);
    }

    for (size_t k = 0; k < (size_t)N; ++k) free(ring[k]);
    free(ring);
}


int crear_sala(const char *nombre) {
    if (num_salas >= MAX_SALAS) return -1;

    key_t key = ftok("/tmp", (int)('A' + num_salas));
    int cola_id = msgget(key, IPC_CREAT | 0666);
    if (cola_id == -1) { perror("Error al crear la cola de la sala"); return -1; }

    strcpy(salas[num_salas].nombre, nombre);
    salas[num_salas].cola_id      = cola_id;
    salas[num_salas].num_usuarios = 0;
    salas[num_salas].capacidad    = CAPACIDAD_SALA_POR_DEFECTO;
    salas[num_salas].espera_ini   = 0;
    salas[num_salas].espera_fin   = 0;
    salas[num_salas].espera_len   = 0;

    num_salas++;
    return num_salas - 1;
}

int buscar_sala(const char *nombre) {
    for (int i = 0; i < num_salas; i++)
        if (strcmp(salas[i].nombre, nombre) == 0) return i;
    return -1;
}

// cargar_salas_desde_archivo: Carga los nombres de las salas desde el archivo "salas.txt"
// y crea las salas en memoria si no existen ya.
static void cargar_salas_desde_archivo(void) {
    FILE *f = fopen("salas.txt", "r");
    if (!f) return;
    char nombre[MAX_NOMBRE];
    while (fgets(nombre, sizeof(nombre), f)) {
        nombre[strcspn(nombre, "\r\n")] = '\0';
        if (buscar_sala(nombre) >= 0) continue;
        int id = crear_sala(nombre);
        if (id >= 0) salas[id].num_usuarios = 0;
    }
    printf("Salas cargadas desde archivo: %d\n", num_salas);
    fclose(f);
}

// Sala_enqueue_espera: Agrega un usuario a la cola de espera de una sala
// Parametros:
//      - idx: Índice de la sala en el array de salas
//      - pid: PID del usuario a agregar a la espera
//      - nombre: Nombre del usuario a agregar a la espera
// Retorna:
//      - Posición en la cola de espera si se agregó correctamente
static int sala_enqueue_espera(int idx, pid_t pid, const char* nombre) {
    struct sala* s = &salas[idx];
    if (s->espera_len >= MAX_EN_ESPERA) return -1;
    s->espera_pids[s->espera_fin] = pid;
    strncpy(s->espera_nombres[s->espera_fin], nombre, MAX_NOMBRE);
    s->espera_nombres[s->espera_fin][MAX_NOMBRE-1] = '\0';
    s->espera_fin = (s->espera_fin + 1) % MAX_EN_ESPERA;
    s->espera_len++;
    return s->espera_len;
}

// Sala_dequeue_espera: Remueve y obtiene el siguiente usuario en la cola de espera de una sala
// Parametros:
//      - idx: Índice de la sala en el array de salas
//      - pid_out: Puntero para almacenar el PID del usuario removido (puede ser NULL)
//      - nombre_out: Puntero para almacenar el nombre del usuario removido (puede ser NULL)
// Retorna:
//      - 1 si se removió un usuario, 0 si la cola de espera está vacía
static int sala_dequeue_espera(int idx, pid_t* pid_out, char* nombre_out) {
    struct sala* s = &salas[idx];
    if (s->espera_len == 0) return 0;
    if (pid_out) *pid_out = s->espera_pids[s->espera_ini];
    if (nombre_out) {
        strncpy(nombre_out, s->espera_nombres[s->espera_ini], MAX_NOMBRE);
        nombre_out[MAX_NOMBRE-1] = '\0';
    }
    s->espera_ini = (s->espera_ini + 1) % MAX_EN_ESPERA;
    s->espera_len--;
    return 1;
}

// sala_ya_en_espera: Verifica si un usuario ya está en la cola de espera de una sala
// Parametros:
//      - idx: Índice de la sala en el array de salas
//      - pid: PID del usuario a verificar
static int sala_ya_en_espera(int idx, pid_t pid) {
    struct sala* s = &salas[idx];
    for (int k = 0; k < s->espera_len; k++) {
        int pos = (s->espera_ini + k) % MAX_EN_ESPERA;
        if (s->espera_pids[pos] == pid) return 1;
    }
    return 0;
}

// sala_add_usuario_cap: Agrega un usuario a una sala si hay capacidad
// Parametros:
//      - idx: Índice de la sala en el array de salas
//      - nombre: Nombre del usuario a agregar
//      - pid: PID del usuario a agregar
// Retorna:
//      - 1=agregado
//      - 0=ya estaba
//      - 2=capacidad llena
//      - -1=overflow
static int sala_add_usuario_cap(int idx, const char* nombre, pid_t pid) {
    struct sala* s = &salas[idx];
    if (s->num_usuarios >= s->capacidad)             return -2;
    if (s->num_usuarios >= MAX_USUARIOS_POR_SALA)    return -1;

    for (int i=0;i<s->num_usuarios;i++)
        if (s->usuarios[i].pid == pid) return 0;

    s->usuarios[s->num_usuarios].pid = pid;
    strncpy(s->usuarios[s->num_usuarios].nombre, nombre, MAX_NOMBRE);
    s->usuarios[s->num_usuarios].nombre[MAX_NOMBRE-1] = '\0';
    s->num_usuarios++;
    return 1;
}

// ==== util envío a sala ====
void enviar_a_sala_menos_remitente(int indice_sala, const char *remitente, const char *texto) {
    if (indice_sala < 0 || indice_sala >= num_salas) return;
    struct sala *s = &salas[indice_sala];

    for (int i = 0; i < s->num_usuarios; i++) {
        if (strcmp(s->usuarios[i].nombre, remitente) == 0) continue;

        struct mensaje out = {0};
        out.mtype = (long)s->usuarios[i].pid;
        out.cmd   = CMD_SEND;
        strncpy(out.remitente, remitente, MAX_NOMBRE);
        strncpy(out.texto, texto, MAX_TEXTO);

        if (msgsnd(s->cola_id, &out, MSGSIZE, 0) == -1) perror("Error al reenviar a la sala");
    }
}

void notificar_sala_excluyendo(int indice_sala, const char *nombre_excluir, const char *remitente_mostrar, const char *texto) {
    if (indice_sala < 0 || indice_sala >= num_salas) return;
    struct sala *s = &salas[indice_sala];

    for (int i = 0; i < s->num_usuarios; i++) {
        if (strcmp(s->usuarios[i].nombre, nombre_excluir) == 0) continue;

        struct mensaje out = {0};
        out.mtype = (long)s->usuarios[i].pid;
        out.cmd   = CMD_SEND;
        strncpy(out.remitente, remitente_mostrar ? remitente_mostrar : "", MAX_NOMBRE-1);
        strncpy(out.texto, texto ? texto : "", MAX_TEXTO-1);

        if (msgsnd(s->cola_id, &out, MSGSIZE, 0) == -1)
            perror("Error al notificar a la sala");
    }
}

static int encontrar_sala_por_pid(pid_t pid_busca) {
    for (int i = 0; i < num_salas; i++)
        for (int j = 0; j < salas[i].num_usuarios; j++)
            if (salas[i].usuarios[j].pid == pid_busca) return i;
    return -1;
}

static void listar_usuarios_de_sala_en_texto(int idx_sala, char *dst, size_t dstsz) {
    if (idx_sala < 0 || idx_sala >= num_salas || dstsz == 0) { if (dstsz) dst[0]='\0'; return; }
    size_t off = 0; int rem = (int)dstsz;

    const char *nombre = salas[idx_sala].nombre;
    int n = snprintf(dst + off, rem, "Sala: %s (%d usuarios)\n", nombre, salas[idx_sala].num_usuarios);
    if (n < 0) return; if (n >= rem){ dst[dstsz-1]='\0'; return; }
    off += n; rem -= n;

    for (int u = 0; u < salas[idx_sala].num_usuarios && rem > 1; u++) {
        n = snprintf(dst + off, rem, " - %s\n", salas[idx_sala].usuarios[u].nombre);
        if (n < 0) break; if (n >= rem){ dst[dstsz-1]='\0'; break; }
        off += n; rem -= n;
    }
}

static void listar_todos_los_usuarios_en_texto(char *dst, size_t dstsz) {
    if (dstsz == 0) return;
    size_t off = 0; int rem = (int)dstsz;

    int n = snprintf(dst + off, rem, "Usuarios por sala:\n");
    if (n < 0) return; if (n >= rem){ dst[dstsz-1]='\0'; return; }
    off += n; rem -= n;

    for (int i = 0; i < num_salas && rem > 1; i++) {
        n = snprintf(dst + off, rem, "[%s] (%d)\n", salas[i].nombre, salas[i].num_usuarios);
        if (n < 0) break; if (n >= rem){ dst[dstsz-1]='\0'; break; }
        off += n; rem -= n;

        for (int u = 0; u < salas[i].num_usuarios && rem > 1; u++) {
            n = snprintf(dst + off, rem, " - %s\n", salas[i].usuarios[u].nombre);
            if (n < 0) break; if (n >= rem){ dst[dstsz-1]='\0'; break; }
            off += n; rem -= n;
        }
    }
}

int main() {
    key_t key_global = ftok("/tmp", 'G');
    int cola_global = msgget(key_global, IPC_CREAT | 0666);
    if (cola_global == -1) { perror("Error al crear la cola global"); exit(1); }

    printf("Servidor de chat iniciado. Esperando clientes...\n");
    cargar_salas_desde_archivo();

    struct mensaje msg;

    while (1) {
        if (msgrcv(cola_global, &msg, MSGSIZE, 0, 0) == -1) {
            perror("Error al recibir mensaje");
            continue;
        }

        // JOIN
        if (msg.mtype == MT_GLOBAL_JOIN && msg.cmd == CMD_JOIN) {
            int indice_sala = buscar_sala(msg.sala);
            if (indice_sala == -1) {
                indice_sala = crear_sala(msg.sala);
                if (indice_sala == -1) { printf("No se pudo crear la sala %s\n", msg.sala); continue; }
                guardar_sala_si_nueva(msg.sala);
                printf("Nueva sala creada: %s\n", msg.sala);
            }

            int add = sala_add_usuario_cap(indice_sala, msg.remitente, msg.pid);
            if (add == 1) {
                printf("Usuario %s agregado a la sala %s (%d/%d)\n",
                       msg.remitente, msg.sala,
                       salas[indice_sala].num_usuarios, salas[indice_sala].capacidad);

                // 1) JOIN OK -> GLOBAL (con sala y qid)
                struct mensaje out = (struct mensaje){0};
                out.mtype    = (long)msg.pid;
                out.cmd      = CMD_JOIN;
                out.sala_qid = salas[indice_sala].cola_id;
                strncpy(out.sala, salas[indice_sala].nombre, sizeof(out.sala)-1);
                snprintf(out.texto, MAX_TEXTO, "Te has unido a la sala: %s", msg.sala);
                if (msgsnd(cola_global, &out, MSGSIZE, 0) == -1) perror("Error al enviar confirmación");
                // --- avisar a los demás que <usuario> se unió (excluye al que entró)
                char mensaje_sistema[MAX_TEXTO];
                snprintf(mensaje_sistema, MAX_TEXTO, "[%s] se unió a la sala.", msg.remitente);
                notificar_sala_excluyendo(indice_sala, msg.remitente, "", mensaje_sistema);

                // 2) Historial -> cola de la sala
                enviar_historial_ultimos(msg.sala, salas[indice_sala].cola_id, msg.pid, 10);
            }
            else if (add == -2) {
                // cSi a tiene una solicitud de espera, notificarlo
                if (sala_ya_en_espera(indice_sala, msg.pid)) {
                    struct mensaje out = (struct mensaje){0};
                    out.mtype = (long)msg.pid; out.cmd = CMD_INFO;
                    snprintf(out.texto, MAX_TEXTO,
                             "Sala '%s' ocupada. Sigues en espera. Te avisaremos cuando haya cupo.",
                             msg.sala);
                    msgsnd(cola_global, &out, MSGSIZE, 0);
                } else {
                    // Agregar a sala de espera
                    int pos = sala_enqueue_espera(indice_sala, msg.pid, msg.remitente);
                    printf("Sala '%s' llena. %s queda en espera (pos=%d)\n", msg.sala, msg.remitente, pos);

                    struct mensaje out = (struct mensaje){0};
                    out.mtype = (long)msg.pid; out.cmd = CMD_INFO;
                    snprintf(out.texto, MAX_TEXTO,
                             "Sala '%s' ocupada. Quedaste en espera (pos=%d). Te avisaremos cuando haya cupo.",
                             msg.sala, pos);
                    msgsnd(cola_global, &out, MSGSIZE, 0);
                }
            }
            else {
                printf("No se pudo agregar al usuario %s a la sala %s (err=%d)\n",
                       msg.remitente, msg.sala, add);
            }
        }

        // SEND
        else if (msg.mtype == MT_GLOBAL_SEND && msg.cmd == CMD_SEND) {
            int indice_sala = buscar_sala(msg.sala);
            if (indice_sala != -1) {
                printf("Mensaje en la sala %s de %s: %s\n", msg.sala, msg.remitente, msg.texto);
                loguear_mensaje(msg.sala, msg.remitente, msg.texto);
                enviar_a_sala_menos_remitente(indice_sala, msg.remitente, msg.texto);
            }
        }

        // Acción del comando SHOW: ver la lista de salas activas (con usuarios)
        else if (msg.mtype == MT_GLOBAL_SHOW && msg.cmd == CMD_SHOW) {
            struct mensaje out = {0};
            out.mtype = msg.pid;
            out.cmd = CMD_INFO;

            int activas = 0;
            for (int i = 0; i < num_salas; i++)
                if (salas[i].num_usuarios > 0) activas++;

            if (activas == 0) {
                snprintf(out.texto, MAX_TEXTO, "No hay salas activas.");
            } else {
                printf("Enviando lista de salas activas a PID %d\n", msg.pid);
                size_t offset = 0;
                offset += snprintf(out.texto + offset, MAX_TEXTO - offset, "Salas activas:\n");
                for (int i = 0; i < num_salas && offset < MAX_TEXTO; i++) {
                    if (salas[i].num_usuarios <= 0) continue;
                    offset += snprintf(out.texto + offset, MAX_TEXTO - offset,
                                       " - %s (%d usuarios)\n",
                                       salas[i].nombre, salas[i].num_usuarios);
                }
                out.texto[MAX_TEXTO - 1] = '\0';
            }
            msgsnd(cola_global, &out, MSGSIZE, 0);
        }

        // Acción del comando SHOW_ALL: ver la lista de todas las salas (incluso vacías)
        else if (msg.mtype == MT_GLOBAL_SHOW_ALL && msg.cmd == CMD_SHOW_ALL) {
            struct mensaje out = {0};
            out.mtype = msg.pid;
            out.cmd = CMD_INFO;

            if (num_salas == 0) {
                snprintf(out.texto, MAX_TEXTO, "No hay salas registradas.");
            } else {
                printf("Enviando lista de todas las salas a PID %d\n", msg.pid);
                size_t offset = 0;
                offset += snprintf(out.texto + offset, MAX_TEXTO - offset,
                                   "Salas registradas (todas):\n");
                for (int i = 0; i < num_salas && offset < MAX_TEXTO; i++) {
                    offset += snprintf(out.texto + offset, MAX_TEXTO - offset,
                                       " - %s (%d activos)\n",
                                       salas[i].nombre, salas[i].num_usuarios);
                }
                out.texto[MAX_TEXTO - 1] = '\0';
            }
            msgsnd(cola_global, &out, MSGSIZE, 0);
        }

        // LEAVE
        else if (msg.mtype == MT_GLOBAL_LEAVE && msg.cmd == CMD_LEAVE) {
            int indice_sala = buscar_sala(msg.sala);
            if (indice_sala != -1) {
                // remover usuario por PID
                int removed = -1;
                for (int i = 0; i < salas[indice_sala].num_usuarios; i++) {
                    if (salas[indice_sala].usuarios[i].pid == msg.pid) {
                        for (int j = i; j < salas[indice_sala].num_usuarios - 1; j++)
                            salas[indice_sala].usuarios[j] = salas[indice_sala].usuarios[j + 1];
                        salas[indice_sala].num_usuarios--;
                        removed = 0;
                        break;
                    }
                }

                if (removed == 0) {
                    printf("Usuario %s ha salido de la sala %s (ahora %d/%d)\n",
                           msg.remitente, msg.sala,
                           salas[indice_sala].num_usuarios, salas[indice_sala].capacidad);

                    char mensaje_sistema[MAX_TEXTO];
                    snprintf(mensaje_sistema, MAX_TEXTO, "%s salió de la sala", msg.remitente);
                    enviar_a_sala_menos_remitente(indice_sala, "", mensaje_sistema);

                    struct mensaje out = {0};
                    out.mtype = (long)msg.pid;
                    out.cmd = CMD_LEAVE;
                    snprintf(out.texto, MAX_TEXTO, "Has salido de la sala: %s", msg.sala);
                    msgsnd(cola_global, &out, MSGSIZE, 0);

                    // promoción desde espera
                    if (salas[indice_sala].espera_len > 0) {
                        pid_t pid_next; char nombre_next[MAX_NOMBRE];
                        if (sala_dequeue_espera(indice_sala, &pid_next, nombre_next) == 1) {
                            int add2 = sala_add_usuario_cap(indice_sala, nombre_next, pid_next);
                            if (add2 == 1) {
                                struct mensaje promote = (struct mensaje){0};
                                promote.mtype    = (long)pid_next;
                                promote.cmd      = CMD_JOIN;
                                promote.sala_qid = salas[indice_sala].cola_id;
                                strncpy(promote.sala, salas[indice_sala].nombre, sizeof(promote.sala)-1);
                                snprintf(promote.texto, MAX_TEXTO,
                                         "Ya hay cupo en '%s'. Te unimos a la sala.",
                                         salas[indice_sala].nombre);

                                // 1) JOIN OK por GLOBAL
                                if (msgsnd(cola_global, &promote, MSGSIZE, 0) == -1)
                                    perror("Error al avisar promoción");

                                // 2) Historial por cola de sala
                                enviar_historial_ultimos(salas[indice_sala].nombre,
                                                         salas[indice_sala].cola_id, pid_next, 10);

                                // --- avisar a los demás que <nombre_next> se unió por promoción
                                char mensaje_sistema[MAX_TEXTO];
                                snprintf(mensaje_sistema, MAX_TEXTO, "[%s] se unió a la sala.", msg.remitente);
                                notificar_sala_excluyendo(indice_sala, msg.remitente, "", mensaje_sistema);

                                printf("Promovido: %s (PID %d) a sala %s (%d/%d)\n",
                                       nombre_next, (int)pid_next,
                                       salas[indice_sala].nombre,
                                       salas[indice_sala].num_usuarios, salas[indice_sala].capacidad);
                            }
                        }
                    }
                } else {
                    struct mensaje out = {0};
                    out.mtype = (long)msg.pid;
                    out.cmd = CMD_LEAVE;
                    snprintf(out.texto, MAX_TEXTO, "Error: No estabas en la sala %s", msg.sala);
                    msgsnd(cola_global, &out, MSGSIZE, 0);
                }
            } else {
                struct mensaje out = {0};
                out.mtype = (long)msg.pid;
                out.cmd = CMD_LEAVE;
                snprintf(out.texto, MAX_TEXTO, "Error: La sala %s no existe", msg.sala);
                msgsnd(cola_global, &out, MSGSIZE, 0);
            }
        }

        // SHOW USERS
        else if (msg.mtype == MT_GLOBAL_SHOW_USERS && msg.cmd == CMD_SHOW_USERS) {
            int idx = encontrar_sala_por_pid(msg.pid);

            struct mensaje out = {0};
            out.mtype = (long)msg.pid;
            out.cmd = CMD_SHOW_USERS;

            if (idx == -1)
                snprintf(out.texto, sizeof(out.texto), "No estás en ninguna sala. Usa 'join <sala>'.");
            else
                listar_usuarios_de_sala_en_texto(idx, out.texto, sizeof(out.texto));

            msgsnd(cola_global, &out, MSGSIZE, 0);
        }

        // SHOW ALL USERS
        else if (msg.mtype == MT_GLOBAL_SHOW_ALL_USERS && msg.cmd == CMD_SHOW_ALL_USERS) {
            struct mensaje out = {0};
            out.mtype = (long)msg.pid;
            out.cmd = CMD_SHOW_ALL_USERS;

            if (num_salas == 0)
                snprintf(out.texto, sizeof(out.texto), "No hay salas creadas.");
            else
                listar_todos_los_usuarios_en_texto(out.texto, sizeof(out.texto));

            msgsnd(cola_global, &out, MSGSIZE, 0);
        }
    }
    return 0;
}
