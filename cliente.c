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

// Estructura para los mensajes
struct mensaje {
    long mtype;         // Tipo de mensaje
    char remitente[MAX_NOMBRE];
    char texto[MAX_TEXTO];
    char sala[MAX_NOMBRE];
};

int cola_global;
int cola_sala = -1;
char nombre_usuario[MAX_NOMBRE];
char sala_actual[MAX_NOMBRE] = "";

// Función para el hilo que recibe mensajes
void *recibir_mensajes(void *arg) {
    struct mensaje msg;

    while (1) {
        if (cola_sala != -1) {
            // Recibir mensajes de la cola de la sala
            if (msgrcv(cola_sala, &msg, sizeof(struct mensaje) - sizeof(long), 0, 0) == -1) {
                perror("Error al recibir mensaje de la sala");
                continue;
            }

            // Mostrar el mensaje si no es del propio usuario
            if (strcmp(msg.remitente, nombre_usuario) != 0) {
                printf("%s: %s\n", msg.remitente, msg.texto);
            }
        }
        usleep(100000); // Pequeña pausa para no consumir demasiado CPU
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
    key_t key_global = ftok("/tmp", 'A');
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
            // Comando para unirse a una sala
            char sala[MAX_NOMBRE];
            sscanf(comando, "join %s", sala);

            // Enviar solicitud de JOIN al servidor
            msg.mtype = 1; // JOIN
            strcpy(msg.remitente, nombre_usuario);
            strcpy(msg.sala, sala);
            strcpy(msg.texto, "");

            if (msgsnd(cola_global, &msg, sizeof(struct mensaje) - sizeof(long), 0) == -1) {
                perror("Error al enviar solicitud de JOIN");
                continue;
            }

            // Esperar confirmación del servidor
            if (msgrcv(cola_global, &msg, sizeof(struct mensaje) - sizeof(long), 2, 0) == -1) {
                perror("Error al recibir confirmación");
                continue;
            }

            printf("%s\n", msg.texto);

            // Obtener la cola de la sala
            key_t key_sala = ftok("/tmp", atoi(sala)); // Esto es un ejemplo, debe ser mejorado
            cola_sala = msgget(key_sala, 0666);
            if (cola_sala == -1) {
                perror("Error al conectar a la cola de la sala");
                continue;
            }

            strcpy(sala_actual, sala);
        } else if (strlen(comando) > 0) {
            // Enviar un mensaje a la sala actual
            if (strlen(sala_actual) == 0) {
                printf("No estás en ninguna sala. Usa 'join <sala>' para unirte a una.\n");
                continue;
            }

            msg.mtype = 3; // MSG
            strcpy(msg.remitente, nombre_usuario);
            strcpy(msg.sala, sala_actual);
            strcpy(msg.texto, comando);

            if (msgsnd(cola_sala, &msg, sizeof(struct mensaje) - sizeof(long), 0) == -1) {
                perror("Error al enviar mensaje");
                continue;
            }
        }
    }

    return 0;
}