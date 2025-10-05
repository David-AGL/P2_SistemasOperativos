#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h> //Tipos básicos usados por IPC (Intercomutación entre procesos)
#include <sys/ipc.h>   // Manejo de colas de mensajes System V
#include <sys/msg.h>   // Manejo de colas de mensajes System V
#include <unistd.h>    // Unix standard
#include <time.h>
#include <sys/stat.h>
#include <sys/file.h>

#define MAX_SALAS 10
#define MAX_USUARIOS_POR_SALA 20
#define MAX_TEXTO 256
#define MAX_NOMBRE 50
#define LOG_DIR "logs"

// Constantes de control para la COLA GLOBAL
#define MT_GLOBAL_JOIN 1L
#define MT_GLOBAL_SEND 3L
#define MT_GLOBAL_SHOW 5L
#define MT_GLOBAL_SHOW_ALL 7L
#define MT_GLOBAL_LEAVE 9L
#define MT_GLOBAL_SHOW_USERS 11L
#define MT_GLOBAL_SHOW_ALL_USERS 13L

// Comandos semánticos dentro del payload
#define CMD_JOIN 1
#define CMD_SEND 2
#define CMD_SHOW 3
#define CMD_INFO 4
#define CMD_HISTORY 6
#define CMD_SHOW_ALL 8
#define CMD_LEAVE 9
#define CMD_SHOW_USERS 11
#define CMD_SHOW_ALL_USERS 13

// Estructura para los mensajes
struct mensaje
{
    long mtype;     // Tipo de mensaje
    int cmd;        // CMD_JOIN / CMD_SEND
    pid_t pid;      // PID del cliente
    int client_qid; // (reservado si luego usas cola privada)
    int sala_qid;   // qid de la sala (respuesta JOIN)
    char remitente[MAX_NOMBRE];
    char texto[MAX_TEXTO];
    char sala[MAX_NOMBRE];
};

#define MSGSIZE (sizeof(struct mensaje) - sizeof(long))

struct usuario
{
    pid_t pid;
    char nombre[MAX_NOMBRE];
};

struct sala
{
    char nombre[MAX_NOMBRE];
    int cola_id; // ID de la cola de la sala
    int num_usuarios;
    struct usuario usuarios[MAX_USUARIOS_POR_SALA];
};

struct sala salas[MAX_SALAS];
int num_salas = 0;

// Función para crear una nueva sala
int crear_sala(const char *nombre)
{
    if (num_salas >= MAX_SALAS)
    {
        return -1; // No se pueden crear más salas
    }

    // Crear una cola de mensajes para la sala
    key_t key = ftok("/tmp", (int)('A' + num_salas)); // Generar una clave única
    int cola_id = msgget(key, IPC_CREAT | 0666);
    if (cola_id == -1)
    {
        perror("Error al crear la cola de la sala");
        return -1;
    }

    // Inicializar la sala
    strcpy(salas[num_salas].nombre, nombre);
    salas[num_salas].cola_id = cola_id;
    salas[num_salas].num_usuarios = 0;

    num_salas++;
    return num_salas - 1; // Retornar el índice de la sala creada
}

// Función para buscar una sala por nombre
int buscar_sala(const char *nombre)
{
    for (int i = 0; i < num_salas; i++)
    {
        if (strcmp(salas[i].nombre, nombre) == 0)
        {
            return i;
        }
    }
    return -1; // No encontrada
}

// Función para verificar si una sala ya existe en "salas.txt"
static bool sala_existe_archivo(const char *nombre)
{
    FILE *f = fopen("salas.txt", "r");
    if (!f)
        return false;
    char linea[MAX_NOMBRE + 8];
    bool existe = false;
    while (fgets(linea, sizeof(linea), f))
    {
        linea[strcspn(linea, "\r\n")] = '\0';
        if (strcmp(linea, nombre) == 0)
        {
            existe = true;
            break;
        }
    }
    fclose(f);
    return existe;
}

// Función para guardar el nombre de una sala en "salas.txt" si no existe ya
static void guardar_sala_si_nueva(const char *nombre)
{
    if (sala_existe_archivo(nombre))
        return;
    FILE *f = fopen("salas.txt", "a");
    if (!f)
    {
        perror("guardar_sala_si_nueva fopen");
        return;
    }
    fprintf(f, "%s\n", nombre);
    fclose(f);
}

// Carga las salas desde "salas.txt" al iniciar el servidor
static void cargar_salas_desde_archivo(void)
{
    FILE *f = fopen("salas.txt", "r");
    if (!f)
        return; // nada que cargar
    char nombre[MAX_NOMBRE];
    while (fgets(nombre, sizeof(nombre), f))
    {
        nombre[strcspn(nombre, "\r\n")] = '\0';
        if (buscar_sala(nombre) >= 0)
            continue; // ya cargada en runtime
        // Crea si no existe. Usará tu esquema ftok basado en num_salas.
        int id = crear_sala(nombre);
        if (id >= 0)
        {
            // Cargadas desde disco arrancan "inactivas": 0 usuarios
            salas[id].num_usuarios = 0;
            // No guardes de nuevo al archivo (ya venía de archivo)
        }
    }
    printf("Salas cargadas desde archivo: %d\n", num_salas);
    fclose(f);
}

// Función para agregar un usuario a una sala
int agregar_usuario(int indice_sala, const char *nombre_usuario, pid_t pid)
{
    if (indice_sala < 0 || indice_sala >= num_salas)
        return -1;
    struct sala *s = &salas[indice_sala];
    if (s->num_usuarios >= MAX_USUARIOS_POR_SALA)
        return -1;

    // Evitar duplicados por nombre o PID
    for (int i = 0; i < s->num_usuarios; i++)
    {
        if (s->usuarios[i].pid == pid)
            return 0;
    }

    s->usuarios[s->num_usuarios].pid = pid;
    strncpy(s->usuarios[s->num_usuarios].nombre, nombre_usuario, MAX_NOMBRE);
    s->num_usuarios++;
    return 0;
}

// Función para remover LEAVE un usuario de una sala
int remover_usuario(int indice_sala, pid_t pid)
{
    if (indice_sala < 0 || indice_sala >= num_salas)
        return -1;

    struct sala *s = &salas[indice_sala];
    // buscamos el usuario por PID
    for (int i = 0; i < s->num_usuarios; i++)
    {
        if (s->usuarios[i].pid == pid)
        {
            // Mover el último usuario a esta posición para llenar el hueco
            for (int j = i; j < s->num_usuarios - 1; j++)
            {
                s->usuarios[j] = s->usuarios[j + 1];
            }
            s->num_usuarios--;
            return 0;
        }
    }
    return -1;
}

// Función para enviar el mensaje a todos los miembrios de una sala, excepto al remitente
void enviar_a_sala_menos_remitente(int indice_sala, const char *remitente, const char *texto)
{
    if (indice_sala < 0 || indice_sala >= num_salas)
        return;
    struct sala *s = &salas[indice_sala];

    for (int i = 0; i < s->num_usuarios; i++)
    {
        if (strcmp(s->usuarios[i].nombre, remitente) == 0)
            continue; // no al remitente

        struct mensaje out = {0};
        out.mtype = (long)s->usuarios[i].pid; // clave: mtype = PID del receptor
        out.cmd = CMD_SEND;
        strncpy(out.remitente, remitente, MAX_NOMBRE);
        strncpy(out.texto, texto, MAX_TEXTO);

        if (msgsnd(s->cola_id, &out, MSGSIZE, 0) == -1)
        {
            perror("Error al reenviar a la sala");
        }
    }
}

// Función para encontrar la sala en la que está un cliente específico
static int encontrar_sala_por_pid(pid_t pid_busca)
{
    for (int i = 0; i < num_salas; i++)
    {
        for (int j = 0; j < salas[i].num_usuarios; j++)
        {
            if (salas[i].usuarios[j].pid == pid_busca)
            {
                return i; // índice de la sala
            }
        }
    }
    return -1; // no está en ninguna sala
}

// Función para listar los usuarios de un canal
static void listar_usuarios_de_sala_en_texto(int idx_sala, char *dst, size_t dstsz)
{
    if (idx_sala < 0 || idx_sala >= num_salas || dstsz == 0)
    {
        if (dstsz)
            dst[0] = '\0';
        return;
    }
    size_t off = 0;
    int rem = (int)dstsz;

    const char *nombre = salas[idx_sala].nombre;
    int n = snprintf(dst + off, rem, "Sala: %s (%d usuarios)\n",
                     nombre, salas[idx_sala].num_usuarios);
    if (n < 0)
        return;
    if (n >= rem)
    {
        dst[dstsz - 1] = '\0';
        return;
    }
    off += n;
    rem -= n;

    for (int u = 0; u < salas[idx_sala].num_usuarios && rem > 1; u++)
    {
        n = snprintf(dst + off, rem, " - %s\n", salas[idx_sala].usuarios[u].nombre);
        if (n < 0)
            break;
        if (n >= rem)
        {
            dst[dstsz - 1] = '\0';
            break;
        }
        off += n;
        rem -= n;
    }
}

// Función para listar todos los usuarios de todos los canales
static void listar_todos_los_usuarios_en_texto(char *dst, size_t dstsz)
{
    if (dstsz == 0)
        return;
    size_t off = 0;
    int rem = (int)dstsz;

    int n = snprintf(dst + off, rem, "Usuarios por sala:\n");
    if (n < 0)
        return;
    if (n >= rem)
    {
        dst[dstsz - 1] = '\0';
        return;
    }
    off += n;
    rem -= n;

    for (int i = 0; i < num_salas && rem > 1; i++)
    {
        n = snprintf(dst + off, rem, "[%s] (%d)\n", salas[i].nombre, salas[i].num_usuarios);
        if (n < 0)
            break;
        if (n >= rem)
        {
            dst[dstsz - 1] = '\0';
            break;
        }
        off += n;
        rem -= n;

        for (int u = 0; u < salas[i].num_usuarios && rem > 1; u++)
        {
            n = snprintf(dst + off, rem, " - %s\n", salas[i].usuarios[u].nombre);
            if (n < 0)
                break;
            if (n >= rem)
            {
                dst[dstsz - 1] = '\0';
                break;
            }
            off += n;
            rem -= n;
        }
    }
}

// Busca el archivo en el que va a guardar los logs
static void ruta_log_sala(const char* nombre_sala, char* path, size_t sz) {
    snprintf(path, sz, LOG_DIR "/sala_%s.jsonl", nombre_sala);
}

// escape JSON básico para texto/control
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

// append de un mensaje a NDJSON (un JSON por línea)
static void loguear_mensaje(const char* sala, const char* remitente, const char* texto) {
    if (!sala || !*sala) return;
    char path[512]; ruta_log_sala(sala, path, sizeof(path));

    FILE* f = fopen(path, "a");
    if (!f) {
        // intentar crear dir y reintentar una vez
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

    // Registro NDJSON (una línea por mensaje)
    fprintf(f, "{\"ts\":%ld,\"sala\":\"%s\",\"from\":\"%s\",\"text\":\"%s\"}\n",
            (long)ts, esc_sala, esc_from, esc_text);

    fflush(f);
    flock(fd, LOCK_UN);
    fclose(f);
}

// Desescapa \" \\ \n \r \t (lo suficiente para tu uso en consola)
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
            else                out[j++] = c; // caída suave
        } else {
            out[j++] = in[i];
        }
    }
    out[j]='\0';
}

// Extrae "from" y "text" de una línea NDJSON como: {"ts":...,"sala":"...","from":"...","text":"..."}
static void extraer_from_y_text(const char* json,
                                char* from, size_t fromsz,
                                char* text, size_t textsz)
{
    const char *pf = strstr(json, "\"from\":\"");
    const char *pt = strstr(json, "\"text\":\"");
    if (!pf || !pt){ from[0]='\0'; text[0]='\0'; return; }

    pf += 8; // avanza tras "from":" 
    pt += 8; // avanza tras "text":" 

    // Copia hasta la comilla final no escapada
    char bufF[512]={0}, bufT[1024]={0};
    size_t i=0;
    for (; pf[i] && pf[i] != '\"'; ++i){
        if (pf[i]=='\\' && pf[i+1]) bufF[i++] = pf[i], bufF[i]=pf[i]; // conserva escape para unescape
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

// Lee las últimas N líneas del archivo de la sala y las envía al PID como CMD_HISTORY por la cola de la sala
static void enviar_historial_ultimos(const char* sala, int qid_sala, pid_t pid_dest, int N){
    if (!sala || !*sala || N <= 0) return;

    char path[512];
    ruta_log_sala(sala, path, sizeof(path));

    FILE* f = fopen(path, "r");
    if (!f) return; // no hay historial aún

    // Ring buffer de N punteros a línea
    char **ring = calloc(N, sizeof(char*));
    if (!ring){ fclose(f); return; }
    size_t ring_i = 0, count = 0;

    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) != -1){
        // guardamos copia
        free(ring[ring_i]);
        ring[ring_i] = strdup(line);
        ring_i = (ring_i + 1) % N;
        if (count < (size_t)N) count++;
    }
    free(line);
    fclose(f);

    // Reproducir en orden cronológico (del más antiguo al más nuevo)
    size_t start = (count == (size_t)N) ? ring_i : 0;
    for (size_t k = 0; k < count; ++k){
        const char* jsonl = ring[(start + k) % N];
        if (!jsonl) continue;

        char autor[MAX_NOMBRE] = {0};
        char texto[MAX_TEXTO]  = {0};
        extraer_from_y_text(jsonl, autor, sizeof autor, texto, sizeof texto);

        // arma mensaje dirigido al cliente (cola de la sala)
        struct mensaje out = {0};
        out.mtype = (long)pid_dest;   // SOLO para ese cliente
        out.cmd   = CMD_HISTORY;      // marcado como historial
        // remitente = autor original, texto = contenido
        strncpy(out.remitente, autor, sizeof(out.remitente)-1);
        strncpy(out.texto,     texto, sizeof(out.texto)-1);

        msgsnd(qid_sala, &out, MSGSIZE, 0);
    }

    // libera ring
    for (size_t k = 0; k < (size_t)N; ++k) free(ring[k]);
    free(ring);
}


int main()
{
    // Crear la cola global para solicitudes de clientes
    key_t key_global = ftok("/tmp", 'G');
    int cola_global = msgget(key_global, IPC_CREAT | 0666);
    if (cola_global == -1)
    {
        perror("Error al crear la cola global");
        exit(1);
    }

    printf("Servidor de chat iniciado. Esperando clientes...\n");
    // Cargar salas desde archivo al iniciar
    cargar_salas_desde_archivo();

    struct mensaje msg;

    while (1)
    {
        if (msgrcv(cola_global, &msg, MSGSIZE, 0, 0) == -1)
        {
            perror("Error al recibir mensaje");
            continue;
        }

        if (msg.mtype == MT_GLOBAL_JOIN && msg.cmd == CMD_JOIN)
        {
            // JOIN
            int indice_sala = buscar_sala(msg.sala);
            if (indice_sala == -1)
            {
                indice_sala = crear_sala(msg.sala);
                if (indice_sala == -1)
                {
                    printf("No se pudo crear la sala %s\n", msg.sala);
                    continue;
                }
                guardar_sala_si_nueva(msg.sala);
                printf("Nueva sala creada: %s\n", msg.sala);
            }

            if (agregar_usuario(indice_sala, msg.remitente, msg.pid) == 0)
            {
                printf("Usuario %s agregado a la sala %s\n", msg.remitente, msg.sala);

                // Respuesta dirigida: mtype = PID del cliente
                struct mensaje out = {0};
                out.mtype = (long)msg.pid;
                out.cmd = CMD_JOIN;
                out.sala_qid = salas[indice_sala].cola_id;
                snprintf(out.texto, MAX_TEXTO, "Te has unido a la sala: %s", msg.sala);
                enviar_historial_ultimos(msg.sala, salas[indice_sala].cola_id, msg.pid, 10);

                if (msgsnd(cola_global, &out, MSGSIZE, 0) == -1)
                {
                    perror("Error al enviar confirmación");
                }
            }
            else
            {
                printf("No se pudo agregar al usuario %s a la sala %s\n", msg.remitente, msg.sala);
            }
        }
        else if (msg.mtype == MT_GLOBAL_SEND && msg.cmd == CMD_SEND)
        {
            // Mensaje para la sala
            int indice_sala = buscar_sala(msg.sala);
            if (indice_sala != -1)
            {
                printf("Mensaje en la sala %s de %s: %s\n", msg.sala, msg.remitente, msg.texto);
                loguear_mensaje(msg.sala, msg.remitente, msg.texto);
                enviar_a_sala_menos_remitente(indice_sala, msg.remitente, msg.texto);
            }
        }
        else if (msg.mtype == MT_GLOBAL_SHOW && msg.cmd == CMD_SHOW)
        {
            struct mensaje out = {0};
            out.mtype = msg.pid;
            out.cmd = CMD_INFO;

            int activas = 0;
            for (int i = 0; i < num_salas; i++)
                if (salas[i].num_usuarios > 0)
                    activas++;

            if (activas == 0)
            {
                snprintf(out.texto, MAX_TEXTO, "No hay salas activas.");
            }
            else
            {
                printf("Enviando lista de salas activas a PID %d\n", msg.pid);
                size_t offset = 0;
                offset += snprintf(out.texto + offset, MAX_TEXTO - offset, "Salas activas:\n");
                for (int i = 0; i < num_salas && offset < MAX_TEXTO; i++)
                {
                    if (salas[i].num_usuarios <= 0)
                        continue;
                    offset += snprintf(out.texto + offset, MAX_TEXTO - offset,
                                       " - %s (%d usuarios)\n",
                                       salas[i].nombre, salas[i].num_usuarios);
                }
                out.texto[MAX_TEXTO - 1] = '\0';
            }

            if (msgsnd(cola_global, &out, MSGSIZE, 0) == -1)
            {
                perror("Error al enviar lista de salas");
            }
        }

        else if (msg.mtype == MT_GLOBAL_SHOW_ALL && msg.cmd == CMD_SHOW_ALL)
        {
            struct mensaje out = {0};
            out.mtype = msg.pid;
            out.cmd = CMD_INFO;

            // Mostramos TODAS las salas cargadas (tengan o no usuarios)
            if (num_salas == 0)
            {
                snprintf(out.texto, MAX_TEXTO, "No hay salas registradas.");
            }
            else
            {
                printf("Enviando lista de todas las salas a PID %d\n", msg.pid);
                size_t offset = 0;
                offset += snprintf(out.texto + offset, MAX_TEXTO - offset,
                                   "Salas registradas (todas):\n");
                for (int i = 0; i < num_salas && offset < MAX_TEXTO; i++)
                {
                    offset += snprintf(out.texto + offset, MAX_TEXTO - offset,
                                       " - %s (%d activos)\n",
                                       salas[i].nombre, salas[i].num_usuarios);
                }
                out.texto[MAX_TEXTO - 1] = '\0';
            }

            if (msgsnd(cola_global, &out, MSGSIZE, 0) == -1)
            {
                perror("Error al enviar lista de salas (all)");
            }
        }
        else if (msg.mtype == MT_GLOBAL_LEAVE && msg.cmd == CMD_LEAVE)
        {
            // LEAVE
            int indice_sala = buscar_sala(msg.sala);
            if (indice_sala != -1)
            {
                if (remover_usuario(indice_sala, msg.pid) == 0)
                {
                    printf("Usuario %s ha salido de la sala %s\n", msg.remitente, msg.sala);
                    // Enviar mensaje del sistema a los usuarios que quedan en la sala
                    char mensaje_sistema[MAX_TEXTO];
                    snprintf(mensaje_sistema, MAX_TEXTO, "%s salió de la sala", msg.remitente);
                    enviar_a_sala_menos_remitente(indice_sala, "", mensaje_sistema);

                    struct mensaje out = {0};
                    out.mtype = (long)msg.pid;
                    out.cmd = CMD_LEAVE;
                    snprintf(out.texto, MAX_TEXTO, "Has salido de la sala: %s", msg.sala);

                    if (msgsnd(cola_global, &out, MSGSIZE, 0) == -1)
                    {
                        perror("Error al enviar confirmación de LEAVE");
                    }
                }
                else
                {
                    printf("No se pudo remover al usuario %s de la sala %s\n", msg.remitente, msg.sala);

                    struct mensaje out = {0};
                    out.mtype = (long)msg.pid;
                    out.cmd = CMD_LEAVE;
                    snprintf(out.texto, MAX_TEXTO, "Error: No estabas en la sala %s", msg.sala);

                    if (msgsnd(cola_global, &out, MSGSIZE, 0) == -1)
                    {
                        perror("Error al enviar error de LEAVE");
                    }
                }
            }
            else
            {
                printf("Sala %s no encontrada para LEAVE\n", msg.sala);

                struct mensaje out = {0};
                out.mtype = (long)msg.pid;
                out.cmd = CMD_LEAVE;
                snprintf(out.texto, MAX_TEXTO, "Error: La sala %s no existe", msg.sala);

                if (msgsnd(cola_global, &out, MSGSIZE, 0) == -1)
                {
                    perror("Error al enviar error de sala no encontrada");
                }
            }
        }
        else if (msg.mtype == MT_GLOBAL_SHOW_USERS && msg.cmd == CMD_SHOW_USERS)
        {
            // Quien pide: msg.pid (y msg.remitente). Buscamos su sala.
            int idx = encontrar_sala_por_pid(msg.pid);

            struct mensaje out = {0};
            out.mtype = (long)msg.pid; // respuesta DIRECTA al cliente
            out.cmd = CMD_SHOW_USERS;

            if (idx == -1)
            {
                snprintf(out.texto, sizeof(out.texto),
                         "No estás en ninguna sala. Usa 'join <sala>'.");
            }
            else
            {
                listar_usuarios_de_sala_en_texto(idx, out.texto, sizeof(out.texto));
            }
            msgsnd(cola_global, &out, MSGSIZE, 0);
        }
        else if (msg.mtype == MT_GLOBAL_SHOW_ALL_USERS && msg.cmd == CMD_SHOW_ALL_USERS)
        {
            struct mensaje out = {0};
            out.mtype = (long)msg.pid; // respuesta al cliente
            out.cmd = CMD_SHOW_ALL_USERS;

            if (num_salas == 0)
            {
                snprintf(out.texto, sizeof(out.texto), "No hay salas creadas.");
            }
            else
            {
                listar_todos_los_usuarios_en_texto(out.texto, sizeof(out.texto));
            }
            msgsnd(cola_global, &out, MSGSIZE, 0);
        }
    }
    return 0;
}