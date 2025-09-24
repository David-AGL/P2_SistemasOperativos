# Sistema de Chat con Colas de Mensajes

Este proyecto implementa un chat simple usando **colas de mensajes System V** en C, con un **servidor central** y múltiples **clientes**.  

---

## 📦 Requisitos

- Linux (probado en distribuciones tipo Ubuntu/Debian).
- Compilador `gcc`.
- Biblioteca de hilos (`pthread`, ya incluida en Linux).

---

## 🔧 Compilación

En la raíz del proyecto, compila ambos programas:

```bash
gcc -o servidor servidor.c -lpthread
gcc -o cliente cliente.c -lpthread

## ▶️ Ejecución

1. Inicia el servidor en una terminal:

./servidor

2. Abre otra(s) terminal(es) y ejecuta clientes con un nombre:

./cliente Maria
./cliente Juan
./cliente Camila

3. Comandos disponibles:

Unirse a una sala:

> join General


Enviar un mensaje (una vez unido):

> Hola a todos!