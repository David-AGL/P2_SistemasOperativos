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
```

---

## ▶️ Ejecución

1. Inicia el servidor en una terminal:

```bash
./servidor
```

2. Abre otra(s) terminal(es) y ejecuta clientes con un nombre:

```bash
./cliente Maria
./cliente Juan
./cliente Camila
```

3. Comandos disponibles:

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
