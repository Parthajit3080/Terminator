# Terminator design document

## Overview

Terminator is a single-file C application (`terminator.c`) that combines an Xlib window, a tabbed command interface, and a small shell-like interpreter. The GUI owns the X11 display and tab presentation. Each tab has a shell worker that parses commands and starts external programs. The worker can run as a detached pthread (default) or as a child process (`useProcess=True`). External commands and pipelines are forked in both modes.

## X11 GUI and event loop

`main` opens the display with `XOpenDisplay`, creates one window, selects exposure, keyboard, pointer, and resize events, and registers the window-manager close protocol. It loads a large fixed X font where available and tries smaller fixed-font fallbacks.

The event loop uses `poll` on the X connection and the output pipe for every active tab. It blocks until X input or worker output is ready, drains queued X events with `XPending`/`XNextEvent`, updates the active view, and reads available output into that tab's scrollback. This keeps the interface responsive while avoiding a busy loop. Scrollback is bounded and drops old complete lines when its storage fills; drawing wraps text to the current window width.

## Tabs, workers, and IPC

Each tab stores its input and output pipe descriptors, worker identity, working directory state, editable input, scrollback, and job list. `start_tab` creates two pipes: the GUI writes commands to the worker and reads the worker's output. In process mode the GUI forks a worker and closes the unused pipe ends in each process. In thread mode it starts a detached pthread; because threads share the process file table, the worker closes its descriptors when its shell loop exits.

A worker reads command lines from its input pipe and sends prompts and command output through its output pipe. Pipes isolate each tab's command stream and rendered output, but this is not a PTY: programs do not receive a real terminal device or interactive terminal control sequences.

## Thread mode, process mode, and shared state

`./terminator` starts one detached pthread per tab. The threads share the address space and process resources with the GUI, so history and control data are visible directly through shared mappings. A mutex protects temporary process-wide `chdir` operations used for path expansion, since concurrent threads cannot safely change directories independently.

`./terminator useProcess=True` starts one forked shell worker per tab. The two modes use the same command and UI implementation. Anonymous `MAP_SHARED` mappings hold state that must be visible across processes and threads: persistent command history, foreground process-group IDs, and watcher cancellation/process data. Each mapping has a process-shared POSIX semaphore initialized with `sem_init(..., 1, 1)` to serialize access.

Each shell maintains its own working directory and job table. The GUI keeps a per-tab directory snapshot for UI-side filename completion. This keeps tab navigation independent without changing the GUI process's current directory.

## Command parsing, processes, and jobs

The interpreter tokenizes operators while respecting quoting, escapes, command substitutions, and nested parentheses. It supports a subset of shell expansion and operators; it is not a complete POSIX shell. Built-ins that change shell state, including `cd`, variable assignment, `jobs`, `fg`, `bg`, and script execution, run in the tab's worker. Other commands are launched with `execvp` from forked children.

A pipeline creates a pipe between adjacent stages and forks one child for each command. Children join a common process group, set up their input/output descriptors, apply redirections, close inherited pipeline descriptors, then execute the requested program. The worker tracks individual PIDs as well as the process-group ID, waits for foreground jobs, and records stopped or background jobs in a bounded per-tab job table. `jobs`, `fg`, and `bg` operate on that table. `multiWatch` is a separate worker feature that launches multiple child commands concurrently, captures their output, and repeats them on a timer.

## Pipes, dup2, and redirection

For a pipeline stage, the child selects standard input from the previous pipe and standard output from the next pipe (or the tab output pipe for the final stage). `dup2` installs these descriptors as standard input and output. Supported file redirections include `<`, `>`, `>>`, and `2>`; the child opens the requested file and uses `dup2` to attach it to the appropriate standard descriptor. Pipeline and temporary file descriptors are closed after duplication so that EOF and pipe cleanup work as expected. Standard error follows the pipeline output unless an explicit `2>` target is supplied.

## History and completion data structures

History uses a fixed-size ring of 10,000 entries, each up to 511 command bytes plus a terminator. Its `next`, `count`, and total counters track the ring's position and size. The shared ring is protected by a process-shared semaphore and loaded from/saved to `~/.bash_history`. The `history` built-in displays at most the newest 1,000 entries. Up/Down navigation keeps a separate draft of the line being edited. Ctrl+R checks for a newest exact match, then uses a suffix automaton to find a longest common contiguous substring.

Filename completion scans the active tab's working directory. The input editor tracks a bounded byte buffer and cursor position; completion tracks up to 32 candidate names and whether each candidate is a directory. One match is inserted directly. Multiple matches extend the common prefix and are displayed as numbered choices, which can be selected by entering a number.

## Signals and shutdown

The GUI handles Ctrl+C and Ctrl+Z as process-control input: it reads the active tab's foreground process-group ID from shared control state and sends SIGINT or SIGTSTP to that group. The shell worker waits with `waitpid` (including stopped-child reporting), updates job state, and returns control to the prompt when appropriate. `SIGPIPE` is ignored in thread mode so writes to a closed tab pipe do not terminate the entire GUI process. Closing a tab closes its command channel, cancels active watcher work, signals tracked foreground/watcher groups, and reaps process workers when available.

## Scripts, configuration, and limits

The script reader supports a defined subset of shell syntax, positional parameters, variables, pipelines, redirections, substitutions, and nested `if`/`elif`/`else`/`fi` conditionals with `[ ... ]`/`test` predicates. It validates conditional structure before execution. Keyboard shortcuts can be overridden through `~/.terminatorrc`; `terminatorrc.example` shows the format.

Important implementation limits include a 4,095-byte command input, up to 2,048 pipeline stages subject to operating-system resource limits, 64 tracked jobs per tab, 32 completion candidates, 32 `multiWatch` commands, and a bounded scrollback. There is no PTY, ANSI terminal renderer, shell loops/functions/arrays, or full POSIX/Bash compatibility. X11 is required to display the application.