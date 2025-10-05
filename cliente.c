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

// Constantes de control para la COLA GLOBAL...
#define MT_GLOBAL_JOIN 1L
#define MT_GLOBAL_SEND 3L 

// Comandos semánticos dentro del payload
#define CMD_JOIN 1
#define CMD_SEND 2
#define CMD_LEAVE 3   //incluir comando 


// Estructura para los mensajes
struct mensaje {
    long mtype;                 // Tipo de mensaje
    int   cmd;                  // CMD_JOIN / CMD_SEND
    pid_t pid;                  // PID del cliente
    int   client_qid;           // (reservado si luego usas cola privada)
    int   sala_qid;             // qid de la sala (respuesta JOIN)
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
void *recibir_mensajes(void *arg) {
    struct mensaje msg;
    while (1) {
        if (cola_sala != -1) {
            // Leer SOLO mtype = mi PID
            long filtro = (long)getpid();
            if (msgrcv(cola_sala, &msg, MSGSIZE, filtro, 0) == -1) {
                perror("Error al recibir mensaje de la sala");
                usleep(100000);
                continue;
            }
            if (msg.cmd == CMD_SEND && strcmp(msg.remitente, nombre_usuario) != 0) {
                if (strcmp(msg.remitente, "SISTEMA") == 0) {
                    printf("%s\n", msg.texto);  // mensaje para Sistema cuando usuario sale del chat
                } else {
                    printf("%s: %s\n", msg.remitente, msg.texto);  // Formato normal
                }
            }
        } else {
            usleep(100000); // pequeña pausa
        }
    }
    return NULL;
}


int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Uso: %s <nombre_usuario>\n", argv[0]);
        exit(1);
    }

    strcpy(nombre_usuario, argv[1]);

    // Conectarse a la cola global
    key_t key_global = ftok("/tmp", 'G');
    cola_global = msgget(key_global, 0666);
    if (cola_global == -1) {
        perror("Error al conectar a la cola global");
        exit(1);
    }

    printf("Bienvenido, %s. Salas disponibles: General, Deportes\n", nombre_usuario);

    // Crear un hilo para recibir mensajes
    pthread_t hilo_receptor;
    pthread_create(&hilo_receptor, NULL, recibir_mensajes, NULL);

    struct mensaje msg;
    char comando[MAX_TEXTO];

    while (1) {
        printf("> ");
        fgets(comando, MAX_TEXTO, stdin);
        comando[strcspn(comando, "\n")] = '\0'; // Eliminar el salto de línea

        if (strncmp(comando, "join ", 5) == 0) {
    char sala[MAX_NOMBRE];
    sscanf(comando, "join %s", sala);

    struct mensaje msg = {0};
    msg.mtype = MT_GLOBAL_JOIN;     // 1
    msg.cmd   = CMD_JOIN;
    msg.pid   = getpid();
    strcpy(msg.remitente, nombre_usuario);
    strcpy(msg.sala, sala);

    if (msgsnd(cola_global, &msg, MSGSIZE, 0) == -1) {
        perror("Error al enviar solicitud de JOIN");
        continue;
    }

    // Esperar confirmación dirigida: mtype = mi PID
    if (msgrcv(cola_global, &msg, MSGSIZE, (long)getpid(), 0) == -1) {
        perror("Error al recibir confirmación");
        continue;
    }

    printf("%s\n", msg.texto);

    // El servidor nos da el qid de la sala
    cola_sala = msg.sala_qid;
    if (cola_sala == -1) {
        fprintf(stderr, "Error: qid de sala inválido\n");
        continue;
    }
    strcpy(sala_actual, sala);
} else if (strncmp(comando, "leave ", 6) == 0) {    //Logica para salir del chat
            char sala_salir[MAX_NOMBRE];
            sscanf(comando, "leave %s", sala_salir);
            
            if (strlen(sala_actual) == 0) {
                printf("No estás en ninguna sala.\n");
                continue;
            }
            
            if (strcmp(sala_salir, sala_actual) != 0) {
                printf("No estás en la sala %s. Estás en: %s\n", sala_salir, sala_actual);
                continue;
            }

            struct mensaje msg = {0};
            msg.mtype = MT_GLOBAL_JOIN;
            msg.cmd   = CMD_LEAVE;
            msg.pid   = getpid();
            strcpy(msg.remitente, nombre_usuario);
            strcpy(msg.sala, sala_actual);
            msgsnd(cola_global, &msg, MSGSIZE, 0);

            cola_sala = -1;
            sala_actual[0] = '\0';
            printf("Has salido de la sala %s.\n", sala_salir);  // Confirmación al usuario
} else if (strlen(comando) > 0) {
    if (strlen(sala_actual) == 0 || cola_sala == -1) {
        printf("No estás en ninguna sala. Usa 'join <sala>' para unirte a una.\n");
        continue;
    }

    struct mensaje msg = {0};
    msg.mtype = MT_GLOBAL_SEND;     // 3 (tu “MSG”)
    msg.cmd   = CMD_SEND;
    msg.pid   = getpid();
    strcpy(msg.remitente, nombre_usuario);
    strcpy(msg.sala, sala_actual);
    strcpy(msg.texto, comando);

    if (msgsnd(cola_global, &msg, MSGSIZE, 0) == -1) {
        perror("Error al enviar mensaje");
        continue;
            }
        }
    }

    return 0;

    

}