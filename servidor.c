#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <unistd.h>

#define MAX_SALAS 10
#define MAX_USUARIOS_POR_SALA 20
#define MAX_TEXTO 256
#define MAX_NOMBRE 50

// Constantes de control para la COLA GLOBAL
#define MT_GLOBAL_JOIN 1L
#define MT_GLOBAL_SEND 3L 

// Comandos semánticos dentro del payload
#define CMD_JOIN 1
#define CMD_SEND 2

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

struct usuario {
    pid_t pid;
    char  nombre[MAX_NOMBRE];
};

struct sala {
    char nombre[MAX_NOMBRE];
    int  cola_id;        // ID de la cola de la sala
    int  num_usuarios;
    struct usuario usuarios[MAX_USUARIOS_POR_SALA];
};


struct sala salas[MAX_SALAS];
int num_salas = 0;

// Función para crear una nueva sala
int crear_sala(const char *nombre) {
    if (num_salas >= MAX_SALAS) {
        return -1; // No se pueden crear más salas
    }

    // Crear una cola de mensajes para la sala
    key_t key = ftok("/tmp", (int)('A' + num_salas)); // Generar una clave única
    int cola_id = msgget(key, IPC_CREAT | 0666);
    if (cola_id == -1) {
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
int buscar_sala(const char *nombre) {
    for (int i = 0; i < num_salas; i++) {
        if (strcmp(salas[i].nombre, nombre) == 0) {
            return i;
        }
    }
    return -1; // No encontrada
}

// Función para agregar un usuario a una sala
int agregar_usuario(int indice_sala, const char *nombre_usuario, pid_t pid) {
    if (indice_sala < 0 || indice_sala >= num_salas) return -1;
    struct sala *s = &salas[indice_sala];
    if (s->num_usuarios >= MAX_USUARIOS_POR_SALA) return -1;

    // Evitar duplicados por nombre o PID
    for (int i = 0; i < s->num_usuarios; i++) {
        if (s->usuarios[i].pid == pid)
            return 0;
    }

    s->usuarios[s->num_usuarios].pid = pid;
    strncpy(s->usuarios[s->num_usuarios].nombre, nombre_usuario, MAX_NOMBRE);
    s->num_usuarios++;
    return 0;
}

void enviar_a_sala_menos_remitente(int indice_sala, const char *remitente, const char *texto) {
    if (indice_sala < 0 || indice_sala >= num_salas) return;
    struct sala *s = &salas[indice_sala];

    for (int i = 0; i < s->num_usuarios; i++) {
        if (strcmp(s->usuarios[i].nombre, remitente) == 0) continue; // no al remitente

        struct mensaje out = {0};
        out.mtype = (long)s->usuarios[i].pid;   // clave: mtype = PID del receptor
        out.cmd   = CMD_SEND;
        strncpy(out.remitente, remitente, MAX_NOMBRE);
        strncpy(out.texto,     texto,     MAX_TEXTO);

        if (msgsnd(s->cola_id, &out, MSGSIZE, 0) == -1) {
            perror("Error al reenviar a la sala");
        }
    }
}

int main() {
    // Crear la cola global para solicitudes de clientes
    key_t key_global = ftok("/tmp", 'G');
    int cola_global = msgget(key_global, IPC_CREAT | 0666);
    if (cola_global == -1) {
        perror("Error al crear la cola global");
        exit(1);
    }

    printf("Servidor de chat iniciado. Esperando clientes...\n");

struct mensaje msg;

while (1) {
    if (msgrcv(cola_global, &msg, MSGSIZE, 0, 0) == -1) {
        perror("Error al recibir mensaje");
        continue;
    }

    if (msg.mtype == MT_GLOBAL_JOIN && msg.cmd == CMD_JOIN) {
        // JOIN
        int indice_sala = buscar_sala(msg.sala);
        if (indice_sala == -1) {
            indice_sala = crear_sala(msg.sala);
            if (indice_sala == -1) {
                printf("No se pudo crear la sala %s\n", msg.sala);
                continue;
            }
            printf("Nueva sala creada: %s\n", msg.sala);
        }

        if (agregar_usuario(indice_sala, msg.remitente, msg.pid) == 0) {
            printf("Usuario %s agregado a la sala %s\n", msg.remitente, msg.sala);

            // Respuesta dirigida: mtype = PID del cliente
            struct mensaje out = {0};
            out.mtype   = (long)msg.pid;
            out.cmd     = CMD_JOIN;
            out.sala_qid= salas[indice_sala].cola_id;
            snprintf(out.texto, MAX_TEXTO, "Te has unido a la sala: %s", msg.sala);

            if (msgsnd(cola_global, &out, MSGSIZE, 0) == -1) {
                perror("Error al enviar confirmación");
            }
        } else {
            printf("No se pudo agregar al usuario %s a la sala %s\n", msg.remitente, msg.sala);
        }

    } else if (msg.mtype == MT_GLOBAL_SEND && msg.cmd == CMD_SEND) {
        // Mensaje para la sala
        int indice_sala = buscar_sala(msg.sala);
        if (indice_sala != -1) {
            printf("Mensaje en la sala %s de %s: %s\n", msg.sala, msg.remitente, msg.texto);
            enviar_a_sala_menos_remitente(indice_sala, msg.remitente, msg.texto);
        }
    }
}

    return 0;
}