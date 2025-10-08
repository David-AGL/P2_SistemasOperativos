# Sistema de Chat de Mensajes (IPC y TCP)

Este proyecto contiene dos variantes del mismo chat: una usando System V IPC (colas de mensajes) y otra usando sockets TCP para comunicación en red. Las funcionalidades y comandos son los mismos en ambas versiones.

## 📦 Requisitos
- Linux
- gcc
- Biblioteca de hilos (`pthread`, ya incluida en Linux). 


## 🔧 Compilación

### Versión IPC (colas System V)
Compilar servidor y cliente IPC:
```bash
gcc -o servidor servidor.c -lpthread
gcc -o cliente cliente.c -lpthread
```

### Versión TCP (sockets, puerto de ejemplo 5555)
Compilar servidor y cliente TCP:
```bash
gcc -o servidor_tcp servidor_tcp.c
gcc -o cliente_tcp cliente_tcp.c
```

## ▶️ Ejecución

### IPC
1. Inicia el servidor IPC:
```bash
./servidor
```
2. En otras terminales inicia clientes (cada cliente con su nombre):
```bash
./cliente Maria
./cliente Juan
```

### TCP
1. Inicia el servidor TCP en el puerto 5555 (ejemplo):
```bash
./servidor_tcp 5555
```
2. En otras terminales o en otras máquinas de la misma red, conecta clientes:
```bash
./cliente_tcp 127.0.0.1 5555 Maria
./cliente_tcp <IP_DEL_SERVIDOR> 5555 Juan
```

## 🤖 Comandos disponibles (idénticos en IPC y TCP)

- **Ver la lista de comandos disponibles:**

  > -help

- **Unirse a una sala (si está llena, quedas en espera):**

  > join <sala>

- **Salir de una sala:**

  > leave <sala>

- **Mostrar las salas activas (con usuarios conectados):**

  > show rooms

- **Mostrar todas las salas registradas (incluso las vacías):**

  > show all rooms

- **Mostrar los usuarios de tu sala actual:**

  > show users

- **Mostrar los usuarios de todas las salas:**

  > show all users


## Notas
- La persistencia de mensajes se guarda en `logs/` como JSONL por sala.
