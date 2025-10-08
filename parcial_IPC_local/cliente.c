// Incluye estandares de C y librerias de IPC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

// Definicion de constantes
#define MAX_TEXTO 256
#define MAX_NOMBRE 50

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
struct mensaje
{
    long mtype;
    int cmd;
    pid_t pid;
    int client_qid;
    int sala_qid;
    char remitente[MAX_NOMBRE];
    char texto[MAX_TEXTO];
    char sala[MAX_NOMBRE];
};

// Tamaño del mensaje sin incluir mtype
#define MSGSIZE (sizeof(struct mensaje) - sizeof(long))

// Variables globales
int cola_global;
int cola_sala = -1;
char nombre_usuario[MAX_NOMBRE];
char sala_actual[MAX_NOMBRE] = "";
char sala_pendiente[MAX_NOMBRE] = "";

// Variable de bloqueo para operaciones sincrónicas
volatile sig_atomic_t bloqueo_global = 0;

// manejar_notificacion_global: Maneja mensajes asíncronos recibidos en la cola global
// Parámetros:
//      - msg: Puntero al mensaje recibido
static void manejar_notificacion_global(const struct mensaje *msg)
{
    if (!msg)
        return;

    if (msg->cmd == CMD_INFO)
    {
        puts(msg->texto);
    }
    else if (msg->cmd == CMD_JOIN)
    {
        // promoción o join-ok tardío
        cola_sala = msg->sala_qid;
        if (cola_sala == -1)
        {
            fprintf(stderr, "[JOIN] qid de sala inválido\n");
        }
        else
        {
            if (msg->sala[0] != '\0')
            {
                strcpy(sala_actual, msg->sala);
            }
            else if (sala_pendiente[0] != '\0')
            {
                strcpy(sala_actual, sala_pendiente);
                sala_pendiente[0] = '\0';
            }
            printf("%s\n", msg->texto);
        }
    }
    else if (msg->cmd == CMD_LEAVE)
    {
        puts(msg->texto);
    }
}

// esperar_cmd_global: Espera un comando específico en la cola global
// Parámetros:
//      - cmd_esperado: Comando que se espera recibir
//      - out: Puntero a una estructura mensaje donde se almacenará el mensaje recibido
// Retorna:
//      - 0 si se recibió el comando esperado
//      - -1 en caso de error
static int esperar_cmd_global(int cmd_esperado, struct mensaje *out)
{
    long filtro = (long)getpid();
    struct mensaje tmp;

    for (;;)
    {
        if (msgrcv(cola_global, &tmp, MSGSIZE, filtro, 0) == -1)
        {
            perror("msgrcv (esperar_cmd_global)");
            return -1;
        }
        if (tmp.cmd == cmd_esperado)
        {
            if (out)
                *out = tmp;
            return 0;
        }
        // No era lo esperado entonces procesar como notificación asíncrona
        manejar_notificacion_global(&tmp);
        // y seguir esperando lo que sí esperamos
    }
}

// recibir_mensajes: Hilo que recibe mensajes tanto de la cola de la sala como de la cola global
void *recibir_mensajes(void *arg)
{
    struct mensaje msg;
    long filtro = (long)getpid();

    while (1)
    {
        int hubo_algo = 0;

        // Cola de la sala: siempre que esté en una sala
        if (cola_sala != -1)
        {
            while (msgrcv(cola_sala, &msg, MSGSIZE, filtro, IPC_NOWAIT) != -1)
            {
                hubo_algo = 1;
                if (msg.cmd == CMD_SEND && strcmp(msg.remitente, nombre_usuario) != 0)
                {
                    printf("%s: %s\n", msg.remitente, msg.texto);
                }
                else if (msg.cmd == CMD_HISTORY)
                {
                    printf("(hist) %s: %s\n", msg.remitente, msg.texto);
                }
            }
        }

        // Cola global: solo si no hay “bloqueo” por operación sincrónica
        if (!bloqueo_global)
        {
            while (msgrcv(cola_global, &msg, MSGSIZE, filtro, IPC_NOWAIT) != -1)
            {
                hubo_algo = 1;
                manejar_notificacion_global(&msg);
            }
        }

        if (!hubo_algo)
            usleep(100000);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("Uso: %s <nombre_usuario>\n", argv[0]);
        exit(1);
    }
    strcpy(nombre_usuario, argv[1]);

    key_t key_global = ftok("/tmp", 'G');
    cola_global = msgget(key_global, 0666);
    if (cola_global == -1)
    {
        perror("Error al conectar a la cola global");
        exit(1);
    }

    printf("Bienvenido, %s. Usa <show rooms> para ver las salas activas \n", nombre_usuario);
    printf("Usa <-help> para conocer los comandos y su uso \n");

    pthread_t hilo_receptor;
    pthread_create(&hilo_receptor, NULL, recibir_mensajes, NULL);

    struct mensaje msg;
    char comando[MAX_TEXTO];

    while (1)
    {
        printf("> ");
        if (!fgets(comando, MAX_TEXTO, stdin))
            break;
        comando[strcspn(comando, "\n")] = '\0';

        // JOIN
        if (strncmp(comando, "join ", 5) == 0)
        {
            char sala[MAX_NOMBRE];
            sscanf(comando, "join %s", sala);

            if (strlen(sala_actual) > 0 && cola_sala != -1)
            {
                printf("Ya estás en la sala '%s'. Usa 'leave %s' primero.\n", sala_actual, sala_actual);
                continue;
            }

            struct mensaje req = {0};
            req.mtype = MT_GLOBAL_JOIN;
            req.cmd = CMD_JOIN;
            req.pid = getpid();
            strcpy(req.remitente, nombre_usuario);
            strcpy(req.sala, sala);

            bloqueo_global = 1;
            if (msgsnd(cola_global, &req, MSGSIZE, 0) == -1)
            {
                bloqueo_global = 0;
                perror("Error al enviar solicitud de JOIN");
                continue;
            }

            struct mensaje resp = {0};
            if (esperar_cmd_global(CMD_JOIN, &resp) == -1)
            {
                bloqueo_global = 0;
                continue;
            }
            bloqueo_global = 0;

            // Puede ser JOIN OK inmediato o promoción que llegó al toque.
            // Si la promoción ya fue procesada por manejar_notificacion_global(),
            // resp contendrá ese JOIN. En ambos casos seteamos estado.
            printf("%s\n", resp.texto);
            cola_sala = resp.sala_qid;
            if (cola_sala == -1)
            {
                fprintf(stderr, "Error: qid de sala inválido\n");
                continue;
            }
            if (resp.sala[0] != '\0')
                strcpy(sala_actual, resp.sala);
            else
                strcpy(sala_actual, sala);
            sala_pendiente[0] = '\0';
            continue;
        }

        // Verifica que el comando sea "show rooms"
        // envia el mensaje correspondiente al comando a la cola global (especifica que es una acción del comando SHOW)
        else if (strcmp(comando, "show rooms") == 0)
        {
            struct mensaje req = {0};
            req.mtype = MT_GLOBAL_SHOW;
            req.cmd = CMD_SHOW;
            req.pid = getpid();

            bloqueo_global = 1;
            if (msgsnd(cola_global, &req, MSGSIZE, 0) == -1)
            {
                bloqueo_global = 0;
                perror("msgsnd show");
                continue;
            }
            struct mensaje resp = {0};
            if (esperar_cmd_global(CMD_INFO, &resp) == -1)
            {
                bloqueo_global = 0;
                continue;
            }
            bloqueo_global = 0;

            puts(resp.texto);
        }

        // Verifica que el comando sea "show all rooms"
        // envia el mensaje correspondiente al comando a la cola global (especifica que es una acción del comando SHOW_ALL)
        else if (strcmp(comando, "show all rooms") == 0)
        {
            struct mensaje req = {0};
            req.mtype = MT_GLOBAL_SHOW_ALL;
            req.cmd = CMD_SHOW_ALL;
            req.pid = getpid();

            bloqueo_global = 1;
            if (msgsnd(cola_global, &req, MSGSIZE, 0) == -1)
            {
                bloqueo_global = 0;
                perror("msgsnd show all");
                continue;
            }
            struct mensaje resp = {0};
            if (esperar_cmd_global(CMD_INFO, &resp) == -1)
            {
                bloqueo_global = 0;
                continue;
            }
            bloqueo_global = 0;

            puts(resp.texto);
        }

        // LEAVE
        else if (strncmp(comando, "leave ", 6) == 0)
        {
            char sala[MAX_NOMBRE];
            sscanf(comando, "leave %s", sala);
            if (strlen(sala) == 0)
            {
                printf("Uso: leave <nombre_sala>\n");
                continue;
            }

            struct mensaje req = {0};
            req.mtype = MT_GLOBAL_LEAVE;
            req.cmd = CMD_LEAVE;
            req.pid = getpid();
            strcpy(req.remitente, nombre_usuario);
            strcpy(req.sala, sala);

            bloqueo_global = 1;
            if (msgsnd(cola_global, &req, MSGSIZE, 0) == -1)
            {
                bloqueo_global = 0;
                perror("Error al enviar solicitud de LEAVE");
                continue;
            }
            struct mensaje resp = {0};
            if (esperar_cmd_global(CMD_LEAVE, &resp) == -1)
            {
                bloqueo_global = 0;
                continue;
            }
            bloqueo_global = 0;

            printf("%s\n", resp.texto);
            if (strcmp(sala, sala_actual) == 0)
            {
                cola_sala = -1;
                sala_actual[0] = '\0';
            }
        }

        // HELP
        else if (strcmp(comando, "-help") == 0)
        {
            printf(" - join <sala>: Entrar a una sala (si está llena, quedas en espera)\n"
                   " - leave <sala>: Salir de una sala\n"
                   " - show rooms: Muestra las salas activas\n"
                   " - show all rooms: Muestra todas las salas registradas\n"
                   " - show users: Muestra los usuarios de tu sala actual\n"
                   " - show all users: Muestra los usuarios de todas las salas\n");
        }

        // SHOW USERS
        else if (strcmp(comando, "show users") == 0)
        {
            if (cola_sala == -1 || sala_actual[0] == '\0')
            {
                printf("No estás en ninguna sala. Usa 'join <sala>'.\n");
                continue;
            }

            struct mensaje req = {0};
            req.mtype = MT_GLOBAL_SHOW_USERS;
            req.cmd = CMD_SHOW_USERS;
            req.pid = getpid();
            strcpy(req.remitente, nombre_usuario);

            bloqueo_global = 1;
            if (msgsnd(cola_global, &req, MSGSIZE, 0) == -1)
            {
                bloqueo_global = 0;
                perror("Error al pedir lista de usuarios");
                continue;
            }
            struct mensaje resp = {0};
            if (esperar_cmd_global(CMD_SHOW_USERS, &resp) == -1)
            {
                bloqueo_global = 0;
                continue;
            }
            bloqueo_global = 0;

            printf("%s\n", resp.texto);
            continue;
        }

        // SHOW ALL USERS
        else if (strcmp(comando, "show all users") == 0)
        {
            struct mensaje req = {0};
            req.mtype = MT_GLOBAL_SHOW_ALL_USERS;
            req.cmd = CMD_SHOW_ALL_USERS;
            req.pid = getpid();
            strcpy(req.remitente, nombre_usuario);

            bloqueo_global = 1;
            if (msgsnd(cola_global, &req, MSGSIZE, 0) == -1)
            {
                bloqueo_global = 0;
                perror("Error al pedir lista global");
                continue;
            }
            struct mensaje resp = {0};
            if (esperar_cmd_global(CMD_SHOW_ALL_USERS, &resp) == -1)
            {
                bloqueo_global = 0;
                continue;
            }
            bloqueo_global = 0;

            printf("%s\n", resp.texto);
            continue;
        }

        // MENSAJE AL CANAL
        else if (strlen(comando) > 0)
        {
            if (sala_actual[0] == '\0' || cola_sala == -1)
            {
                printf("No estás en ninguna sala. Usa 'join <sala>' para unirte a una.\n");
                continue;
            }

            struct mensaje out = {0};
            out.mtype = MT_GLOBAL_SEND;
            out.cmd = CMD_SEND;
            out.pid = getpid();
            strcpy(out.remitente, nombre_usuario);
            strcpy(out.sala, sala_actual);
            strcpy(out.texto, comando);

            if (msgsnd(cola_global, &out, MSGSIZE, 0) == -1)
            {
                perror("Error al enviar mensaje");
                continue;
            }
        }
    }

    return 0;
}
