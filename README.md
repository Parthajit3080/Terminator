# Terminator – Custom Linux Terminal Emulator

A feature-rich terminal emulator and custom shell environment developed in **C using X11, POSIX system calls, Pthreads, and Inter-Process Communication (IPC)**.

Terminator combines a graphical terminal interface with a custom command interpreter, supporting multiple tabs, process management, command execution, synchronization, persistent history, autocomplete, and shell scripting.

---

## 🚀 Features

### 1. Graphical User Interface (GUI)
- Built using the X11 library.
- Multiple terminal tabs with independent command sessions.
- Create, switch between, and close tabs.
- Scrollable terminal output.
- Text selection, copy, and paste support.
- Customizable keyboard shortcuts.

### 2. Shell Command Execution
- Supports built-in shell commands.
- Executes external Linux commands using process creation and execution system calls.
- Supports command arguments and environment variables.
- Handles quoted strings and escaped characters.
- Supports command separators and conditional operators.

### 3. Input and Output Redirection
Supports standard shell redirection operators:

| Operator | Description |
|---|---|
| `<` | Input redirection |
| `>` | Output redirection |
| `>>` | Append output |
| `2>` | Redirect standard error |

### 4. Pipeline Execution
- Supports connecting multiple commands using pipes (`|`).
- Passes the output of one process as input to another.
- Uses process creation and file descriptor management.
- Supports background pipeline execution.

Example:

```bash
ls -l | grep ".c" | wc -l
```

### 5. Multithreading and Synchronization
- Uses POSIX Pthreads for thread-based terminal sessions.
- Uses mutex-based synchronization.
- Implements semaphores for coordinating shared resources.
- Supports concurrent command execution across terminal tabs.

### 6. Inter-Process Communication (IPC)
Supports process-based terminal sessions and shared data management using:

- `fork()` – Process creation.
- `mmap()` – Shared memory allocation.
- `sem_init()` – Semaphore initialization.
- `sem_wait()` and `sem_post()` – Synchronization.
- Pipes – Communication between GUI and shell workers.

Two execution modes are available:

| Mode | Implementation |
|---|---|
| Thread Mode | Pthreads |
| Process Mode | Fork-based processes |

### 7. MultiWatch – Concurrent Command Monitoring

MultiWatch executes multiple commands concurrently and displays their outputs separately.

Example:

```bash
multiWatch["date", "whoami", "uptime", "ls"]
```

Features:
- Concurrent execution of multiple commands.
- Separate output sections for each command.
- Automatic periodic restarting of completed commands.
- Process monitoring and cancellation.
- Ctrl+C cancellation support.

### 8. Process Management and Job Control

Supports foreground and background process management.

| Command | Description |
|---|---|
| `command &` | Execute a command in the background |
| `jobs` | Display available background jobs |
| `fg %1` | Bring a job to the foreground |
| `bg %1` | Resume a stopped job in the background |

Also includes process-group management and signal handling.

### 9. Command History and Search
- Persistent command history.
- Stores up to 10,000 commands in memory.
- Loads and saves history across sessions.
- Supports history navigation.
- Implements a **Suffix Automaton** for efficient longest-substring matching against previous commands.

### 10. Intelligent Autocomplete
- Filename and directory completion.
- Prefix-based matching.
- Common-prefix completion.
- Multiple completion suggestions.
- Number-based selection of completion candidates.

### 11. Shell Scripting

Includes a custom script interpreter supporting:

- Shell variables.
- Environment variable expansion.
- Script arguments (`$0`, `$1`, etc.).
- Exit status (`$?`).
- Argument count (`$#`).
- Conditional statements (`if`, `elif`, `else`, `fi`).
- Script execution and nested sourcing.
- Basic Bash-style condition evaluation.

**Note:** Terminator implements a subset of Bash scripting functionality and is not intended to be a complete replacement for Bash.

---

## 🛠️ Technologies Used

| Technology | Purpose |
|---|---|
| C | Core implementation |
| X11 (Xlib) | Graphical terminal interface |
| POSIX Threads | Multithreading |
| Semaphores | Synchronization |
| Shared Memory (`mmap`) | Inter-process data sharing |
| Pipes | IPC and command pipelines |
| Process Management | `fork()`, `exec()`, `waitpid()` |
| Signals | Process and job control |
| GCC | Compilation |

---

## 📋 Requirements

The following are required to compile and run Terminator:

- Linux operating system or WSL with an X11-compatible display server.
- GCC compiler.
- X11 development libraries.
- POSIX Threads support.
- POSIX semaphores.

### Install dependencies on Ubuntu / WSL

```bash
sudo apt update
sudo apt install build-essential libx11-dev
```

For WSL, ensure that an X11-compatible display server is available and that the `DISPLAY` environment variable is configured.

---

## ⚙️ Compilation

Clone the repository:

```bash
git clone https://github.com/YOUR_USERNAME/Terminator.git
```

Navigate to the project directory:

```bash
cd Terminator
```

Compile the source code:

```bash
gcc -Wall -Wextra -O2 terminator.c -o terminator -lX11 -pthread
```

---

## ▶️ Running Terminator

### Thread Mode (Default)

```bash
./terminator
```

In this mode, terminal sessions use POSIX threads.

### Process Mode

```bash
./terminator useProcess=True
```

In this mode, each terminal session runs as a separate process.

---

## 🧪 Example Commands

Try the following commands after launching Terminator:

### Basic Commands

```bash
pwd
ls -la
whoami
date
uname -a
```

### Pipeline

```bash
ls -l | grep ".c" | wc -l
```

### Redirection

```bash
echo "Hello Terminator" > output.txt
cat < output.txt
echo "Second line" >> output.txt
```

### Background Execution

```bash
sleep 30 &
jobs
```

### MultiWatch

```bash
multiWatch["date", "whoami", "uptime", "ls"]
```

### Shell Variables

```bash
name="Terminator"
echo $name
```

### Conditional Scripting

Create a file named `test.sh`:

```bash
#!/bin/bash

x=10

if [ "$x" -eq 10 ]; then
    echo "Condition satisfied"
else
    echo "Condition failed"
fi
```

Execute it inside Terminator:

```bash
./test.sh
```

---

## 📁 Project Structure

```text
Terminator/
│
├── terminator.c       # Main source code
├── README.md          # Project documentation
└── .terminatorrc      # Optional keyboard shortcut configuration
```

---

## 🎯 Project Objectives

The primary objectives of this project are:

1. To understand Linux system programming and process management.
2. To implement multithreading and synchronization using POSIX APIs.
3. To explore Inter-Process Communication through shared memory, pipes, and semaphores.
4. To develop a custom command interpreter and graphical terminal interface.
5. To implement process management, job control, and signal handling.
6. To explore efficient string-search algorithms through command-history search.
7. To integrate operating-system concepts into a practical application.

---

## 📚 Concepts Demonstrated

- Operating Systems
- Process Creation and Management
- Multithreading
- Thread Synchronization
- Inter-Process Communication (IPC)
- Shared Memory
- Semaphores
- Pipes and Redirection
- Signals and Job Control
- File Handling
- Command Parsing
- String Matching Algorithms
- Graphical Programming with X11

---

## ⚠️ Limitations

- Terminator is a custom shell implementation, not a complete Bash replacement.
- Some advanced Bash features and shell constructs are not supported.
- Runtime behavior depends on the Linux environment and X11 configuration.
- Complex pipelines, concurrent operations, and process-management edge cases may have limitations.

---

## 👨‍💻 Author

**Parthajit**

Computer Science and Engineering Undergraduate  
Indian Institute of Technology (IIT) Kharagpur

---

## 📄 License

This project is intended for educational and academic purposes.

Add an appropriate open-source license file if you intend to distribute the project under a specific license.
