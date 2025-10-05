#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <unistd.h>
#include <pthread.h>

#define MAX_TEXTO 256
#define MAX_NOMBRE 50

// Constantes de control para la COLA GLOBAL
#define MT_GLOBAL_JOIN 1L
#define MT_GLOBAL_SEND 3L
#define MT_GLOBAL_SHOW 5L
#define MT_GLOBAL_SHOW_ALL 7L
#define MT_GLOBAL_LEAVE 9L

// Comandos semánticos dentro del payload
#define CMD_JOIN 1
#define CMD_SEND 2
#define CMD_SHOW 3
#define CMD_INFO 4
#define CMD_SHOW_ALL 8
#define CMD_LEAVE 9

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

int cola_global;
int cola_sala = -1;
char nombre_usuario[MAX_NOMBRE];
char sala_actual[MAX_NOMBRE] = "";

// Función para el hilo que recibe mensajes
void *recibir_mensajes(void *arg)
{
    struct mensaje msg;
    while (1)
    {
        if (cola_sala != -1)
        {
            long filtro = (long)getpid();
            if (msgrcv(cola_sala, &msg, MSGSIZE, filtro, IPC_NOWAIT) == -1)
            {
                usleep(100000);
                continue;
            }
            if (msg.cmd == CMD_SEND && strcmp(msg.remitente, nombre_usuario) != 0)
            {
                printf("%s: %s\n", msg.remitente, msg.texto);
            }
        }
        else
        {
            usleep(200000); // pausa mas cuando no hay sala activ
        }
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

    // Conectarse a la cola global
    key_t key_global = ftok("/tmp", 'G');
    cola_global = msgget(key_global, 0666);
    if (cola_global == -1)
    {
        perror("Error al conectar a la cola global");
        exit(1);
    }

    printf("Bienvenido, %s. Salas disponibles: General, Deportes\n", nombre_usuario);

    // Crear un hilo para recibir mensajes
    pthread_t hilo_receptor;
    pthread_create(&hilo_receptor, NULL, recibir_mensajes, NULL);

    struct mensaje msg;
    char comando[MAX_TEXTO];

    while (1)
    {
        printf("> ");
        fgets(comando, MAX_TEXTO, stdin);
        comando[strcspn(comando, "\n")] = '\0'; // Eliminar el salto de línea

        if (strncmp(comando, "join ", 5) == 0)
        {
            char sala[MAX_NOMBRE];
            sscanf(comando, "join %s", sala);
            if (strlen(sala_actual) > 0 && cola_sala != -1)            // Warning para evitar join  de otra sala estando en una
            {
                printf("Ya estás en la sala '%s'. Usa 'leave %s' primero antes de unirte a otra sala.\n", 
                       sala_actual, sala_actual);
                continue;
            }

            struct mensaje msg = {0};
            msg.mtype = MT_GLOBAL_JOIN; // 1
            msg.cmd = CMD_JOIN;
            msg.pid = getpid();
            strcpy(msg.remitente, nombre_usuario);
            strcpy(msg.sala, sala);

            if (msgsnd(cola_global, &msg, MSGSIZE, 0) == -1)
            {
                perror("Error al enviar solicitud de JOIN");
                continue;
            }

            // Esperar confirmación dirigida: mtype = mi PID
            if (msgrcv(cola_global, &msg, MSGSIZE, (long)getpid(), 0) == -1)
            {
                perror("Error al recibir confirmación");
                continue;
            }

            printf("%s\n", msg.texto);

            // El servidor nos da el qid de la sala
            cola_sala = msg.sala_qid;
            if (cola_sala == -1)
            {
                fprintf(stderr, "Error: qid de sala inválido\n");
                continue;
            }
            strcpy(sala_actual, sala);
        }
        else if (strcmp(comando, "show") == 0)
        {
            // Enviar petición SHOW por la cola global
            struct mensaje req = {0};
            req.mtype = MT_GLOBAL_SHOW; // mtype que usas para 'show'
            req.cmd = CMD_SHOW;         // comando 'show'
            req.pid = getpid();         // para que el servidor responda a tu PID

            if (msgsnd(cola_global, &req, MSGSIZE, 0) == -1)
            {
                perror("msgsnd show");
                continue;
            }

            // Esperar la respuesta informativa dirigida a mí (mtype = mi PID)
            struct mensaje resp = {0};
            if (msgrcv(cola_global, &resp, MSGSIZE, (long)getpid(), 0) == -1)
            {
                perror("msgrcv show");
                continue;
            }

            // El servidor debe responder con CMD_INFO y el listado en 'texto'
            if (resp.cmd == CMD_INFO)
            {
                puts(resp.texto);
            }
            else
            {
                // Por si el servidor respondiera otra cosa
                printf("[show] Respuesta inesperada (cmd=%d)\n", resp.cmd);
            }
        }
        else if (strcmp(comando, "show all") == 0)
        {
            struct mensaje req = {0};
            req.mtype = MT_GLOBAL_SHOW_ALL; 
            req.cmd = CMD_SHOW_ALL;        
            req.pid = getpid();

            if (msgsnd(cola_global, &req, MSGSIZE, 0) == -1)
            {
                perror("msgsnd show all");
                continue;
            }

            struct mensaje resp = {0};
            if (msgrcv(cola_global, &resp, MSGSIZE, (long)getpid(), 0) == -1)
            {
                perror("msgrcv show all");
                continue;
            }

            if (resp.cmd == CMD_INFO)
            {
                puts(resp.texto);
            }
            else
            {
                printf("[show all] Respuesta inesperada (cmd=%d)\n", resp.cmd);
            }
        }
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

            if (msgsnd(cola_global, &req, MSGSIZE, 0) == -1){
                perror("Error al enviar solicitud de LEAVE");
                continue;
            }
            struct mensaje resp = {0};
            if (msgrcv(cola_global, &resp, MSGSIZE, (long)getpid(), 0) == -1)
            {
                perror("Error al recibir confirmación de LEAVE");
                continue;
            }

            printf("%s\n", resp.texto);
            if (strcmp(sala, sala_actual) == 0) {
                cola_sala = -1;
                strcpy(sala_actual, "");
            }
        }
        else if (strlen(comando) > 0)
        {
            if (strlen(sala_actual) == 0 || cola_sala == -1)
            {
                printf("No estás en ninguna sala. Usa 'join <sala>' para unirte a una.\n");
                continue;
            }

            struct mensaje msg = {0};
            msg.mtype = MT_GLOBAL_SEND; 
            msg.cmd = CMD_SEND;
            msg.pid = getpid();
            strcpy(msg.remitente, nombre_usuario);
            strcpy(msg.sala, sala_actual);
            strcpy(msg.texto, comando);

            if (msgsnd(cola_global, &msg, MSGSIZE, 0) == -1){
                perror("Error al enviar mensaje");
                continue;
            }
        }
    }

    return 0;
}

