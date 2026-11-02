#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wordexp.h>

#define MAX_TABS 16
#define BUFFER_SIZE (256 * 1024)
#define INPUT_SIZE 4096
#define MAX_ARGS 128
#define MAX_JOBS 64
#define MAX_VARIABLES 128
#define MAX_PIPE_COMMANDS 2048
#define MAX_SCRIPT_ARGS 32
#define HISTORY_CAPACITY 10000
#define HISTORY_LINE_SIZE 512
#define TAB_BAR_HEIGHT 38
#define LEFT_MARGIN 12
#define MAX_KEYBINDINGS 32

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    char text[BUFFER_SIZE];
    size_t length;
    char input[INPUT_SIZE];
    size_t input_length;
    size_t cursor;
    size_t scroll_lines;
    int history_index;
    char history_draft[INPUT_SIZE];
    int search_mode;
    char search_match[INPUT_SIZE];
    size_t input_selection_anchor;
    int input_selection_active;
    size_t selection_anchor;
    int selection_active;
    size_t completion_count;
    size_t completion_start;
    int completion_pending;
    char completion_choice[4];
    size_t completion_choice_length;
    char completion_items[32][256];
    unsigned char completion_is_dir[32];
    int active;
    int input_fd;
    int output_fd;
    int id;
    pid_t pid;
    pthread_t thread;
    int process_mode;
} Tab;

typedef struct {
    int command_fd;
    int output_fd;
    struct SharedHistory *history;
    struct SharedControl *control;
    int tab_index;
    char cwd[PATH_MAX];
} ShellArgs;

typedef struct {
    int id;
    pid_t pgid;
    int running;
    char command[HISTORY_LINE_SIZE];
    pid_t processes[MAX_PIPE_COMMANDS];
    size_t process_count;
    size_t finished_count;
} Job;

typedef struct SharedHistory {
    sem_t mutex;
    size_t next;
    size_t count;
    uint64_t total;
    char entries[HISTORY_CAPACITY][HISTORY_LINE_SIZE];
} SharedHistory;

typedef struct SharedControl {
    sem_t mutex;
    pid_t foreground[MAX_TABS];
    volatile sig_atomic_t watch_cancel[MAX_TABS];
    pid_t watch_pids[MAX_TABS][MAX_PIPE_COMMANDS];
    size_t watch_pid_count[MAX_TABS];
    char tab_cwd[MAX_TABS][PATH_MAX];
} SharedControl;

typedef struct {
    int command_fd;
    int output_fd;
    char cwd[PATH_MAX];
    int last_status;
    int next_job_id;
    int source_depth;
    Job jobs[MAX_JOBS];
    size_t job_count;
    SharedHistory *history;
    struct {
        char name[64];
        char value[INPUT_SIZE];
    } variables[MAX_VARIABLES];
    size_t variable_count;
    char script_args[MAX_SCRIPT_ARGS][HISTORY_LINE_SIZE];
    size_t script_argc;
    int tab_index;
    int suppress_history;
    pid_t process_group_override;
    SharedControl *control;
} Shell;

typedef enum {
    TOKEN_WORD,
    TOKEN_PIPE,
    TOKEN_INPUT,
    TOKEN_OUTPUT,
    TOKEN_APPEND,
    TOKEN_ERROR_OUTPUT,
    TOKEN_SEMICOLON,
    TOKEN_BACKGROUND,
    TOKEN_AND,
    TOKEN_OR
} TokenType;

typedef struct {
    TokenType type;
    char *text;
} ShellToken;

typedef struct {
    char *argv[MAX_ARGS];
    size_t argc;
    char *input_path;
    char *output_path;
    char *error_path;
    int append_output;
} ParsedCommand;

static pthread_mutex_t wordexp_cwd_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    size_t offset;
    size_t length;
} VisualLine;

static Display *display;
static Window window;
static int screen;
static GC gc;
static XFontStruct *font;
static Atom wm_delete_window;
static Tab tabs[MAX_TABS];
static int current_tab = -1;
static int next_tab_id = 1;
static int process_mode;
static int window_width = 900;
static int window_height = 600;
static int shutting_down;
static unsigned long color_background;
static unsigned long color_panel;
static unsigned long color_tab;
static unsigned long color_tab_active;
static unsigned long color_border;
static unsigned long color_text;
static unsigned long color_muted;
static unsigned long color_accent;
static SharedHistory *shared_history;
static SharedControl *shared_control;
static char initial_cwd[PATH_MAX];
static char history_path[PATH_MAX];
static Atom clipboard_atom;
static Atom utf8_atom;
static Atom targets_atom;
static Atom clipboard_property;
static char selection_copy[BUFFER_SIZE];
static size_t selection_start;
static size_t selection_end;
static int selection_tab = -1;
static int selection_dragging;
static int selection_owner;
static int paste_pending;
static int paste_fallback;
static Atom paste_selection_atom;
static int keybindings_loaded;

typedef enum {
    ACTION_NEW_TAB, ACTION_CLOSE_TAB, ACTION_NEXT_TAB, ACTION_PREVIOUS_TAB,
    ACTION_COPY, ACTION_PASTE, ACTION_SEARCH, ACTION_CLEAR, ACTION_EOF
} KeyAction;

typedef struct {
    KeySym key;
    unsigned int modifiers;
    KeyAction action;
} KeyBinding;

static KeyBinding keybindings[MAX_KEYBINDINGS];
static size_t keybinding_count;

static void draw_window(void);
static void close_tab(int index);
static int execute_line(Shell *shell, const char *line);
static int execute_external(Shell *shell, char **argv, int background, const char *command);
static int add_tab(void);
static void history_lock(SharedHistory *history);
static void control_lock(SharedControl *control);
static void erase_input_range(Tab *tab, size_t start, size_t end);

static unsigned long named_color(const char *name, unsigned long fallback) {
    XColor allocated;
    XColor exact;
    if (XAllocNamedColor(display, DefaultColormap(display, screen), name,
                         &allocated, &exact)) return allocated.pixel;
    return fallback;
}

static void append_text(Tab *tab, const char *text, size_t length) {
    if (length == 0) return;
    if (length >= BUFFER_SIZE) {
        if (selection_tab == (int)(tab - tabs)) selection_tab = -1;
        text += length - (BUFFER_SIZE - 1);
        length = BUFFER_SIZE - 1;
        tab->length = 0;
    }
    if (tab->length + length >= BUFFER_SIZE) {
        size_t remove = tab->length + length - (BUFFER_SIZE - 1);
        size_t newline = 0;
        while (newline < remove && tab->text[newline] != '\n') newline++;
        if (newline < remove) remove = newline + 1;
        if (selection_tab >= 0) {
            if (selection_start < remove || selection_end < remove) {
                selection_tab = -1;
                selection_start = selection_end = 0;
            } else {
                selection_start -= remove;
                selection_end -= remove;
            }
        }
        memmove(tab->text, tab->text + remove, tab->length - remove);
        tab->length -= remove;
        tab->scroll_lines = 0;
    }
    memcpy(tab->text + tab->length, text, length);
    tab->length += length;
    tab->text[tab->length] = '\0';
    if (tab == &tabs[current_tab]) tab->scroll_lines = 0;
}

typedef struct {
    int next[256];
    int link;
    int length;
} SuffixState;

static int suffix_state_new(SuffixState *states, int *count) {
    int index = (*count)++;
    states[index].link = -1;
    states[index].length = 0;
    for (int i = 0; i < 256; i++) states[index].next[i] = -1;
    return index;
}

static void suffix_automaton_build(SuffixState *states, int *state_count,
                                   const char *text, size_t length) {
    *state_count = 0;
    int root = suffix_state_new(states, state_count);
    int last = root;
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)text[i];
        int current = suffix_state_new(states, state_count);
        states[current].length = states[last].length + 1;
        int p = last;
        while (p >= 0 && states[p].next[ch] < 0) {
            states[p].next[ch] = current;
            p = states[p].link;
        }
        if (p < 0) states[current].link = root;
        else {
            int q = states[p].next[ch];
            if (states[p].length + 1 == states[q].length) states[current].link = q;
            else {
                int clone = suffix_state_new(states, state_count);
                states[clone].length = states[p].length + 1;
                states[clone].link = states[q].link;
                memcpy(states[clone].next, states[q].next, sizeof(states[q].next));
                while (p >= 0 && states[p].next[ch] == q) {
                    states[p].next[ch] = clone;
                    p = states[p].link;
                }
                states[q].link = states[current].link = clone;
            }
        }
        last = current;
    }
}

static size_t suffix_match_length(const SuffixState *states, const char *text) {
    int state = 0;
    size_t length = 0, best = 0;
    for (size_t i = 0; text[i]; i++) {
        unsigned char ch = (unsigned char)text[i];
        while (state && states[state].next[ch] < 0) {
            state = states[state].link;
            length = (size_t)states[state].length;
        }
        if (states[state].next[ch] >= 0) {
            state = states[state].next[ch];
            length++;
            if (length > best) best = length;
        } else {
            state = 0;
            length = 0;
        }
    }
    return best;
}

static int history_best_match(const char *term, char *match, size_t match_capacity) {
    SharedHistory *history = shared_history;
    size_t term_length = strlen(term);
    if (term_length > HISTORY_LINE_SIZE - 1) term_length = HISTORY_LINE_SIZE - 1;
    if (term_length == 0 || match_capacity == 0) return 0;
    history_lock(history);
    size_t first = (history->next + HISTORY_CAPACITY - history->count) % HISTORY_CAPACITY;
    for (size_t offset = 0; offset < history->count; offset++) {
        size_t index = (history->next + HISTORY_CAPACITY - 1 - offset) % HISTORY_CAPACITY;
        if (strcmp(history->entries[index], term) == 0) {
            snprintf(match, match_capacity, "%s", history->entries[index]);
            sem_post(&history->mutex);
            return 1;
        }
    }
    size_t max_states = term_length * 2 + 1;
    SuffixState *states = calloc(max_states, sizeof(*states));
    if (!states) {
        sem_post(&history->mutex);
        return 0;
    }
    int state_count = 0;
    suffix_automaton_build(states, &state_count, term, term_length);
    size_t longest = 0;
    for (size_t offset = 0; offset < history->count; offset++) {
        size_t index = (first + offset) % HISTORY_CAPACITY;
        size_t matched = suffix_match_length(states, history->entries[index]);
        if (matched > longest) longest = matched;
    }
    int found = 0;
    if (longest > 2) {
        for (size_t offset = 0; offset < history->count; offset++) {
            size_t index = (history->next + HISTORY_CAPACITY - 1 - offset) % HISTORY_CAPACITY;
            if (suffix_match_length(states, history->entries[index]) == longest) {
                snprintf(match, match_capacity, "%s", history->entries[index]);
                found = 1;
                break;
            }
        }
    }
    free(states);
    sem_post(&history->mutex);
    return found;
}

static void write_all(int fd, const char *data, size_t length) {
    while (length > 0) {
        ssize_t written = write(fd, data, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return;
        data += written;
        length -= (size_t)written;
    }
}

static void history_lock(SharedHistory *history) {
    while (sem_wait(&history->mutex) < 0 && errno == EINTR) { }
}

static void control_lock(SharedControl *control) {
    while (sem_wait(&control->mutex) < 0 && errno == EINTR) { }
}

static void set_foreground(Shell *shell, pid_t pgid) {
    control_lock(shell->control);
    shell->control->foreground[shell->tab_index] = pgid;
    sem_post(&shell->control->mutex);
}

static void close_inherited_gui_fds(void) {
    /* Process workers already closed the GUI ends when their tab was created.
       In thread mode, forked command children must drop every inherited tab end. */
    if (process_mode) return;
    for (int i = 0; i < MAX_TABS; i++) {
        if (!tabs[i].active) continue;
        if (tabs[i].input_fd >= 0) close(tabs[i].input_fd);
        if (tabs[i].output_fd >= 0) close(tabs[i].output_fd);
    }
}

static size_t encode_history_line(const char *line, char *encoded, size_t capacity) {
    size_t used = 0;
    for (size_t i = 0; line[i] && used + 2 < capacity; i++) {
        if (line[i] == '\n') { encoded[used++] = '\\'; encoded[used++] = 'n'; }
        else if (line[i] == '\r') { encoded[used++] = '\\'; encoded[used++] = 'r'; }
        else if (line[i] == '\\') { encoded[used++] = '\\'; encoded[used++] = '\\'; }
        else encoded[used++] = line[i];
    }
    encoded[used++] = '\n';
    return used;
}

static void rewrite_history_file_locked(SharedHistory *history) {
    if (!history_path[0]) return;
    char temporary[PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", history_path) >= (int)sizeof(temporary)) return;
    int fd = open(temporary, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) return;
    size_t first = (history->next + HISTORY_CAPACITY - history->count) % HISTORY_CAPACITY;
    for (size_t i = 0; i < history->count; i++) {
        size_t index = (first + i) % HISTORY_CAPACITY;
        char encoded[HISTORY_LINE_SIZE * 2 + 2];
        size_t length = encode_history_line(history->entries[index], encoded, sizeof(encoded));
        write_all(fd, encoded, length);
    }
    close(fd);
    rename(temporary, history_path);
}

static void history_add(SharedHistory *history, const char *line) {
    history_lock(history);
    size_t slot = history->next;
    size_t line_length = strnlen(line, HISTORY_LINE_SIZE - 1);
    memcpy(history->entries[slot], line, line_length);
    history->entries[slot][line_length] = '\0';
    history->next = (slot + 1) % HISTORY_CAPACITY;
    if (history->count < HISTORY_CAPACITY) history->count++;
    history->total++;
    if (history_path[0]) {
        if (history->total > HISTORY_CAPACITY) {
            rewrite_history_file_locked(history);
        } else {
            int fd = open(history_path, O_CREAT | O_WRONLY | O_APPEND, 0600);
            if (fd >= 0) {
                char encoded[HISTORY_LINE_SIZE * 2 + 2];
                size_t length = encode_history_line(history->entries[slot], encoded, sizeof(encoded));
                write_all(fd, encoded, length);
                close(fd);
            }
        }
    }
    sem_post(&history->mutex);
}

static void history_load_file(SharedHistory *history) {
    if (!history_path[0]) return;
    FILE *file = fopen(history_path, "r");
    if (!file) return;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        if (length > 0 && line[length - 1] == '\n') line[--length] = '\0';
        for (ssize_t i = 0; i + 1 < length; i++) {
            if (line[i] == '\\' && line[i + 1] == 'n') { line[i] = '\n'; memmove(line + i + 1, line + i + 2, (size_t)(length - i - 1)); length--; }
            else if (line[i] == '\\' && line[i + 1] == 'r') { line[i] = '\r'; memmove(line + i + 1, line + i + 2, (size_t)(length - i - 1)); length--; }
            else if (line[i] == '\\' && line[i + 1] == '\\') { line[i] = '\\'; memmove(line + i + 1, line + i + 2, (size_t)(length - i - 1)); length--; }
        }
        size_t slot = history->next;
        strncpy(history->entries[slot], line, HISTORY_LINE_SIZE - 1);
        history->entries[slot][HISTORY_LINE_SIZE - 1] = '\0';
        history->next = (slot + 1) % HISTORY_CAPACITY;
        if (history->count < HISTORY_CAPACITY) history->count++;
        history->total++;
    }
    free(line);
    fclose(file);
    if (history->total > HISTORY_CAPACITY) history->total = HISTORY_CAPACITY;
}

static void history_print(Shell *shell) {
    SharedHistory *history = shell->history;
    history_lock(history);
    size_t shown = history->count < 1000 ? history->count : 1000;
    size_t first = (history->next + HISTORY_CAPACITY - shown) % HISTORY_CAPACITY;
    for (size_t i = 0; i < shown; i++) {
        size_t index = (first + i) % HISTORY_CAPACITY;
        dprintf(shell->output_fd, "%zu  %s\n", i + 1, history->entries[index]);
    }
    sem_post(&history->mutex);
}

static int resolve_path(Shell *shell, const char *path, char *resolved) {
    char candidate[PATH_MAX];
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        const char *home = getenv("HOME");
        if (!home) home = shell->cwd;
        if (snprintf(candidate, sizeof(candidate), "%s%s", home, path + 1) >= (int)sizeof(candidate)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    } else if (path[0] == '/') {
        if (snprintf(candidate, sizeof(candidate), "%s", path) >= (int)sizeof(candidate)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    } else {
        if (snprintf(candidate, sizeof(candidate), "%s/%s", shell->cwd, path) >= (int)sizeof(candidate)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }
    return realpath(candidate, resolved) ? 0 : -1;
}

static void remove_job(Shell *shell, size_t index) {
    if (index + 1 < shell->job_count)
        memmove(&shell->jobs[index], &shell->jobs[index + 1],
                (shell->job_count - index - 1) * sizeof(shell->jobs[0]));
    shell->job_count--;
}

static void refresh_jobs(Shell *shell) {
    for (size_t i = 0; i < shell->job_count;) {
        Job *job = &shell->jobs[i];
        for (size_t p = 0; p < job->process_count; p++) {
            if (job->processes[p] <= 0) continue;
            int status;
            pid_t result = waitpid(job->processes[p], &status,
                                   WNOHANG | WUNTRACED | WCONTINUED);
            if (result == job->processes[p]) {
                if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    job->processes[p] = 0;
                    job->finished_count++;
                } else if (WIFSTOPPED(status)) job->running = 0;
                else if (WIFCONTINUED(status)) job->running = 1;
            } else if (result < 0 && errno == ECHILD) {
                job->processes[p] = 0;
                job->finished_count++;
            }
        }
        if (job->finished_count >= job->process_count) {
            remove_job(shell, i);
            continue;
        }
        i++;
    }
}

static int add_job(Shell *shell, pid_t pgid, pid_t child, int running, const char *command) {
    if (shell->job_count == MAX_JOBS) return -1;
    Job *job = &shell->jobs[shell->job_count++];
    job->id = shell->next_job_id++;
    job->pgid = pgid;
    job->running = running;
    strncpy(job->command, command, sizeof(job->command) - 1);
    job->command[sizeof(job->command) - 1] = '\0';
    job->processes[0] = child;
    job->process_count = 1;
    job->finished_count = 0;
    return job->id;
}

static int add_job_group(Shell *shell, pid_t pgid, const pid_t *processes,
                         size_t count, int running, const char *command) {
    if (count == 0 || count > MAX_PIPE_COMMANDS || shell->job_count == MAX_JOBS) return -1;
    Job *job = &shell->jobs[shell->job_count++];
    memset(job, 0, sizeof(*job));
    job->id = shell->next_job_id++;
    job->pgid = pgid;
    job->running = running;
    job->process_count = count;
    strncpy(job->command, command, sizeof(job->command) - 1);
    memcpy(job->processes, processes, count * sizeof(processes[0]));
    return job->id;
}

static size_t find_job(Shell *shell, const char *spec) {
    refresh_jobs(shell);
    if (shell->job_count == 0) return (size_t)-1;
    if (!spec || !*spec) return shell->job_count - 1;
    if (*spec == '%') spec++;
    char *end = NULL;
    long id = strtol(spec, &end, 10);
    if (!end || *end || id <= 0) return (size_t)-1;
    for (size_t i = 0; i < shell->job_count; i++)
        if (shell->jobs[i].id == id) return i;
    return (size_t)-1;
}

static void print_jobs(Shell *shell) {
    refresh_jobs(shell);
    for (size_t i = 0; i < shell->job_count; i++)
        dprintf(shell->output_fd, "[%d]  %s  %s\n", shell->jobs[i].id,
                shell->jobs[i].running ? "Running" : "Stopped",
                shell->jobs[i].command);
}

static int wait_for_job(Shell *shell, size_t index) {
    Job *job = &shell->jobs[index];
    int final_status = 0;
    for (size_t p = 0; p < job->process_count; p++) {
        if (job->processes[p] <= 0) continue;
        int status = 0;
        pid_t result;
        do { result = waitpid(job->processes[p], &status, WUNTRACED); }
        while (result < 0 && errno == EINTR);
        if (result < 0) {
            dprintf(shell->output_fd, "terminator: wait: %s\n", strerror(errno));
            job->processes[p] = 0;
            job->finished_count++;
            final_status = 1;
            continue;
        }
        if (WIFSTOPPED(status)) {
            job->running = 0;
            dprintf(shell->output_fd, "[%d]+ Stopped %s\n", job->id, job->command);
            return 128 + WSTOPSIG(status);
        }
        job->processes[p] = 0;
        job->finished_count++;
        if (WIFEXITED(status)) final_status = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) final_status = 128 + WTERMSIG(status);
    }
    remove_job(shell, index);
    return final_status;
}

typedef struct {
    char command[INPUT_SIZE];
    pid_t pid;
    int fd;
    int header_written;
    int last_character;
    time_t started_at;
    time_t restart_at;
} WatchTask;

static void emit_watch_footer(Shell *shell, WatchTask *task) {
    static const char divider[] = "----------------------------------------------------\n";
    if (task->last_character != '\n') write_all(shell->output_fd, "\n", 1);
    write_all(shell->output_fd, divider, sizeof(divider) - 1);
}

static void publish_watch_children(Shell *shell, WatchTask *tasks, size_t count) {
    control_lock(shell->control);
    size_t stored = 0;
    for (size_t i = 0; i < count && stored < MAX_PIPE_COMMANDS; i++)
        if (tasks[i].pid > 0) shell->control->watch_pids[shell->tab_index][stored++] = tasks[i].pid;
    shell->control->watch_pid_count[shell->tab_index] = stored;
    sem_post(&shell->control->mutex);
}

static int launch_watch_task(Shell *shell, WatchTask *task) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        dprintf(shell->output_fd, "terminator: multiWatch: pipe: %s\n", strerror(errno));
        return -1;
    }
    pid_t child = fork();
    if (child < 0) {
        dprintf(shell->output_fd, "terminator: multiWatch: fork: %s\n", strerror(errno));
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }
    if (child == 0) {
        setpgid(0, 0);
        close(pipefd[0]);
        Shell child_shell = *shell;
        child_shell.output_fd = pipefd[1];
        child_shell.process_group_override = getpid();
        child_shell.suppress_history = 1;
        close_inherited_gui_fds();
        close(shell->output_fd);
        int exit_requested = execute_line(&child_shell, task->command);
        close(pipefd[1]);
        _exit(exit_requested ? child_shell.last_status : child_shell.last_status);
    }
    setpgid(child, child);
    close(pipefd[1]);
    task->pid = child;
    task->fd = pipefd[0];
    task->header_written = 0;
    task->last_character = '\n';
    task->started_at = time(NULL);
    return 0;
}

static void emit_watch_header(Shell *shell, WatchTask *task) {
    if (task->header_written) return;
    dprintf(shell->output_fd, "\"%s\", %ld:\n----------------------------------------------------\n",
            task->command, (long)task->started_at);
    task->header_written = 1;
}

static int run_multiwatch_once(Shell *shell, const char *argument_text) {
    WatchTask tasks[32];
    memset(tasks, 0, sizeof(tasks));
    for (size_t i = 0; i < 32; i++) tasks[i].fd = -1;
    size_t count = 0;
    const char *p = argument_text;
    while (*p && (isspace((unsigned char)*p) || *p == '[')) p++;
    while (*p && *p != ']' && count < 32) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p || *p == ']') break;
        char quote = 0;
        if (*p == '"' || *p == '\'') quote = *p++;
        size_t used = 0;
        int escaped = 0;
        while (*p) {
            if (escaped) { if (used + 1 < sizeof(tasks[count].command)) tasks[count].command[used++] = *p; escaped = 0; p++; continue; }
            if (*p == '\\' && quote) { escaped = 1; p++; continue; }
            if (quote) { if (*p == quote) { p++; break; } }
            else if (*p == ',' || *p == ']') break;
            if (used + 1 < sizeof(tasks[count].command)) tasks[count].command[used++] = *p;
            p++;
        }
        tasks[count].command[used] = '\0';
        if (used) count++;
        while (*p && *p != ',' && *p != ']') p++;
        if (*p == ',') p++;
    }
    if (count == 0) {
        dprintf(shell->output_fd, "usage: multiWatch [\"command\", \"command\"]\n");
        return 2;
    }
    control_lock(shell->control);
    shell->control->watch_cancel[shell->tab_index] = 0;
    shell->control->watch_pid_count[shell->tab_index] = 0;
    sem_post(&shell->control->mutex);
    for (size_t i = 0; i < count; i++) {
        tasks[i].restart_at = 0;
        if (launch_watch_task(shell, &tasks[i]) < 0) tasks[i].restart_at = time(NULL) + 2;
    }
    publish_watch_children(shell, tasks, count);
    int cancelled = 0;
    time_t cancel_started = 0;
    for (;;) {
        control_lock(shell->control);
        cancelled = shell->control->watch_cancel[shell->tab_index];
        sem_post(&shell->control->mutex);
        if (cancelled && !cancel_started) {
            cancel_started = time(NULL);
            for (size_t i = 0; i < count; i++)
                if (tasks[i].pid > 0) kill(-tasks[i].pid, SIGTERM);
        }
        if (cancelled && time(NULL) - cancel_started >= 1)
            for (size_t i = 0; i < count; i++) if (tasks[i].pid > 0) kill(-tasks[i].pid, SIGKILL);

        struct pollfd fds[32];
        size_t mapping[32];
        nfds_t nfds = 0;
        time_t now = time(NULL);
        int timeout = 2000;
        int any_active = 0;
        for (size_t i = 0; i < count; i++) {
            if (tasks[i].pid > 0 && tasks[i].fd >= 0) {
                fds[nfds] = (struct pollfd){tasks[i].fd, POLLIN | POLLHUP, 0};
                mapping[nfds++] = i;
                any_active = 1;
            } else if (tasks[i].pid > 0) any_active = 1;
            else if (!cancelled) {
                if (tasks[i].restart_at <= now) {
                    if (launch_watch_task(shell, &tasks[i]) < 0) tasks[i].restart_at = now + 2;
                    else any_active = 1;
                } else {
                    int remaining = (int)(tasks[i].restart_at - now) * 1000;
                    if (remaining < timeout) timeout = remaining;
                }
            }
        }
        publish_watch_children(shell, tasks, count);
        int ready = poll(fds, nfds, timeout);
        if (ready < 0 && errno != EINTR) {
            dprintf(shell->output_fd, "terminator: multiWatch: poll: %s\n", strerror(errno));
            cancelled = 1;
        }
        for (nfds_t pfd = 0; pfd < nfds; pfd++) {
            if (!(fds[pfd].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            size_t index = mapping[pfd];
            char buffer[2048];
            ssize_t bytes = read(tasks[index].fd, buffer, sizeof(buffer));
            if (bytes > 0) {
                emit_watch_header(shell, &tasks[index]);
                write_all(shell->output_fd, buffer, (size_t)bytes);
                tasks[index].last_character = (unsigned char)buffer[bytes - 1];
            } else if (bytes == 0 || (bytes < 0 && errno != EINTR && errno != EAGAIN)) {
                emit_watch_header(shell, &tasks[index]);
                emit_watch_footer(shell, &tasks[index]);
                close(tasks[index].fd);
                tasks[index].fd = -1;
                int status;
                while (waitpid(tasks[index].pid, &status, 0) < 0 && errno == EINTR) { }
                tasks[index].pid = 0;
                tasks[index].restart_at = time(NULL) + 2;
            }
        }
        if (cancelled) {
            int remaining = 0;
            for (size_t i = 0; i < count; i++) {
                if (tasks[i].pid <= 0) continue;
                int status;
                pid_t result = waitpid(tasks[i].pid, &status, WNOHANG);
                if (result == tasks[i].pid || (result < 0 && errno == ECHILD)) {
                    if (tasks[i].fd >= 0) {
                        char buffer[2048];
                        ssize_t bytes;
                        while ((bytes = read(tasks[i].fd, buffer, sizeof(buffer))) > 0) {
                            emit_watch_header(shell, &tasks[i]);
                            write_all(shell->output_fd, buffer, (size_t)bytes);
                            tasks[i].last_character = (unsigned char)buffer[bytes - 1];
                        }
                        emit_watch_header(shell, &tasks[i]);
                        emit_watch_footer(shell, &tasks[i]);
                        close(tasks[i].fd);
                        tasks[i].fd = -1;
                    }
                    tasks[i].pid = 0;
                } else remaining++;
            }
            if (!remaining) break;
        } else if (!any_active) {
            int waiting = 0;
            for (size_t i = 0; i < count; i++) if (tasks[i].pid > 0 || tasks[i].restart_at > time(NULL)) waiting = 1;
            if (!waiting) break;
        }
    }
    control_lock(shell->control);
    shell->control->watch_pid_count[shell->tab_index] = 0;
    shell->control->watch_cancel[shell->tab_index] = 0;
    memset(shell->control->watch_pids[shell->tab_index], 0, sizeof(shell->control->watch_pids[shell->tab_index]));
    sem_post(&shell->control->mutex);
    return cancelled ? 130 : 0;
}

typedef enum { SCRIPT_NORMAL, SCRIPT_IF, SCRIPT_ELIF, SCRIPT_ELSE, SCRIPT_FI, SCRIPT_THEN } ScriptKind;
typedef struct { ScriptKind kind; char *text; char *condition; int needs_then; } ScriptLine;

static void strip_script_comment(char *line) {
    char quote = 0;
    int escaped = 0;
    for (size_t i = 0; line[i]; i++) {
        char ch = line[i];
        if (escaped) { escaped = 0; continue; }
        if (ch == '\\' && quote != '\'') { escaped = 1; continue; }
        if (quote) { if (ch == quote) quote = 0; continue; }
        if (ch == '\'' || ch == '"') { quote = ch; continue; }
        if (ch == '#' && (i == 0 || isspace((unsigned char)line[i - 1]))) {
            line[i] = '\0';
            return;
        }
    }
}

static char *trim_script_space(char *text) {
    while (isspace((unsigned char)*text)) text++;
    size_t length = strlen(text);
    while (length && isspace((unsigned char)text[length - 1])) text[--length] = '\0';
    return text;
}

static int parse_script_line(char *line, ScriptLine *result) {
    memset(result, 0, sizeof(*result));
    strip_script_comment(line);
    char *text = trim_script_space(line);
    if (!*text) return 0;
    if (strcmp(text, "then") == 0) { result->kind = SCRIPT_THEN; return 1; }
    if (strcmp(text, "else") == 0) { result->kind = SCRIPT_ELSE; return 1; }
    if (strcmp(text, "fi") == 0) { result->kind = SCRIPT_FI; return 1; }
    size_t prefix = 0;
    if (strncmp(text, "if ", 3) == 0) { result->kind = SCRIPT_IF; prefix = 3; }
    else if (strncmp(text, "elif ", 5) == 0) { result->kind = SCRIPT_ELIF; prefix = 5; }
    if (prefix) {
        char *condition = text + prefix;
        char *then = strstr(condition, "; then");
        if (then) {
            if (*trim_script_space(then + 6)) return -1;
            *then = '\0';
        }
        else {
            size_t length = strlen(condition);
            if (length >= 5 && strcmp(condition + length - 5, " then") == 0)
                condition[length - 5] = '\0';
            else result->needs_then = 1;
        }
        condition = trim_script_space(condition);
        size_t length = strlen(condition);
        if (length && condition[length - 1] == ';') condition[--length] = '\0';
        condition = trim_script_space(condition);
        if (!*condition) return -1;
        result->condition = strdup(condition);
        if (!result->condition) return -1;
        return 1;
    }
    result->kind = SCRIPT_NORMAL;
    result->text = strdup(text);
    return result->text ? 1 : -1;
}

static void free_script_lines(ScriptLine *lines, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(lines[i].text);
        free(lines[i].condition);
    }
    free(lines);
}

static int validate_script(Shell *shell, ScriptLine *lines, size_t count) {
    int saw_else[128] = {0};
    size_t depth = 0;
    int valid = 1;
    for (size_t i = 0; i < count; i++) {
        if (lines[i].kind == SCRIPT_IF || lines[i].kind == SCRIPT_ELIF) {
            const char *condition = lines[i].condition;
            if (!condition || (condition[0] != '[' && strncmp(condition, "test ", 5) != 0)) {
                valid = 0;
                break;
            }
            if (condition[0] == '[') {
                size_t n = strlen(condition);
                if (n < 3 || condition[n - 1] != ']') { valid = 0; break; }
            }
            if (lines[i].needs_then &&
                (i + 1 >= count || lines[i + 1].kind != SCRIPT_THEN)) {
                valid = 0;
                break;
            }
        }
        if (lines[i].kind == SCRIPT_THEN &&
            (i == 0 || !lines[i - 1].needs_then ||
             (lines[i - 1].kind != SCRIPT_IF && lines[i - 1].kind != SCRIPT_ELIF))) {
            valid = 0;
            break;
        }
        if (lines[i].kind == SCRIPT_IF) {
            if (depth == sizeof(saw_else) / sizeof(saw_else[0])) { valid = 0; break; }
            saw_else[depth++] = 0;
        } else if (lines[i].kind == SCRIPT_ELIF) {
            if (depth == 0 || saw_else[depth - 1]) { valid = 0; break; }
        } else if (lines[i].kind == SCRIPT_ELSE) {
            if (depth == 0 || saw_else[depth - 1]) { valid = 0; break; }
            saw_else[depth - 1] = 1;
        } else if (lines[i].kind == SCRIPT_FI) {
            if (depth == 0) { valid = 0; break; }
            depth--;
        }
        continue;
    }
    if (valid && depth == 0) return 1;
    dprintf(shell->output_fd, "terminator: script syntax error: unmatched or misplaced if/elif/else/fi\n");
    return 0;
}

static int evaluate_script_condition(Shell *shell, const char *condition) {
    char copy[INPUT_SIZE];
    snprintf(copy, sizeof(copy), "%s", condition);
    char *start = trim_script_space(copy);
    if (strncmp(start, "[", 1) != 0 && strncmp(start, "test ", 5) != 0) {
        dprintf(shell->output_fd, "terminator: script syntax error: if condition must use [ ... ] or test\n");
        shell->last_status = 2;
        return 0;
    }
    execute_line(shell, start);
    return shell->last_status == 0;
}

static int execute_script_block(Shell *shell, ScriptLine *lines, size_t count,
                                size_t *position, int enabled, ScriptKind *stop);

static int execute_script_if(Shell *shell, ScriptLine *lines, size_t count,
                             size_t *position, int enabled) {
    if (*position >= count || lines[*position].kind != SCRIPT_IF) return 0;
    ScriptLine *opening = &lines[*position];
    int needs_then = opening->needs_then;
    int taken = enabled && evaluate_script_condition(shell, opening->condition);
    (*position)++;
    if (needs_then && *position < count && lines[*position].kind == SCRIPT_THEN) (*position)++;
    ScriptKind stop = SCRIPT_NORMAL;
    for (;;) {
        if (execute_script_block(shell, lines, count, position, enabled && taken, &stop)) return 1;
        if (stop == SCRIPT_ELIF) {
            ScriptLine *branch = &lines[*position];
            int branch_needs_then = branch->needs_then;
            int condition = enabled && !taken &&
                evaluate_script_condition(shell, branch->condition);
            if (condition) taken = 1;
            (*position)++;
            if (branch_needs_then && *position < count && lines[*position].kind == SCRIPT_THEN)
                (*position)++;
            continue;
        }
        if (stop == SCRIPT_ELSE) {
            (*position)++;
            if (execute_script_block(shell, lines, count, position,
                                     enabled && !taken, &stop)) return 1;
        }
        if (stop == SCRIPT_FI) { (*position)++; return 0; }
        dprintf(shell->output_fd, "terminator: script syntax error: expected elif, else, or fi\n");
        shell->last_status = 2;
        return 0;
    }
}

static int execute_script_block(Shell *shell, ScriptLine *lines, size_t count,
                                size_t *position, int enabled, ScriptKind *stop) {
    while (*position < count) {
        ScriptLine *line = &lines[*position];
        if (line->kind == SCRIPT_ELIF || line->kind == SCRIPT_ELSE || line->kind == SCRIPT_FI) {
            *stop = line->kind;
            return 0;
        }
        if (line->kind == SCRIPT_IF) {
            if (execute_script_if(shell, lines, count, position, enabled)) return 1;
            continue;
        }
        (*position)++;
        if (enabled && line->kind == SCRIPT_NORMAL && line->text &&
            execute_line(shell, line->text)) return 1;
    }
    *stop = SCRIPT_NORMAL;
    return 0;
}

static int source_file(Shell *shell, const char *filename,
                       char **arguments, size_t argument_count) {
    if (shell->source_depth >= 32) {
        dprintf(shell->output_fd, "terminator: source: maximum nesting depth reached\n");
        shell->last_status = 1;
        return 0;
    }
    char path[PATH_MAX];
    if (resolve_path(shell, filename, path) < 0) {
        dprintf(shell->output_fd, "terminator: source: %s: %s\n", filename, strerror(errno));
        shell->last_status = 1;
        return 0;
    }
    FILE *file = fopen(path, "r");
    if (!file) {
        dprintf(shell->output_fd, "terminator: source: %s: %s\n", filename, strerror(errno));
        shell->last_status = 1;
        return 0;
    }
    ScriptLine *lines = NULL;
    size_t line_count = 0, line_capacity = 0;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    int parse_error = 0;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = '\0';
        ScriptLine parsed;
        int result = parse_script_line(line, &parsed);
        if (result < 0) { parse_error = 1; break; }
        if (result == 0) continue;
        if (line_count == line_capacity) {
            size_t next_capacity = line_capacity ? line_capacity * 2 : 64;
            ScriptLine *next = realloc(lines, next_capacity * sizeof(*lines));
            if (!next) { free(parsed.text); free(parsed.condition); parse_error = 1; break; }
            lines = next;
            line_capacity = next_capacity;
        }
        lines[line_count++] = parsed;
    }
    free(line);
    fclose(file);
    if (parse_error || !validate_script(shell, lines, line_count)) {
        if (parse_error) dprintf(shell->output_fd, "terminator: source: could not parse script\n");
        free_script_lines(lines, line_count);
        shell->last_status = 2;
        return 0;
    }
    char saved_arguments[MAX_SCRIPT_ARGS][HISTORY_LINE_SIZE];
    memcpy(saved_arguments, shell->script_args, sizeof(saved_arguments));
    size_t saved_argc = shell->script_argc;
    int saved_suppress_history = shell->suppress_history;
    memset(shell->script_args, 0, sizeof(shell->script_args));
    shell->script_argc = argument_count + 1;
    if (shell->script_argc > MAX_SCRIPT_ARGS) shell->script_argc = MAX_SCRIPT_ARGS;
    snprintf(shell->script_args[0], sizeof(shell->script_args[0]), "%s", filename);
    for (size_t i = 1; i < shell->script_argc; i++)
        snprintf(shell->script_args[i], sizeof(shell->script_args[i]), "%s", arguments[i - 1]);
    shell->suppress_history = 1;
    shell->source_depth++;
    shell->last_status = 0;
    size_t position = 0;
    ScriptKind stop;
    int should_exit = execute_script_block(shell, lines, line_count, &position, 1, &stop);
    shell->source_depth--;
    shell->suppress_history = saved_suppress_history;
    shell->script_argc = saved_argc;
    memcpy(shell->script_args, saved_arguments, sizeof(saved_arguments));
    free_script_lines(lines, line_count);
    return should_exit;
}

static const char *shell_variable(Shell *shell, const char *name) {
    for (size_t i = 0; i < shell->variable_count; i++)
        if (strcmp(shell->variables[i].name, name) == 0) return shell->variables[i].value;
    const char *environment = getenv(name);
    return environment ? environment : "";
}

static void append_quoted_value(char *output, size_t capacity, size_t *used,
                                const char *value, char quote) {
    if (quote != '"') {
        if (*used + 1 < capacity) output[(*used)++] = '"';
        for (size_t i = 0; value[i] && *used + 2 < capacity; i++) {
            if (value[i] == '\\' || value[i] == '"' || value[i] == '$' || value[i] == '`')
                output[(*used)++] = '\\';
            output[(*used)++] = value[i];
        }
        if (*used + 1 < capacity) output[(*used)++] = '"';
        return;
    }
    for (size_t i = 0; value[i] && *used + 2 < capacity; i++) {
        if (value[i] == '\\' || value[i] == '"' || value[i] == '$' || value[i] == '`')
            output[(*used)++] = '\\';
        output[(*used)++] = value[i];
    }
}

static void expand_shell_references(Shell *shell, const char *source,
                                    char *destination, size_t capacity) {
    size_t out = 0;
    char quote = 0;
    for (size_t i = 0; source[i] && out + 1 < capacity;) {
        char ch = source[i];
        if (ch == '\\' && quote != '\'') {
            destination[out++] = ch;
            i++;
            if (source[i] && out + 1 < capacity) destination[out++] = source[i++];
            continue;
        }
        if (quote == '\'') {
            destination[out++] = ch;
            if (ch == quote) quote = 0;
            i++;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
            destination[out++] = ch;
            i++;
            continue;
        }
        if (ch != '$') {
            destination[out++] = ch;
            i++;
            continue;
        }
        char value[INPUT_SIZE];
        value[0] = '\0';
        size_t advance = 1;
        if (source[i + 1] == '?') {
            snprintf(value, sizeof(value), "%d", shell->last_status);
            advance = 2;
        } else if (source[i + 1] == '#') {
            snprintf(value, sizeof(value), "%zu", shell->script_argc ? shell->script_argc - 1 : 0);
            advance = 2;
        } else if (source[i + 1] == '$') {
            snprintf(value, sizeof(value), "%ld", (long)getpid());
            advance = 2;
        } else if (isdigit((unsigned char)source[i + 1])) {
            size_t index = (size_t)(source[i + 1] - '0');
            if (index < shell->script_argc)
                snprintf(value, sizeof(value), "%s", shell->script_args[index]);
            advance = 2;
        } else {
            size_t begin = i + 1;
            size_t end = begin;
            if (source[begin] == '{') {
                begin++;
                end = begin;
                while (source[end] && source[end] != '}') end++;
                if (source[end] == '}') advance = end - i + 1;
                else { destination[out++] = '$'; i++; continue; }
            } else {
                while (isalnum((unsigned char)source[end]) || source[end] == '_') end++;
                if (end == begin) { destination[out++] = '$'; i++; continue; }
                advance = end - i;
            }
            char name[128];
            size_t name_length = end - begin;
            if (name_length >= sizeof(name)) name_length = sizeof(name) - 1;
            memcpy(name, source + begin, name_length);
            name[name_length] = '\0';
            snprintf(value, sizeof(value), "%s", shell_variable(shell, name));
        }
        append_quoted_value(destination, capacity, &out, value, quote);
        i += advance;
    }
    destination[out] = '\0';
}

static int expand_word(Shell *shell, const char *word, wordexp_t *result) {
    char expanded[INPUT_SIZE * 4];
    expand_shell_references(shell, word, expanded, sizeof(expanded));
    char saved_cwd[PATH_MAX];
    pthread_mutex_lock(&wordexp_cwd_mutex);
    int have_cwd = getcwd(saved_cwd, sizeof(saved_cwd)) != NULL;
    int changed = have_cwd && chdir(shell->cwd) == 0;
    int error = wordexp(expanded, result, 0);
    int saved_errno = errno;
    int restore_error = 0;
    if (changed && chdir(saved_cwd) < 0) restore_error = errno;
    pthread_mutex_unlock(&wordexp_cwd_mutex);
    if (restore_error) {
        errno = restore_error;
        return WRDE_BADVAL;
    }
    if (!changed && have_cwd) {
        errno = saved_errno;
        return WRDE_BADVAL;
    }
    return error;
}

static int tokenize_shell_line(const char *line, ShellToken *tokens, size_t token_capacity,
                               size_t *token_count, char *storage, size_t storage_capacity) {
    char word[INPUT_SIZE];
    size_t word_length = 0, used = 0, count = 0;
    char quote = 0;
    int escaped = 0, substitution_depth = 0, backtick = 0;
    #define FLUSH_WORD() do { \
        if (word_length) { \
            if (count >= token_capacity || used + word_length + 1 > storage_capacity) return -1; \
            memcpy(storage + used, word, word_length); \
            storage[used + word_length] = '\0'; \
            tokens[count++] = (ShellToken){TOKEN_WORD, storage + used}; \
            used += word_length + 1; \
            word_length = 0; \
        } \
    } while (0)
    #define PUSH_OPERATOR(kind) do { \
        if (count >= token_capacity) return -1; \
        tokens[count++] = (ShellToken){kind, NULL}; \
    } while (0)

    for (size_t i = 0; line[i]; i++) {
        char ch = line[i];
        if (escaped) {
            if (word_length + 1 < sizeof(word)) word[word_length++] = ch;
            escaped = 0;
            continue;
        }
        if (ch == '\\' && quote != '\'') {
            if (word_length + 1 < sizeof(word)) word[word_length++] = ch;
            escaped = 1;
            continue;
        }
        if (quote) {
            if (word_length + 1 < sizeof(word)) word[word_length++] = ch;
            if (quote == '"' && substitution_depth > 0) {
                if (ch == '(' && !(i > 0 && line[i - 1] == '$')) substitution_depth++;
                else if (ch == ')') substitution_depth--;
            }
            if (ch == quote) quote = 0;
            else if (quote == '"' && ch == '$' && line[i + 1] == '(') substitution_depth++;
            continue;
        }
        if ((ch == '\'' || ch == '"') && !backtick) {
            quote = ch;
            if (word_length + 1 < sizeof(word)) word[word_length++] = ch;
            continue;
        }
        if (ch == '`') {
            backtick = !backtick;
            if (word_length + 1 < sizeof(word)) word[word_length++] = ch;
            continue;
        }
        if (ch == '$' && line[i + 1] == '(' && !backtick) {
            substitution_depth++;
            if (word_length + 2 < sizeof(word)) {
                word[word_length++] = ch;
                word[word_length++] = line[++i];
            }
            continue;
        }
        if (substitution_depth > 0) {
            if (ch == '(') substitution_depth++;
            if (ch == ')') substitution_depth--;
            if (word_length + 1 < sizeof(word)) word[word_length++] = ch;
            continue;
        }
        if (backtick) {
            if (word_length + 1 < sizeof(word)) word[word_length++] = ch;
            continue;
        }
        if (isspace((unsigned char)ch)) { FLUSH_WORD(); continue; }
        if (ch == '2' && line[i + 1] == '>' && word_length == 0) {
            PUSH_OPERATOR(TOKEN_ERROR_OUTPUT);
            i++;
            continue;
        }
        if (ch == '|') {
            FLUSH_WORD();
            if (line[i + 1] == '|') { PUSH_OPERATOR(TOKEN_OR); i++; }
            else PUSH_OPERATOR(TOKEN_PIPE);
            continue;
        }
        if (ch == '<') { FLUSH_WORD(); PUSH_OPERATOR(TOKEN_INPUT); continue; }
        if (ch == '>') {
            FLUSH_WORD();
            if (line[i + 1] == '>') { PUSH_OPERATOR(TOKEN_APPEND); i++; }
            else PUSH_OPERATOR(TOKEN_OUTPUT);
            continue;
        }
        if (ch == ';') {
            FLUSH_WORD();
            if (line[i + 1] == ';') { PUSH_OPERATOR(TOKEN_SEMICOLON); i++; }
            else PUSH_OPERATOR(TOKEN_SEMICOLON);
            continue;
        }
        if (ch == '&') {
            FLUSH_WORD();
            if (line[i + 1] == '&') { PUSH_OPERATOR(TOKEN_AND); i++; }
            else PUSH_OPERATOR(TOKEN_BACKGROUND);
            continue;
        }
        if (word_length + 1 < sizeof(word)) word[word_length++] = ch;
    }
    FLUSH_WORD();
    *token_count = count;
    #undef FLUSH_WORD
    #undef PUSH_OPERATOR
    return 0;
}

static int store_argument(char *arena, size_t arena_capacity, size_t *used,
                          ParsedCommand *command, const char *value) {
    size_t length = strlen(value) + 1;
    if (*used + length > arena_capacity || command->argc + 1 >= MAX_ARGS) return -1;
    char *stored = arena + *used;
    memcpy(stored, value, length);
    *used += length;
    command->argv[command->argc++] = stored;
    command->argv[command->argc] = NULL;
    return 0;
}

static int parse_pipeline(Shell *shell, ShellToken *tokens, size_t start, size_t end,
                          ParsedCommand *commands, size_t *command_count,
                          char *arena, size_t arena_capacity, int *background) {
    memset(commands, 0, sizeof(*commands) * MAX_PIPE_COMMANDS);
    *command_count = 1;
    *background = 0;
    size_t arena_used = 0;
    TokenType redirection = TOKEN_WORD;
    for (size_t i = start; i < end; i++) {
        ShellToken *token = &tokens[i];
        ParsedCommand *command = &commands[*command_count - 1];
        if (token->type == TOKEN_PIPE) {
            if (redirection != TOKEN_WORD || command->argc == 0 ||
                *command_count >= MAX_PIPE_COMMANDS) return -1;
            (*command_count)++;
            continue;
        }
        if (token->type == TOKEN_INPUT || token->type == TOKEN_OUTPUT ||
            token->type == TOKEN_APPEND || token->type == TOKEN_ERROR_OUTPUT) {
            if (redirection != TOKEN_WORD) return -1;
            redirection = token->type;
            continue;
        }
        if (token->type == TOKEN_BACKGROUND) {
            if (i + 1 != end || redirection != TOKEN_WORD) return -1;
            *background = 1;
            continue;
        }
        if (token->type != TOKEN_WORD) return -1;

        wordexp_t words;
        memset(&words, 0, sizeof(words));
        int error = expand_word(shell, token->text, &words);
        if (error != 0) {
            const char *message = "invalid command syntax";
            if (error == WRDE_BADCHAR) message = "unsupported shell operator";
            else if (error == WRDE_BADVAL) message = "could not expand shell value";
            else if (error == WRDE_CMDSUB) message = "command substitution failed";
            else if (error == WRDE_NOSPACE) message = "not enough memory to expand word";
            dprintf(shell->output_fd, "terminator: parse error: %s\n", message);
            return -1;
        }
        if (redirection != TOKEN_WORD) {
            if (words.we_wordc != 1) {
                wordfree(&words);
                dprintf(shell->output_fd, "terminator: redirection needs one file name\n");
                return -1;
            }
            size_t length = strlen(words.we_wordv[0]) + 1;
            if (arena_used + length > arena_capacity) {
                wordfree(&words);
                return -1;
            }
            char *path = arena + arena_used;
            memcpy(path, words.we_wordv[0], length);
            arena_used += length;
            if (redirection == TOKEN_INPUT) command->input_path = path;
            else if (redirection == TOKEN_ERROR_OUTPUT) command->error_path = path;
            else {
                command->output_path = path;
                command->append_output = redirection == TOKEN_APPEND;
            }
            redirection = TOKEN_WORD;
        } else {
            for (size_t w = 0; w < words.we_wordc; w++) {
                if (store_argument(arena, arena_capacity, &arena_used, command,
                                   words.we_wordv[w]) < 0) {
                    wordfree(&words);
                    dprintf(shell->output_fd, "terminator: too many command arguments\n");
                    return -1;
                }
            }
        }
        wordfree(&words);
    }
    if (redirection != TOKEN_WORD || commands[*command_count - 1].argc == 0) return -1;
    for (size_t i = 0; i < *command_count; i++)
        commands[i].argv[commands[i].argc] = NULL;
    return 0;
}

static int execute_pipeline(Shell *shell, ParsedCommand *commands, size_t command_count,
                            int background, const char *raw_command) {
    if (command_count == 1 && !commands[0].input_path && !commands[0].output_path &&
        !commands[0].error_path)
        return execute_external(shell, commands[0].argv, background, raw_command);
    if (background) refresh_jobs(shell);
    if (background && shell->job_count >= MAX_JOBS) {
        dprintf(shell->output_fd, "terminator: jobs: job table is full\n");
        return 1;
    }
    int pipes[MAX_PIPE_COMMANDS - 1][2];
    for (size_t i = 0; i + 1 < command_count; i++) {
        if (pipe(pipes[i]) < 0) {
            dprintf(shell->output_fd, "terminator: pipe: %s\n", strerror(errno));
            for (size_t j = 0; j < i; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            return 1;
        }
    }
    pid_t pids[MAX_PIPE_COMMANDS];
    size_t spawned = 0;
    pid_t pgid = 0;
    for (size_t i = 0; i < command_count; i++) {
        pid_t child = fork();
        if (child == 0) {
            if (shell->process_group_override > 0) setpgid(0, shell->process_group_override);
            else if (pgid == 0) setpgid(0, 0);
            else setpgid(0, pgid);
            close_inherited_gui_fds();
            if (chdir(shell->cwd) < 0) _exit(126);
            int input_fd = i > 0 ? pipes[i - 1][0] : -1;
            int output_fd = i + 1 < command_count ? pipes[i][1] : shell->output_fd;
            int opened_input = -1, opened_output = -1, opened_error = -1;
            if (commands[i].input_path) {
                opened_input = open(commands[i].input_path, O_RDONLY);
                if (opened_input < 0) {
                    dprintf(shell->output_fd, "terminator: %s: %s\n",
                            commands[i].input_path, strerror(errno));
                    _exit(1);
                }
                input_fd = opened_input;
            } else if (i == 0) {
                opened_input = open("/dev/null", O_RDONLY);
                input_fd = opened_input;
            }
            if (commands[i].output_path) {
                int flags = O_CREAT | O_WRONLY |
                    (commands[i].append_output ? O_APPEND : O_TRUNC);
                opened_output = open(commands[i].output_path, flags, 0666);
                if (opened_output < 0) {
                    dprintf(shell->output_fd, "terminator: %s: %s\n",
                            commands[i].output_path, strerror(errno));
                    _exit(1);
                }
                output_fd = opened_output;
            }
            if (commands[i].error_path) {
                opened_error = open(commands[i].error_path,
                                    O_CREAT | O_WRONLY | O_TRUNC, 0666);
                if (opened_error < 0) {
                    dprintf(shell->output_fd, "terminator: %s: %s\n",
                            commands[i].error_path, strerror(errno));
                    _exit(1);
                }
            }
            if (input_fd >= 0) dup2(input_fd, STDIN_FILENO);
            if (output_fd >= 0) dup2(output_fd, STDOUT_FILENO);
            if (commands[i].error_path) dup2(opened_error, STDERR_FILENO);
            else if (output_fd >= 0) dup2(output_fd, STDERR_FILENO);
            for (size_t p = 0; p + 1 < command_count; p++) {
                close(pipes[p][0]); close(pipes[p][1]);
            }
            if (opened_input >= 0) close(opened_input);
            if (opened_output >= 0) close(opened_output);
            if (opened_error >= 0) close(opened_error);
            close(shell->output_fd);
            execvp(commands[i].argv[0], commands[i].argv);
            dprintf(STDERR_FILENO, "terminator: %s: command not found\n", commands[i].argv[0]);
            _exit(127);
        }
        if (child < 0) {
            dprintf(shell->output_fd, "terminator: fork: %s\n", strerror(errno));
            break;
        }
        if (pgid == 0) pgid = shell->process_group_override > 0 ?
            shell->process_group_override : child;
        setpgid(child, pgid);
        pids[spawned++] = child;
    }
    for (size_t i = 0; i + 1 < command_count; i++) {
        close(pipes[i][0]); close(pipes[i][1]);
    }
    if (spawned == 0) return 1;
    if (background) {
        int id = add_job_group(shell, pgid, pids, spawned, 1, raw_command);
        if (id < 0) {
            kill(-pgid, SIGTERM);
            dprintf(shell->output_fd, "terminator: jobs: job table is full\n");
            return 1;
        }
        dprintf(shell->output_fd, "[%d] %ld\n", id, (long)pgid);
        return 0;
    }
    set_foreground(shell, pgid);
    int final_status = 0;
    for (size_t i = 0; i < spawned; i++) {
        int status = 0;
        pid_t result;
        do { result = waitpid(pids[i], &status, WUNTRACED); }
        while (result < 0 && errno == EINTR);
        if (result < 0) { final_status = 1; continue; }
        if (WIFSTOPPED(status)) {
            set_foreground(shell, 0);
            int id = add_job_group(shell, pgid, pids, spawned, 0, raw_command);
            dprintf(shell->output_fd, "[%d]+ Stopped %s\n", id, raw_command);
            return 128 + WSTOPSIG(status);
        }
        if (WIFEXITED(status)) final_status = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) final_status = 128 + WTERMSIG(status);
    }
    set_foreground(shell, 0);
    return final_status;
}

static int execute_external(Shell *shell, char **argv, int background, const char *command) {
    if (background) refresh_jobs(shell);
    if (background && shell->job_count >= MAX_JOBS) {
        dprintf(shell->output_fd, "terminator: jobs: job table is full\n");
        return 1;
    }
    pid_t child = fork();
    if (child == 0) {
        if (shell->process_group_override > 0) setpgid(0, shell->process_group_override);
        else setpgid(0, 0);
        close_inherited_gui_fds();
        if (chdir(shell->cwd) < 0) {
            dprintf(shell->output_fd, "terminator: chdir: %s\n", strerror(errno));
            _exit(126);
        }
        dup2(shell->output_fd, STDOUT_FILENO);
        dup2(shell->output_fd, STDERR_FILENO);
        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) { dup2(null_fd, STDIN_FILENO); close(null_fd); }
        close(shell->output_fd);
        execvp(argv[0], argv);
        dprintf(STDOUT_FILENO, "terminator: %s: command not found\n", argv[0]);
        _exit(127);
    }
    if (child < 0) {
        dprintf(shell->output_fd, "terminator: fork: %s\n", strerror(errno));
        return 1;
    }
    pid_t command_group = shell->process_group_override > 0 ?
        shell->process_group_override : child;
    setpgid(child, command_group);
    if (background) {
        int id = add_job(shell, command_group, child, 1, command);
        if (id < 0) {
            kill(-command_group, SIGTERM);
            dprintf(shell->output_fd, "terminator: jobs: job table is full\n");
            return 1;
        }
        dprintf(shell->output_fd, "[%d] %ld\n", id, (long)child);
        return 0;
    }
    set_foreground(shell, command_group);
    int status = 0;
    pid_t result;
    do { result = waitpid(child, &status, WUNTRACED); }
    while (result < 0 && errno == EINTR);
    set_foreground(shell, 0);
    if (result < 0) {
        dprintf(shell->output_fd, "terminator: wait: %s\n", strerror(errno));
        return 1;
    }
    if (WIFSTOPPED(status)) {
        int id = add_job(shell, command_group, child, 0, command);
        dprintf(shell->output_fd, "[%d]+ Stopped %s\n", id, command);
        return 128 + WSTOPSIG(status);
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 0;
}

static int is_variable_name(const char *name, size_t length) {
    if (length == 0 || length >= 64 ||
        !(isalpha((unsigned char)name[0]) || name[0] == '_')) return 0;
    for (size_t i = 1; i < length; i++)
        if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) return 0;
    return 1;
}

static void set_shell_variable(Shell *shell, const char *name, const char *value) {
    size_t index = 0;
    while (index < shell->variable_count && strcmp(shell->variables[index].name, name) != 0)
        index++;
    if (index == shell->variable_count) {
        if (shell->variable_count == MAX_VARIABLES) return;
        shell->variable_count++;
    }
    snprintf(shell->variables[index].name, sizeof(shell->variables[index].name), "%s", name);
    snprintf(shell->variables[index].value, sizeof(shell->variables[index].value), "%s", value);
}

static int run_test_builtin(Shell *shell, char **argv, size_t argc) {
    size_t first = 1;
    size_t count = argc > 0 ? argc - 1 : 0;
    if (strcmp(argv[0], "[") == 0) {
        if (count == 0 || strcmp(argv[argc - 1], "]") != 0) return 2;
        count--;
    }
    if (count == 0) return 1;
    if (count == 1) return argv[first][0] != '\0' ? 0 : 1;
    if (count == 2) {
        struct stat info;
        char path[PATH_MAX];
        if (strcmp(argv[first], "-f") != 0 && strcmp(argv[first], "-d") != 0) return 2;
        if (resolve_path(shell, argv[first + 1], path) < 0 || stat(path, &info) < 0) return 1;
        if (strcmp(argv[first], "-f") == 0) return S_ISREG(info.st_mode) ? 0 : 1;
        return S_ISDIR(info.st_mode) ? 0 : 1;
    }
    if (count != 3) return 2;
    const char *left = argv[first];
    const char *op = argv[first + 1];
    const char *right = argv[first + 2];
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) return strcmp(left, right) == 0 ? 0 : 1;
    if (strcmp(op, "!=") == 0) return strcmp(left, right) != 0 ? 0 : 1;
    char *left_end = NULL, *right_end = NULL;
    errno = 0;
    long a = strtol(left, &left_end, 10);
    long b = strtol(right, &right_end, 10);
    if (errno || !left_end || left_end == left || *left_end ||
        !right_end || right_end == right || *right_end) return 2;
    if (strcmp(op, "-eq") == 0) return a == b ? 0 : 1;
    if (strcmp(op, "-gt") == 0) return a > b ? 0 : 1;
    if (strcmp(op, "-ne") == 0) return a != b ? 0 : 1;
    if (strcmp(op, "-lt") == 0) return a < b ? 0 : 1;
    if (strcmp(op, "-ge") == 0) return a >= b ? 0 : 1;
    if (strcmp(op, "-le") == 0) return a <= b ? 0 : 1;
    return 2;
}

static int execute_builtin(Shell *shell, char **argv, size_t argc, int *handled) {
    *handled = 1;
    if (strcmp(argv[0], "[") == 0 || strcmp(argv[0], "test") == 0) {
        shell->last_status = run_test_builtin(shell, argv, argc);
    } else if (argc == 1) {
        char *equals = strchr(argv[0], '=');
        if (equals && is_variable_name(argv[0], (size_t)(equals - argv[0]))) {
            *equals = '\0';
            set_shell_variable(shell, argv[0], equals + 1);
            shell->last_status = 0;
            return 0;
        }
    }
    if (strcmp(argv[0], "cd") == 0) {
        const char *target = argc > 1 ? argv[1] : getenv("HOME");
        char resolved[PATH_MAX];
        struct stat info;
        const char *reason = NULL;
        if (!target) target = shell->cwd;
        if (resolve_path(shell, target, resolved) < 0) reason = strerror(errno);
        else if (stat(resolved, &info) < 0) reason = strerror(errno);
        else if (!S_ISDIR(info.st_mode)) reason = "not a directory";
        if (reason) {
            dprintf(shell->output_fd, "terminator: cd: %s: %s\n", target, reason);
            shell->last_status = 1;
        } else {
            strncpy(shell->cwd, resolved, sizeof(shell->cwd) - 1);
            shell->cwd[sizeof(shell->cwd) - 1] = '\0';
            control_lock(shell->control);
            snprintf(shell->control->tab_cwd[shell->tab_index],
                     sizeof(shell->control->tab_cwd[shell->tab_index]), "%s", resolved);
            sem_post(&shell->control->mutex);
            shell->last_status = 0;
        }
    } else if (strcmp(argv[0], "exit") == 0) {
        char *end = NULL;
        long status = argc > 1 ? strtol(argv[1], &end, 10) : shell->last_status;
        if (argc > 1 && (!end || *end)) {
            dprintf(shell->output_fd, "terminator: exit: numeric argument required\n");
            status = 2;
        }
        shell->last_status = (int)(status & 255);
        return 1;
    } else if (strcmp(argv[0], "history") == 0) {
        history_print(shell);
        shell->last_status = 0;
    } else if (strcmp(argv[0], "jobs") == 0) {
        print_jobs(shell);
        shell->last_status = 0;
    } else if (strcmp(argv[0], "bg") == 0 || strcmp(argv[0], "fg") == 0) {
        size_t index = find_job(shell, argc > 1 ? argv[1] : NULL);
        if (index == (size_t)-1) {
            dprintf(shell->output_fd, "terminator: %s: no such job\n", argv[0]);
            shell->last_status = 1;
        } else if (strcmp(argv[0], "bg") == 0) {
            if (kill(-shell->jobs[index].pgid, SIGCONT) < 0) {
                dprintf(shell->output_fd, "terminator: bg: %s\n", strerror(errno));
                shell->last_status = 1;
            } else {
                shell->jobs[index].running = 1;
                dprintf(shell->output_fd, "[%d]+ %s &\n", shell->jobs[index].id,
                        shell->jobs[index].command);
                shell->last_status = 0;
            }
        } else {
            if (kill(-shell->jobs[index].pgid, SIGCONT) < 0)
                dprintf(shell->output_fd, "terminator: fg: %s\n", strerror(errno));
            shell->jobs[index].running = 1;
            shell->last_status = wait_for_job(shell, index);
        }
    } else if (strcmp(argv[0], "source") == 0 ||
               (strcmp(argv[0], ".") == 0)) {
        if (argc < 2) {
            dprintf(shell->output_fd, "usage: source FILE [ARG ...]\n");
            shell->last_status = 2;
        } else if (source_file(shell, argv[1], argc > 2 ? argv + 2 : NULL,
                               argc > 2 ? argc - 2 : 0)) return 1;
    } else if (strcmp(argv[0], "bash") == 0 && argc > 1) {
        if (source_file(shell, argv[1], argc > 2 ? argv + 2 : NULL,
                        argc > 2 ? argc - 2 : 0)) return 1;
    } else if (strcmp(argv[0], "multiWatch") == 0) {
        char text[INPUT_SIZE];
        size_t used = 0;
        for (size_t i = 1; i < argc && used + strlen(argv[i]) + 2 < sizeof(text); i++) {
            if (i > 1) text[used++] = ',';
            memcpy(text + used, argv[i], strlen(argv[i]));
            used += strlen(argv[i]);
            text[used] = '\0';
        }
        shell->last_status = run_multiwatch_once(shell, text);
    } else {
        *handled = 0;
    }
    return 0;
}

static int is_bash_script(Shell *shell, const char *name) {
    char path[PATH_MAX];
    if (resolve_path(shell, name, path) < 0) return 0;
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    char first_line[256] = "";
    if (!fgets(first_line, sizeof(first_line), file)) first_line[0] = '\0';
    fclose(file);
    size_t length = strlen(name);
    return (length >= 3 && strcmp(name + length - 3, ".sh") == 0) ||
           strncmp(first_line, "#!/bin/bash", 11) == 0 ||
           strncmp(first_line, "#!/usr/bin/env bash", 19) == 0;
}

static int execute_segment(Shell *shell, ShellToken *tokens, size_t start, size_t end,
                           const char *raw) {
    ParsedCommand commands[MAX_PIPE_COMMANDS];
    size_t command_count = 0;
    char arena[INPUT_SIZE * 8];
    int background = 0;
    if (parse_pipeline(shell, tokens, start, end, commands, &command_count,
                       arena, sizeof(arena), &background) < 0) {
        dprintf(shell->output_fd, "terminator: parse error: invalid pipeline or redirection\n");
        shell->last_status = 2;
        return 0;
    }
    if (command_count == 1 && !commands[0].input_path && !commands[0].output_path &&
        !commands[0].error_path) {
        int handled = 0;
        int should_exit = execute_builtin(shell, commands[0].argv,
                                          commands[0].argc, &handled);
        if (handled) return should_exit;
        if (is_bash_script(shell, commands[0].argv[0])) {
            shell->last_status = 0;
            if (source_file(shell, commands[0].argv[0],
                            commands[0].argc > 1 ? commands[0].argv + 1 : NULL,
                            commands[0].argc > 1 ? commands[0].argc - 1 : 0)) return 1;
            return 0;
        }
    }
    shell->last_status = execute_pipeline(shell, commands, command_count,
                                          background, raw);
    return 0;
}

static int execute_line(Shell *shell, const char *line) {
    char source[INPUT_SIZE];
    size_t length = strnlen(line, sizeof(source) - 1);
    memcpy(source, line, length);
    source[length] = '\0';
    char *trimmed = source;
    while (isspace((unsigned char)*trimmed)) trimmed++;
    if (*trimmed == '\0') return 0;
    if (!shell->suppress_history) history_add(shell->history, trimmed);

    char expanded_multi[INPUT_SIZE * 4];
    expand_shell_references(shell, trimmed, expanded_multi, sizeof(expanded_multi));
    if (strncmp(expanded_multi, "multiWatch", 10) == 0 &&
        (expanded_multi[10] == '\0' || isspace((unsigned char)expanded_multi[10]) ||
         expanded_multi[10] == '[')) {
        shell->last_status = run_multiwatch_once(shell, expanded_multi + 10);
        return 0;
    }

    ShellToken tokens[INPUT_SIZE / 2 + 2];
    char token_storage[INPUT_SIZE * 2];
    size_t token_count = 0;
    if (tokenize_shell_line(trimmed, tokens, sizeof(tokens) / sizeof(tokens[0]),
                            &token_count, token_storage, sizeof(token_storage)) < 0) {
        dprintf(shell->output_fd, "terminator: parse error: command is too long or has too many tokens\n");
        shell->last_status = 2;
        return 0;
    }
    size_t segment_start = 0;
    TokenType next_condition = TOKEN_SEMICOLON;
    int should_run = 1;
    for (size_t i = 0; i <= token_count; i++) {
        TokenType separator = i < token_count ? tokens[i].type : TOKEN_SEMICOLON;
        if (i < token_count && separator != TOKEN_SEMICOLON &&
            separator != TOKEN_AND && separator != TOKEN_OR) continue;
        if (should_run && i > segment_start) {
            char raw_segment[INPUT_SIZE];
            size_t raw_length = 0;
            for (size_t j = segment_start; j < i && raw_length + 2 < sizeof(raw_segment); j++) {
                if (tokens[j].type == TOKEN_WORD) {
                    size_t word_length = strlen(tokens[j].text);
                    memcpy(raw_segment + raw_length, tokens[j].text, word_length);
                    raw_length += word_length;
                    raw_segment[raw_length++] = ' ';
                }
            }
            raw_segment[raw_length] = '\0';
            if (execute_segment(shell, tokens, segment_start, i,
                                raw_length ? raw_segment : trimmed)) return 1;
        }
        if (i < token_count) {
            next_condition = separator;
            should_run = next_condition == TOKEN_SEMICOLON ||
                (next_condition == TOKEN_AND && shell->last_status == 0) ||
                (next_condition == TOKEN_OR && shell->last_status != 0);
            segment_start = i + 1;
        }
    }
    return 0;
}

static void shell_loop(int command_fd, int output_fd, SharedHistory *history,
                       SharedControl *control, int tab_index,
                       const char *working_directory) {
    Shell shell;
    memset(&shell, 0, sizeof(shell));
    shell.command_fd = command_fd;
    shell.output_fd = output_fd;
    shell.history = history;
    shell.control = control;
    shell.tab_index = tab_index;
    shell.next_job_id = 1;
    strncpy(shell.cwd, working_directory, sizeof(shell.cwd) - 1);
    shell.cwd[sizeof(shell.cwd) - 1] = '\0';
    control_lock(shell.control);
    snprintf(shell.control->tab_cwd[tab_index], sizeof(shell.control->tab_cwd[tab_index]),
             "%s", shell.cwd);
    sem_post(&shell.control->mutex);

    char input[INPUT_SIZE];
    size_t used = 0;
    int overflow = 0;
    int done = 0;
    static const char prompt[] = "user@terminator> ";
    static const char continuation[] = "> ";
    write_all(output_fd, prompt, sizeof(prompt) - 1);
    while (!done) {
        char chunk[512];
        ssize_t count = read(command_fd, chunk, sizeof(chunk));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        for (ssize_t i = 0; i < count; i++) {
            if (chunk[i] != '\n') {
                if (used + 1 < sizeof(input)) input[used++] = chunk[i];
                else overflow = 1;
                continue;
            }
            if (overflow) {
                dprintf(output_fd, "terminator: command exceeds the %d byte input limit\n",
                        INPUT_SIZE - 1);
                used = 0;
                overflow = 0;
                write_all(output_fd, prompt, sizeof(prompt) - 1);
                continue;
            }
            input[used] = '\0';
            char quote = 0;
            int escaped = 0, parenthesis_depth = 0, backtick = 0;
            for (size_t j = 0; j < used; j++) {
                char ch = input[j];
                if (escaped) { escaped = 0; continue; }
                if (ch == '\\' && quote != '\'') { escaped = 1; continue; }
                if (ch == '`' && quote != '\'') { backtick = !backtick; continue; }
                if (quote) {
                    if (ch == quote) quote = 0;
                    else if (quote == '"' && ch == '$' && input[j + 1] == '(')
                        parenthesis_depth++;
                    else if (parenthesis_depth > 0 && ch == ')') parenthesis_depth--;
                    continue;
                }
                if (ch == '\'' || ch == '"') { quote = ch; continue; }
                if (ch == '$' && input[j + 1] == '(') { parenthesis_depth++; j++; continue; }
                if (parenthesis_depth > 0 && ch == '(') parenthesis_depth++;
                else if (parenthesis_depth > 0 && ch == ')') parenthesis_depth--;
            }
            if (quote || escaped || parenthesis_depth > 0 || backtick) {
                if (used + 1 < sizeof(input)) {
                    input[used++] = '\n';
                    write_all(output_fd, continuation, sizeof(continuation) - 1);
                    continue;
                }
                dprintf(output_fd, "terminator: incomplete command exceeds the %d byte input limit\n",
                        INPUT_SIZE - 1);
                used = 0;
                write_all(output_fd, prompt, sizeof(prompt) - 1);
                continue;
            }
            done = execute_line(&shell, input);
            used = 0;
            if (!done) write_all(output_fd, prompt, sizeof(prompt) - 1);
            if (done) break;
        }
    }
    close(command_fd);
    close(output_fd);
}

static void *shell_thread(void *opaque) {
    ShellArgs *args = opaque;
    int command_fd = args->command_fd;
    int output_fd = args->output_fd;
    SharedHistory *history = args->history;
    SharedControl *control = args->control;
    int tab_index = args->tab_index;
    char cwd[PATH_MAX];
    memcpy(cwd, args->cwd, sizeof(cwd));
    free(args);
    shell_loop(command_fd, output_fd, history, control, tab_index, cwd);
    return NULL;
}

static int start_tab(int slot) {
    int to_shell[2] = {-1, -1};
    int from_shell[2] = {-1, -1};
    if (pipe(to_shell) < 0 || pipe(from_shell) < 0) {
        perror("terminator: pipe");
        if (to_shell[0] >= 0) close(to_shell[0]);
        if (to_shell[1] >= 0) close(to_shell[1]);
        if (from_shell[0] >= 0) close(from_shell[0]);
        if (from_shell[1] >= 0) close(from_shell[1]);
        return -1;
    }
    Tab *tab = &tabs[slot];
    memset(tab, 0, sizeof(*tab));
    if (selection_tab == slot) selection_tab = -1;
    tab->active = 1;
    tab->id = next_tab_id++;
    tab->history_index = -1;
    tab->cursor = 0;
    tab->input_fd = to_shell[1];
    tab->output_fd = from_shell[0];
    tab->pid = -1;
    tab->process_mode = process_mode;
    fcntl(to_shell[0], F_SETFD, FD_CLOEXEC);
    fcntl(to_shell[1], F_SETFD, FD_CLOEXEC);
    fcntl(from_shell[0], F_SETFD, FD_CLOEXEC);
    fcntl(from_shell[1], F_SETFD, FD_CLOEXEC);

    if (process_mode) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("terminator: fork");
            close(to_shell[0]); close(to_shell[1]);
            close(from_shell[0]); close(from_shell[1]);
            memset(tab, 0, sizeof(*tab));
            return -1;
        }
        if (pid == 0) {
            for (int i = 0; i < MAX_TABS; i++) {
                if (i == slot || !tabs[i].active) continue;
                close(tabs[i].input_fd);
                close(tabs[i].output_fd);
            }
            close(to_shell[1]);
            close(from_shell[0]);
            shell_loop(to_shell[0], from_shell[1], shared_history,
                       shared_control, slot, initial_cwd);
            _exit(0);
        }
        tab->pid = pid;
        close(to_shell[0]);
        close(from_shell[1]);
    } else {
        ShellArgs *args = malloc(sizeof(*args));
        if (!args) {
            close(to_shell[0]); close(to_shell[1]);
            close(from_shell[0]); close(from_shell[1]);
            memset(tab, 0, sizeof(*tab));
            return -1;
        }
        args->command_fd = to_shell[0];
        args->output_fd = from_shell[1];
        args->history = shared_history;
        args->control = shared_control;
        args->tab_index = slot;
        memcpy(args->cwd, initial_cwd, sizeof(args->cwd));
        if (pthread_create(&tab->thread, NULL, shell_thread, args) != 0) {
            free(args);
            close(to_shell[0]); close(to_shell[1]);
            close(from_shell[0]); close(from_shell[1]);
            memset(tab, 0, sizeof(*tab));
            return -1;
        }
        pthread_detach(tab->thread);
        /* Threads share the process file table: the worker closes these ends
         * when its shell loop exits, so the GUI must not close them here. */
    }
    current_tab = slot;
    return 0;
}

static int active_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_TABS; i++) count += tabs[i].active;
    return count;
}

static int add_tab(void) {
    for (int i = 0; i < MAX_TABS; i++) {
        if (!tabs[i].active && start_tab(i) == 0) {
            draw_window();
            return i;
        }
    }
    return -1;
}

static int next_active(int start, int direction) {
    for (int step = 1; step <= MAX_TABS; step++) {
        int index = (start + direction * step + MAX_TABS * 2) % MAX_TABS;
        if (tabs[index].active) return index;
    }
    return -1;
}

static void close_tab(int index) {
    if (index < 0 || index >= MAX_TABS || !tabs[index].active) return;
    if (selection_tab == index) selection_tab = -1;
    Tab *tab = &tabs[index];
    pid_t foreground = 0;
    pid_t watch_pids[MAX_PIPE_COMMANDS];
    size_t watch_count = 0;
    control_lock(shared_control);
    shared_control->watch_cancel[index] = 1;
    foreground = shared_control->foreground[index];
    watch_count = shared_control->watch_pid_count[index];
    if (watch_count > MAX_PIPE_COMMANDS) watch_count = MAX_PIPE_COMMANDS;
    memcpy(watch_pids, shared_control->watch_pids[index], watch_count * sizeof(pid_t));
    sem_post(&shared_control->mutex);
    if (foreground > 0) kill(-foreground, SIGTERM);
    for (size_t i = 0; i < watch_count; i++) if (watch_pids[i] > 0) kill(-watch_pids[i], SIGTERM);
    tab->active = 0;
    if (tab->input_fd >= 0) close(tab->input_fd);
    if (tab->output_fd >= 0) close(tab->output_fd);
    if (tab->process_mode && tab->pid > 0) {
        /* Closing the command pipe asks the worker to exit; reap when available. */
        waitpid(tab->pid, NULL, WNOHANG);
    }
    if (current_tab == index) {
        current_tab = next_active(index, 1);
    }
    draw_window();
}

static int text_width(const char *text, size_t length) {
    if (length > 32767) length = 32767;
    return XTextWidth(font, text, (int)length);
}

static const char *action_name(KeyAction action) {
    static const char *names[] = {"new-tab", "close-tab", "next-tab", "previous-tab",
                                  "copy", "paste", "search", "clear", "eof"};
    return action >= ACTION_NEW_TAB && action <= ACTION_EOF ? names[action] : "";
}

static int parse_action(const char *name, KeyAction *action) {
    for (int i = ACTION_NEW_TAB; i <= ACTION_EOF; i++) {
        if (strcmp(name, action_name((KeyAction)i)) == 0) {
            *action = (KeyAction)i;
            return 1;
        }
    }
    return 0;
}

static int parse_binding_key(char *text, KeySym *key, unsigned int *modifiers) {
    *modifiers = 0;
    char *save = NULL;
    char *part = strtok_r(text, "+", &save);
    char *key_name = NULL;
    while (part) {
        if (strcasecmp(part, "ctrl") == 0 || strcasecmp(part, "control") == 0)
            *modifiers |= ControlMask;
        else if (strcasecmp(part, "shift") == 0) *modifiers |= ShiftMask;
        else if (strcasecmp(part, "alt") == 0) *modifiers |= Mod1Mask;
        else if (strcasecmp(part, "super") == 0 || strcasecmp(part, "meta") == 0)
            *modifiers |= Mod4Mask;
        else if (!key_name) key_name = part;
        else return 0;
        part = strtok_r(NULL, "+", &save);
    }
    if (!key_name || !*key_name) return 0;
    *key = XStringToKeysym(key_name);
    return *key != NoSymbol;
}

static int add_binding(KeyAction action, KeySym key, unsigned int modifiers) {
    if (modifiers == ControlMask && (key == XK_c || key == XK_C ||
                                     key == XK_z || key == XK_Z)) return 0;
    for (size_t i = 0; i < keybinding_count; i++) {
        if (keybindings[i].key == key && keybindings[i].modifiers == modifiers &&
            keybindings[i].action != action) return 0;
    }
    for (size_t i = 0; i < keybinding_count;) {
        if (keybindings[i].action == action) {
            memmove(&keybindings[i], &keybindings[i + 1],
                    (keybinding_count - i - 1) * sizeof(keybindings[0]));
            keybinding_count--;
        } else i++;
    }
    if (keybinding_count >= MAX_KEYBINDINGS) return 0;
    keybindings[keybinding_count++] = (KeyBinding){key, modifiers, action};
    return 1;
}

static void load_keybindings(void) {
    if (keybindings_loaded) return;
    keybindings_loaded = 1;
    add_binding(ACTION_NEW_TAB, XK_T, ControlMask | ShiftMask);
    add_binding(ACTION_CLOSE_TAB, XK_W, ControlMask | ShiftMask);
    add_binding(ACTION_NEXT_TAB, XK_Page_Down, ControlMask);
    add_binding(ACTION_PREVIOUS_TAB, XK_Page_Up, ControlMask);
    add_binding(ACTION_COPY, XK_C, ControlMask | ShiftMask);
    add_binding(ACTION_PASTE, XK_V, ControlMask | ShiftMask);
    add_binding(ACTION_SEARCH, XK_r, ControlMask);
    add_binding(ACTION_CLEAR, XK_l, ControlMask);
    add_binding(ACTION_EOF, XK_d, ControlMask);

    const char *home = getenv("HOME");
    if (!home) return;
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/.terminatorrc", home) >= (int)sizeof(path)) return;
    FILE *file = fopen(path, "r");
    if (!file) return;
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, file) >= 0) {
        char action_text[64], binding_text[128], extra[8];
        /* The documented format is: bind ACTION Control+Shift+Key. */
        if (sscanf(line, " bind %63s %127s %7s", action_text, binding_text, extra) != 2) continue;
        KeyAction action;
        KeySym key;
        unsigned int modifiers;
        if (!parse_action(action_text, &action) ||
            !parse_binding_key(binding_text, &key, &modifiers) ||
            !add_binding(action, key, modifiers))
            fprintf(stderr, "terminator: ignored invalid or conflicting key binding in %s\n", path);
    }
    free(line);
    fclose(file);
}

static int bound_action(KeySym key, unsigned int state, KeyAction *action) {
    unsigned int modifiers = state & (ControlMask | ShiftMask | Mod1Mask | Mod4Mask);
    for (size_t i = 0; i < keybinding_count; i++) {
        if (keybindings[i].key == key && keybindings[i].modifiers == modifiers) {
            *action = keybindings[i].action;
            return 1;
        }
    }
    return 0;
}

static void copy_bounded(char *destination, size_t capacity, const char *source) {
    if (capacity == 0) return;
    size_t length = strnlen(source, capacity - 1);
    memmove(destination, source, length);
    destination[length] = '\0';
}

static void set_input(Tab *tab, const char *text) {
    copy_bounded(tab->input, sizeof(tab->input), text);
    tab->input_length = strlen(tab->input);
    tab->cursor = tab->input_length;
    tab->completion_pending = 0;
    tab->input_selection_active = 0;
}

static void insert_input(Tab *tab, const char *text, size_t length) {
    if (tab->input_selection_active) {
        size_t start = tab->input_selection_anchor < tab->cursor ?
            tab->input_selection_anchor : tab->cursor;
        size_t end = tab->input_selection_anchor > tab->cursor ?
            tab->input_selection_anchor : tab->cursor;
        erase_input_range(tab, start, end);
        tab->input_selection_active = 0;
    }
    if (length == 0 || tab->input_length + length >= sizeof(tab->input)) return;
    memmove(tab->input + tab->cursor + length, tab->input + tab->cursor,
            tab->input_length - tab->cursor + 1);
    memcpy(tab->input + tab->cursor, text, length);
    tab->input_length += length;
    tab->cursor += length;
    tab->completion_pending = 0;
    tab->completion_choice_length = 0;
    tab->completion_choice[0] = '\0';
}

static void erase_input_range(Tab *tab, size_t start, size_t end) {
    if (start > end || end > tab->input_length) return;
    memmove(tab->input + start, tab->input + end, tab->input_length - end + 1);
    tab->input_length -= end - start;
    tab->cursor = start;
    tab->completion_pending = 0;
    tab->input_selection_active = 0;
}

static int get_history_recent(size_t offset, char *destination, size_t capacity,
                              size_t *history_count) {
    SharedHistory *history = shared_history;
    int found = 0;
    history_lock(history);
    if (history_count) *history_count = history->count;
    if (offset < history->count) {
        size_t index = (history->next + HISTORY_CAPACITY - 1 - offset) % HISTORY_CAPACITY;
        snprintf(destination, capacity, "%s", history->entries[index]);
        found = 1;
    }
    sem_post(&history->mutex);
    return found;
}

static void history_up(Tab *tab) {
    if (tab->history_index < 0) {
        copy_bounded(tab->history_draft, sizeof(tab->history_draft), tab->input);
        if (!get_history_recent(0, tab->input, sizeof(tab->input), NULL)) return;
        tab->history_index = 0;
    } else if (get_history_recent((size_t)tab->history_index + 1,
                                  tab->input, sizeof(tab->input), NULL)) {
        tab->history_index++;
    }
    tab->input_length = strlen(tab->input);
    tab->cursor = tab->input_length;
    tab->input_selection_active = 0;
}

static void history_down(Tab *tab) {
    if (tab->history_index < 0) return;
    if (tab->history_index == 0) {
        tab->history_index = -1;
        set_input(tab, tab->history_draft);
    } else if (get_history_recent((size_t)tab->history_index - 1,
                                  tab->input, sizeof(tab->input), NULL)) {
        tab->history_index--;
        tab->input_length = strlen(tab->input);
        tab->cursor = tab->input_length;
        tab->input_selection_active = 0;
    }
}

static void escape_filename(const char *source, char *destination, size_t capacity) {
    size_t out = 0;
    for (size_t i = 0; source[i] && out + 2 < capacity; i++) {
        if (source[i] == ' ' || source[i] == '\\' || source[i] == '\t')
            destination[out++] = '\\';
        destination[out++] = source[i];
    }
    destination[out] = '\0';
}

static void complete_filename(Tab *tab) {
    size_t start = tab->cursor;
    while (start > 0 && !isspace((unsigned char)tab->input[start - 1])) start--;
    size_t token_length = tab->cursor - start;
    if (token_length >= 256) return;
    char token[256];
    memcpy(token, tab->input + start, token_length);
    token[token_length] = '\0';
    char *slash = strrchr(token, '/');
    char directory[PATH_MAX];
    const char *base = slash ? slash + 1 : token;
    size_t directory_prefix_length = slash ? (size_t)(base - token) : 0;
    char directory_prefix[256] = "";
    if (directory_prefix_length) {
        memcpy(directory_prefix, token, directory_prefix_length);
        directory_prefix[directory_prefix_length] = '\0';
    }
    char cwd[PATH_MAX];
    control_lock(shared_control);
    snprintf(cwd, sizeof(cwd), "%s", shared_control->tab_cwd[current_tab][0] ?
             shared_control->tab_cwd[current_tab] : initial_cwd);
    sem_post(&shared_control->mutex);
    if (slash && token[0] == '/') snprintf(directory, sizeof(directory), "%s", directory_prefix);
    else if (snprintf(directory, sizeof(directory), "%s/%s", cwd,
                      directory_prefix_length ? directory_prefix : ".") >= (int)sizeof(directory)) return;
    DIR *dir = opendir(directory);
    if (!dir) {
        XBell(display, 0);
        return;
    }
    char matches[32][256];
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, base, strlen(base)) != 0) continue;
        if (count < sizeof(matches) / sizeof(matches[0]))
            snprintf(matches[count++], sizeof(matches[0]), "%s", entry->d_name);
    }
    closedir(dir);
    if (count == 0) {
        XBell(display, 0);
        return;
    }
    tab->completion_start = start;
    tab->completion_count = count;
    tab->completion_choice_length = 0;
    tab->completion_choice[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        snprintf(tab->completion_items[i], sizeof(tab->completion_items[i]),
                 "%s%s", directory_prefix, matches[i]);
        char full[PATH_MAX];
        if (snprintf(full, sizeof(full), "%s/%s", directory, matches[i]) < (int)sizeof(full)) {
            struct stat info;
            tab->completion_is_dir[i] = stat(full, &info) == 0 && S_ISDIR(info.st_mode);
        } else tab->completion_is_dir[i] = 0;
    }
    if (count == 1) {
        char raw_replacement[300], replacement[600];
        snprintf(raw_replacement, sizeof(raw_replacement), "%s%s", directory_prefix, matches[0]);
        escape_filename(raw_replacement, replacement, sizeof(replacement));
        erase_input_range(tab, start, tab->cursor);
        insert_input(tab, replacement, strlen(replacement));
        if (tab->completion_is_dir[0]) insert_input(tab, "/", 1);
        else insert_input(tab, " ", 1);
        tab->completion_pending = 0;
        return;
    }
    size_t common = strlen(matches[0]);
    for (size_t i = 1; i < count; i++) {
        size_t j = 0;
        while (j < common && matches[i][j] == matches[0][j]) j++;
        common = j;
    }
    if (common > strlen(base)) {
        char raw_replacement[300], replacement[600];
        snprintf(raw_replacement, sizeof(raw_replacement), "%s%.*s", directory_prefix,
                 (int)common, matches[0]);
        escape_filename(raw_replacement, replacement, sizeof(replacement));
        erase_input_range(tab, start, tab->cursor);
        insert_input(tab, replacement, strlen(replacement));
        tab->completion_start = start;
    }
    tab->completion_pending = 1;
    for (size_t i = 0; i < count; i++) {
        char item_line[320];
        int length = snprintf(item_line, sizeof(item_line), "%zu) %s%s\n", i + 1,
                              directory_prefix, matches[i]);
        if (length > 0) append_text(tab, item_line, (size_t)length);
    }
}

static size_t make_visual_lines(const char *text, size_t length, VisualLine *lines,
                                size_t capacity, int max_width) {
    size_t count = 0;
    size_t pos = 0;
    while (pos < length && count < capacity) {
        if (text[pos] == '\n') {
            lines[count++] = (VisualLine){pos, 0};
            pos++;
            continue;
        }
        size_t line_start = pos;
        int width = 0;
        while (pos < length && text[pos] != '\n') {
            int char_width = text_width(text + pos, 1);
            if (pos > line_start && width + char_width > max_width) break;
            width += char_width;
            pos++;
        }
        /* Keep one wide glyph on its own line rather than stalling. */
        if (pos == line_start) pos++;
        lines[count++] = (VisualLine){line_start, pos - line_start};
        if (pos < length && text[pos] == '\n') pos++;
    }
    if ((length == 0 || text[length - 1] == '\n') && count < capacity) {
        lines[count++] = (VisualLine){length, 0};
    }
    return count;
}

static void draw_window(void) {
    if (!display || current_tab < 0 || !tabs[current_tab].active) return;
    XClearWindow(display, window);
    int baseline = font->ascent + 7;
    int cell_height = font->ascent + font->descent + 5;

    int x = 8;
    int bar_y = 6;
    int tab_height = 28;
    for (int i = 0; i < MAX_TABS; i++) {
        if (!tabs[i].active) continue;
        char label[16];
        snprintf(label, sizeof(label), "%d", i + 1);
        int width = text_width(label, strlen(label)) + 28;
        XSetForeground(display, gc, i == current_tab ? color_tab_active : color_tab);
        XFillRectangle(display, window, gc, x, bar_y, (unsigned)width, (unsigned)tab_height);
        XSetForeground(display, gc, i == current_tab ? color_accent : color_border);
        XDrawRectangle(display, window, gc, x, bar_y, (unsigned)width, (unsigned)tab_height);
        XSetForeground(display, gc, i == current_tab ? color_text : color_muted);
        XDrawString(display, window, gc, x + 12, bar_y + baseline, label, strlen(label));
        int close_x = x + width - 14;
        int close_y = bar_y + tab_height / 2;
        XSetForeground(display, gc, i == current_tab ? color_accent : color_muted);
        XDrawLine(display, window, gc, close_x - 3, close_y - 3, close_x + 3, close_y + 3);
        XDrawLine(display, window, gc, close_x + 3, close_y - 3, close_x - 3, close_y + 3);
        x += width + 5;
    }
    int plus_width = 34;
    XSetForeground(display, gc, color_panel);
    XFillRectangle(display, window, gc, x, bar_y, (unsigned)plus_width, (unsigned)tab_height);
    XSetForeground(display, gc, color_accent);
    XDrawRectangle(display, window, gc, x, bar_y, (unsigned)plus_width, (unsigned)tab_height);
    int plus_x = x + plus_width / 2;
    int plus_y = bar_y + tab_height / 2;
    XDrawLine(display, window, gc, plus_x - 5, plus_y, plus_x + 5, plus_y);
    XDrawLine(display, window, gc, plus_x, plus_y - 5, plus_x, plus_y + 5);

    const char hint[] = "Tab complete | Ctrl+Pg tabs | Shift+Pg scroll";
    XSetForeground(display, gc, color_muted);
    int hint_y = TAB_BAR_HEIGHT + baseline + 2;
    XDrawString(display, window, gc, LEFT_MARGIN, hint_y, hint, sizeof(hint) - 1);
    char status[48];
    if (tabs[current_tab].scroll_lines > 0)
        snprintf(status, sizeof(status), "%zu LINES UP", tabs[current_tab].scroll_lines);
    else
        snprintf(status, sizeof(status), "TAB %d / %d", current_tab + 1, active_count());
    int status_width = text_width(status, strlen(status));
    int status_x = window_width - LEFT_MARGIN - status_width;
    int hint_end = LEFT_MARGIN + text_width(hint, sizeof(hint) - 1) + 12;
    if (status_x >= hint_end) XDrawString(display, window, gc, status_x, hint_y,
                                          status, strlen(status));
    XSetForeground(display, gc, color_border);
    XDrawLine(display, window, gc, LEFT_MARGIN, TAB_BAR_HEIGHT + cell_height + 2,
              window_width - LEFT_MARGIN, TAB_BAR_HEIGHT + cell_height + 2);

    Tab *tab = &tabs[current_tab];
    int usable_width = window_width - LEFT_MARGIN * 2;
    if (usable_width < 1) usable_width = 1;
    VisualLine *lines = malloc((tab->length + tab->input_length + 4) * sizeof(*lines));
    if (!lines) return;
    size_t line_count = make_visual_lines(tab->text, tab->length, lines,
                                          tab->length + tab->input_length + 3,
                                          usable_width);
    int content_top = TAB_BAR_HEIGHT + cell_height + 7;
    int visible = (window_height - content_top - cell_height) / cell_height;
    if (visible < 1) visible = 1;
    size_t max_scroll = line_count > (size_t)visible ? line_count - (size_t)visible : 0;
    if (tab->scroll_lines > max_scroll) tab->scroll_lines = max_scroll;
    size_t end_line = line_count > tab->scroll_lines ? line_count - tab->scroll_lines : 0;
    size_t start_line = end_line > (size_t)visible ? end_line - (size_t)visible : 0;
    int y = content_top + font->ascent;
    XSetForeground(display, gc, color_text);
    for (size_t i = start_line; i < end_line; i++) {
        size_t len = lines[i].length;
        if (len > 32767) len = 32767;
        if (selection_tab == current_tab && selection_end > selection_start) {
            size_t line_begin = lines[i].offset;
            size_t line_end = line_begin + len;
            size_t selected_begin = selection_start > line_begin ? selection_start : line_begin;
            size_t selected_end = selection_end < line_end ? selection_end : line_end;
            if (selected_end > selected_begin) {
                int left = LEFT_MARGIN + text_width(tab->text + line_begin,
                                                     selected_begin - line_begin);
                int right = LEFT_MARGIN + text_width(tab->text + line_begin,
                                                      selected_end - line_begin);
                XSetForeground(display, gc, color_tab_active);
                XFillRectangle(display, window, gc, left, y - font->ascent,
                               (unsigned)(right - left),
                               (unsigned)(font->ascent + font->descent));
                XSetForeground(display, gc, color_text);
            }
        }
        XDrawString(display, window, gc, LEFT_MARGIN, y,
                    tab->text + lines[i].offset, (int)len);
        y += cell_height;
    }
    free(lines);

    int input_y = window_height - cell_height + font->ascent;
    XSetForeground(display, gc, color_panel);
    XFillRectangle(display, window, gc, 0, window_height - cell_height - 7,
                   (unsigned)window_width, (unsigned)(cell_height + 7));
    XSetForeground(display, gc, color_border);
    XDrawLine(display, window, gc, LEFT_MARGIN, window_height - cell_height - 7,
              window_width - LEFT_MARGIN, window_height - cell_height - 7);

    const char *prompt = tab->search_mode ? "reverse-i-search> " : "user@terminator> ";
    size_t prompt_length = strlen(prompt);
    int prompt_width = text_width(prompt, prompt_length);
    int input_x = LEFT_MARGIN + prompt_width;
    int available_width = window_width - input_x - LEFT_MARGIN;
    if (available_width < 1) available_width = 1;
    size_t input_len = tab->input_length;
    if (input_len >= INPUT_SIZE) input_len = INPUT_SIZE - 1;
    if (tab->cursor > input_len) tab->cursor = input_len;
    size_t input_start = 0;
    const char marker[] = "...";
    int marker_width = text_width(marker, sizeof(marker) - 1);
    if (text_width(tab->input, tab->cursor) > available_width) {
        int tail_width = available_width - marker_width;
        if (tail_width < 0) tail_width = 0;
        size_t low = 0;
        size_t high = tab->cursor;
        while (low < high) {
            size_t middle = low + (high - low + 1) / 2;
            if (text_width(tab->input + tab->cursor - middle, middle) <= tail_width)
                low = middle;
            else
                high = middle - 1;
        }
        input_start = tab->cursor - low;
    }
    XSetForeground(display, gc, color_accent);
    XDrawString(display, window, gc, LEFT_MARGIN, input_y, prompt, (int)prompt_length);
    int cursor_x = input_x;
    if (input_start) {
        XSetForeground(display, gc, color_muted);
        XDrawString(display, window, gc, cursor_x, input_y, marker, sizeof(marker) - 1);
        cursor_x += marker_width;
    }
    int input_text_x = cursor_x;
    if (tab->input_selection_active) {
        size_t selected_start = tab->input_selection_anchor < tab->cursor ?
            tab->input_selection_anchor : tab->cursor;
        size_t selected_end = tab->input_selection_anchor > tab->cursor ?
            tab->input_selection_anchor : tab->cursor;
        if (selected_start < input_start) selected_start = input_start;
        if (selected_end > input_len) selected_end = input_len;
        if (selected_end > selected_start) {
            int left = input_text_x + text_width(tab->input + input_start,
                                                  selected_start - input_start);
            int right = input_text_x + text_width(tab->input + input_start,
                                                   selected_end - input_start);
            XSetForeground(display, gc, color_tab_active);
            XFillRectangle(display, window, gc, left, input_y - font->ascent,
                           (unsigned)(right - left), (unsigned)(font->ascent + font->descent));
        }
    }
    XSetForeground(display, gc, color_text);
    size_t before_cursor = tab->cursor - input_start;
    XDrawString(display, window, gc, cursor_x, input_y, tab->input + input_start,
                (int)before_cursor);
    cursor_x += text_width(tab->input + input_start, before_cursor);
    XSetForeground(display, gc, color_accent);
    XFillRectangle(display, window, gc, cursor_x, input_y - font->ascent,
                   2, (unsigned)(font->ascent + font->descent));
    XSetForeground(display, gc, color_text);
    int remaining_width = window_width - LEFT_MARGIN - cursor_x - 3;
    size_t suffix_length = input_len - tab->cursor;
    while (suffix_length && text_width(tab->input + tab->cursor, suffix_length) > remaining_width)
        suffix_length--;
    if (suffix_length > 0)
        XDrawString(display, window, gc, cursor_x + 3, input_y,
                    tab->input + tab->cursor, (int)suffix_length);
    if (tab->search_mode && tab->input_length > 0 && remaining_width > 32) {
        const char prefix[] = "  => ";
        const char *suggestion = tab->search_match[0] ? tab->search_match : "no match";
        int suffix_x = cursor_x + 3 + text_width(tab->input + tab->cursor, suffix_length);
        if (suffix_x + text_width(prefix, sizeof(prefix) - 1) + 24 < window_width - LEFT_MARGIN) {
            XSetForeground(display, gc, color_muted);
            XDrawString(display, window, gc, suffix_x, input_y, prefix, sizeof(prefix) - 1);
            suffix_x += text_width(prefix, sizeof(prefix) - 1);
            size_t match_length = strlen(suggestion);
            while (match_length && suffix_x + text_width(suggestion, match_length) > window_width - LEFT_MARGIN)
                match_length--;
            XDrawString(display, window, gc, suffix_x, input_y, suggestion,
                        (int)match_length);
        }
    }
    XFlush(display);
}

static void update_history_search(Tab *tab) {
    tab->search_match[0] = '\0';
    if (tab->search_mode && tab->input_length > 0)
        history_best_match(tab->input, tab->search_match, sizeof(tab->search_match));
}

static void choose_completion(Tab *tab, size_t index) {
    if (!tab->completion_pending || index >= tab->completion_count) return;
    char replacement[512], escaped[1024];
    snprintf(replacement, sizeof(replacement), "%s", tab->completion_items[index]);
    escape_filename(replacement, escaped, sizeof(escaped));
    size_t start = tab->completion_start;
    erase_input_range(tab, start, tab->cursor);
    insert_input(tab, escaped, strlen(escaped));
    if (tab->completion_is_dir[index]) insert_input(tab, "/", 1);
    else insert_input(tab, " ", 1);
    tab->completion_pending = 0;
    tab->completion_choice_length = 0;
    tab->completion_choice[0] = '\0';
}

static void copy_current_selection(Tab *tab) {
    selection_copy[0] = '\0';
    if (selection_tab == current_tab && selection_end > selection_start &&
        selection_end <= tab->length) {
        size_t length = selection_end - selection_start;
        if (length >= sizeof(selection_copy)) length = sizeof(selection_copy) - 1;
        memcpy(selection_copy, tab->text + selection_start, length);
        selection_copy[length] = '\0';
    } else if (tab->input_selection_active) {
        size_t begin = tab->input_selection_anchor < tab->cursor ?
            tab->input_selection_anchor : tab->cursor;
        size_t end = tab->input_selection_anchor > tab->cursor ?
            tab->input_selection_anchor : tab->cursor;
        if (end > begin) {
            size_t length = end - begin;
            if (length >= sizeof(selection_copy)) length = sizeof(selection_copy) - 1;
            memcpy(selection_copy, tab->input + begin, length);
            selection_copy[length] = '\0';
        }
    }
    if (!selection_copy[0]) {
        size_t length = tab->input_length;
        if (length >= sizeof(selection_copy)) length = sizeof(selection_copy) - 1;
        memcpy(selection_copy, tab->input, length);
        selection_copy[length] = '\0';
    }
    XSetSelectionOwner(display, clipboard_atom, window, CurrentTime);
    if (XGetSelectionOwner(display, clipboard_atom) != window)
        XSetSelectionOwner(display, XA_PRIMARY, window, CurrentTime);
    selection_owner = 1;
}

static void request_paste(void) {
    Atom selection = XGetSelectionOwner(display, clipboard_atom) == None ?
        XA_PRIMARY : clipboard_atom;
    paste_pending = 1;
    paste_fallback = 0;
    paste_selection_atom = selection;
    XConvertSelection(display, selection, utf8_atom, clipboard_property,
                      window, CurrentTime);
    XFlush(display);
}

static void submit_input(Tab *tab) {
    if (tab->completion_pending && tab->completion_choice_length > 0) {
        long selection = strtol(tab->completion_choice, NULL, 10);
        if (selection > 0) choose_completion(tab, (size_t)(selection - 1));
        return;
    }
    if (tab->search_mode) {
        if (tab->search_match[0]) set_input(tab, tab->search_match);
        else set_input(tab, tab->history_draft);
        tab->search_mode = 0;
        tab->history_index = -1;
        return;
    }
    append_text(tab, tab->input, tab->input_length);
    append_text(tab, "\n", 1);
    write_all(tab->input_fd, tab->input, tab->input_length);
    write_all(tab->input_fd, "\n", 1);
    tab->input_length = 0;
    tab->input[0] = '\0';
    tab->cursor = 0;
    tab->history_index = -1;
    tab->input_selection_active = 0;
    tab->completion_pending = 0;
    tab->scroll_lines = 0;
}

static void handle_key(XKeyEvent *event) {
    char bytes[32];
    KeySym key;
    int length = XLookupString(event, bytes, sizeof(bytes), &key, NULL);
    if (current_tab < 0) return;
    Tab *tab = &tabs[current_tab];
    KeyAction action;
    int has_action = bound_action(key, event->state, &action);
    if ((event->state & ControlMask) && !(event->state & ShiftMask) &&
        (key == XK_c || key == XK_C)) {
        if (tab->search_mode) {
            set_input(tab, tab->history_draft);
            tab->search_mode = 0;
            draw_window();
            return;
        }
        pid_t foreground = 0;
        pid_t watch_pids[MAX_PIPE_COMMANDS];
        size_t watch_count = 0;
        control_lock(shared_control);
        foreground = shared_control->foreground[current_tab];
        if (shared_control->watch_pid_count[current_tab]) {
            shared_control->watch_cancel[current_tab] = 1;
            watch_count = shared_control->watch_pid_count[current_tab];
            if (watch_count > MAX_PIPE_COMMANDS) watch_count = MAX_PIPE_COMMANDS;
            memcpy(watch_pids, shared_control->watch_pids[current_tab], watch_count * sizeof(pid_t));
        }
        sem_post(&shared_control->mutex);
        if (foreground > 0) kill(-foreground, SIGINT);
        for (size_t i = 0; i < watch_count; i++) if (watch_pids[i] > 0) kill(-watch_pids[i], SIGTERM);
        if (foreground > 0 || watch_count > 0) append_text(tab, "^C\n", 3);
        else {
            tab->input_length = 0;
            tab->input[0] = '\0';
            tab->cursor = 0;
            tab->input_selection_active = 0;
        }
        draw_window();
        return;
    } else if ((event->state & ControlMask) && !(event->state & ShiftMask) &&
               (key == XK_z || key == XK_Z)) {
        control_lock(shared_control);
        pid_t foreground = shared_control->foreground[current_tab];
        sem_post(&shared_control->mutex);
        if (foreground > 0) {
            kill(-foreground, SIGTSTP);
            append_text(tab, "^Z\n", 3);
        }
        draw_window();
        return;
    } else if (has_action && action == ACTION_NEW_TAB) {
        add_tab();
        return;
    } else if (has_action && action == ACTION_CLOSE_TAB) {
        close_tab(current_tab);
        return;
    } else if (has_action && action == ACTION_NEXT_TAB) {
        int next = next_active(current_tab, 1);
        if (next >= 0) current_tab = next;
    } else if (has_action && action == ACTION_PREVIOUS_TAB) {
        int previous = next_active(current_tab, -1);
        if (previous >= 0) current_tab = previous;
    } else if (has_action && action == ACTION_COPY) {
        copy_current_selection(tab);
    } else if (has_action && action == ACTION_PASTE) {
        request_paste();
    } else if (has_action && action == ACTION_SEARCH) {
        if (!tab->search_mode) {
            copy_bounded(tab->history_draft, sizeof(tab->history_draft), tab->input);
            tab->input[0] = '\0';
            tab->input_length = tab->cursor = 0;
            tab->search_mode = 1;
        }
        update_history_search(tab);
    } else if (has_action && action == ACTION_CLEAR) {
        tab->length = 0;
        tab->text[0] = '\0';
        tab->scroll_lines = 0;
        if (selection_tab == current_tab) selection_tab = -1;
    } else if (has_action && action == ACTION_EOF && tab->input_length == 0) {
        close_tab(current_tab);
        return;
    } else if (key == XK_Return || key == XK_KP_Enter) {
        submit_input(tab);
        update_history_search(tab);
    } else if (key == XK_Escape) {
        if (tab->search_mode) {
            set_input(tab, tab->history_draft);
            tab->search_mode = 0;
        }
        tab->completion_pending = 0;
        tab->completion_choice_length = 0;
        tab->completion_choice[0] = '\0';
    } else if (key == XK_Tab) {
        complete_filename(tab);
    } else if (tab->completion_pending && length == 1 && bytes[0] >= '0' &&
               bytes[0] <= '9') {
        if (tab->completion_choice_length + 1 < sizeof(tab->completion_choice)) {
            tab->completion_choice[tab->completion_choice_length++] = bytes[0];
            tab->completion_choice[tab->completion_choice_length] = '\0';
        }
    } else if (key == XK_Left) {
        if ((event->state & ShiftMask) && !tab->input_selection_active) {
            tab->input_selection_anchor = tab->cursor;
            tab->input_selection_active = 1;
        }
        if (tab->cursor > 0) tab->cursor--;
        if (!(event->state & ShiftMask)) tab->input_selection_active = 0;
    } else if (key == XK_Right) {
        if ((event->state & ShiftMask) && !tab->input_selection_active) {
            tab->input_selection_anchor = tab->cursor;
            tab->input_selection_active = 1;
        }
        if (tab->cursor < tab->input_length) tab->cursor++;
        if (!(event->state & ShiftMask)) tab->input_selection_active = 0;
    } else if (key == XK_Home || ((event->state & ControlMask) && key == XK_a)) {
        if ((event->state & ShiftMask) && !tab->input_selection_active) {
            tab->input_selection_anchor = tab->cursor;
            tab->input_selection_active = 1;
        }
        tab->cursor = 0;
        if (!(event->state & ShiftMask)) tab->input_selection_active = 0;
    } else if (key == XK_End || ((event->state & ControlMask) && key == XK_e)) {
        if ((event->state & ShiftMask) && !tab->input_selection_active) {
            tab->input_selection_anchor = tab->cursor;
            tab->input_selection_active = 1;
        }
        tab->cursor = tab->input_length;
        if (!(event->state & ShiftMask)) tab->input_selection_active = 0;
    } else if (key == XK_Delete) {
        if (tab->input_selection_active) {
            size_t begin = tab->input_selection_anchor < tab->cursor ? tab->input_selection_anchor : tab->cursor;
            size_t end = tab->input_selection_anchor > tab->cursor ? tab->input_selection_anchor : tab->cursor;
            erase_input_range(tab, begin, end);
            tab->input_selection_active = 0;
        } else if (tab->cursor < tab->input_length) erase_input_range(tab, tab->cursor, tab->cursor + 1);
    } else if (key == XK_BackSpace) {
        if (tab->input_selection_active) {
            size_t begin = tab->input_selection_anchor < tab->cursor ? tab->input_selection_anchor : tab->cursor;
            size_t end = tab->input_selection_anchor > tab->cursor ? tab->input_selection_anchor : tab->cursor;
            erase_input_range(tab, begin, end);
            tab->input_selection_active = 0;
        } else if (tab->cursor > 0) erase_input_range(tab, tab->cursor - 1, tab->cursor);
    } else if (key == XK_Page_Up && (event->state & ShiftMask)) {
        tab->scroll_lines += 8;
    } else if (key == XK_Page_Down && (event->state & ShiftMask)) {
        tab->scroll_lines = tab->scroll_lines > 8 ? tab->scroll_lines - 8 : 0;
    } else if (key == XK_Up) {
        history_up(tab);
    } else if (key == XK_Down) {
        history_down(tab);
    } else if ((event->state & ControlMask) && key == XK_u) {
        erase_input_range(tab, 0, tab->cursor);
    } else if ((event->state & ControlMask) && key == XK_k) {
        erase_input_range(tab, tab->cursor, tab->input_length);
    } else if ((event->state & ControlMask) && key == XK_w) {
        size_t start = tab->cursor;
        while (start > 0 && isspace((unsigned char)tab->input[start - 1])) start--;
        while (start > 0 && !isspace((unsigned char)tab->input[start - 1])) start--;
        erase_input_range(tab, start, tab->cursor);
    } else if (length > 0 && !(event->state & (ControlMask | Mod1Mask | Mod4Mask))) {
        for (int i = 0; i < length; i++) {
            unsigned char ch = (unsigned char)bytes[i];
            if (ch >= 32 && ch != 127) insert_input(tab, (char *)&bytes[i], 1);
        }
        if (tab->search_mode) update_history_search(tab);
    }
    if (!tab->search_mode && key != XK_Up && key != XK_Down && key != XK_Left && key != XK_Right)
        tab->history_index = -1;
    if (tab->search_mode) update_history_search(tab);
    draw_window();
}

static size_t text_position_at(int tab_index, int x, int y) {
    Tab *tab = &tabs[tab_index];
    int cell_height = font->ascent + font->descent + 5;
    int content_top = TAB_BAR_HEIGHT + cell_height + 7;
    int visible = (window_height - content_top - cell_height) / cell_height;
    if (visible < 1) visible = 1;
    size_t line_capacity = tab->length + 2;
    VisualLine *lines = malloc(line_capacity * sizeof(*lines));
    if (!lines) return tab->length;
    size_t line_count = make_visual_lines(tab->text, tab->length, lines,
                                          line_capacity,
                                          window_width - LEFT_MARGIN * 2);
    size_t max_scroll = line_count > (size_t)visible ? line_count - (size_t)visible : 0;
    size_t scroll = tab->scroll_lines > max_scroll ? max_scroll : tab->scroll_lines;
    size_t end_line = line_count > scroll ? line_count - scroll : 0;
    size_t start_line = end_line > (size_t)visible ? end_line - (size_t)visible : 0;
    int row = (y - content_top) / cell_height;
    if (row < 0) row = 0;
    size_t line_index = start_line + (size_t)row;
    if (line_index >= end_line) {
        free(lines);
        return tab->length;
    }
    VisualLine line = lines[line_index];
    size_t result = line.offset;
    int target_x = x - LEFT_MARGIN;
    if (target_x <= 0) { free(lines); return result; }
    int width = 0;
    for (size_t i = 0; i < line.length; i++) {
        int char_width = text_width(tab->text + line.offset + i, 1);
        if (target_x < width + char_width / 2) break;
        width += char_width;
        result++;
    }
    free(lines);
    return result;
}

static void update_drag_selection(int x, int y) {
    if (!selection_dragging || selection_tab < 0 || !tabs[selection_tab].active) return;
    selection_end = text_position_at(selection_tab, x, y);
    draw_window();
}

static void finish_drag_selection(void) {
    if (!selection_dragging) return;
    selection_dragging = 0;
    if (selection_end < selection_start) {
        size_t swap = selection_start;
        selection_start = selection_end;
        selection_end = swap;
    }
    selection_copy[0] = '\0';
    if (selection_tab >= 0 && selection_end > selection_start) {
        size_t length = selection_end - selection_start;
        if (length >= sizeof(selection_copy)) length = sizeof(selection_copy) - 1;
        memcpy(selection_copy, tabs[selection_tab].text + selection_start, length);
        selection_copy[length] = '\0';
        XSetSelectionOwner(display, XA_PRIMARY, window, CurrentTime);
        selection_owner = 1;
    }
    draw_window();
}

static void handle_selection_request(XSelectionRequestEvent *request) {
    XSelectionEvent response;
    memset(&response, 0, sizeof(response));
    response.type = SelectionNotify;
    response.display = request->display;
    response.requestor = request->requestor;
    response.selection = request->selection;
    response.target = request->target;
    response.time = request->time;
    response.property = None;
    Atom property = request->property == None ? request->target : request->property;
    if (request->target == targets_atom) {
        Atom supported[] = {targets_atom, XA_STRING, utf8_atom};
        XChangeProperty(display, request->requestor, property, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)supported, 3);
        response.property = property;
    } else if (request->target == XA_STRING || request->target == utf8_atom) {
        XChangeProperty(display, request->requestor, property, request->target, 8,
                        PropModeReplace, (unsigned char *)selection_copy,
                        (int)strlen(selection_copy));
        response.property = property;
    }
    XSendEvent(display, request->requestor, False, 0, (XEvent *)&response);
    XFlush(display);
}

static void handle_selection_notify(XSelectionEvent *event) {
    if (!paste_pending || current_tab < 0) {
        paste_pending = 0;
        return;
    }
    if (event->property == None && !paste_fallback) {
        paste_fallback = 1;
        XConvertSelection(display, paste_selection_atom, XA_STRING,
                          clipboard_property, window, CurrentTime);
        XFlush(display);
        return;
    }
    if (event->property == None) { paste_pending = 0; return; }
    Atom actual_type;
    int actual_format;
    unsigned long count, remaining;
    unsigned char *data = NULL;
    int result = XGetWindowProperty(display, window, event->property, 0,
                                    INPUT_SIZE / 4, True, AnyPropertyType,
                                    &actual_type, &actual_format, &count,
                                    &remaining, &data);
    if (result == Success && data && actual_format == 8 && count > 0) {
        size_t length = count;
        if (length > INPUT_SIZE - 1) length = INPUT_SIZE - 1;
        insert_input(&tabs[current_tab], (const char *)data, length);
        if (tabs[current_tab].search_mode) update_history_search(&tabs[current_tab]);
    }
    if (data) XFree(data);
    paste_pending = 0;
    draw_window();
}

static void handle_click(XButtonEvent *event) {
    if (current_tab < 0) return;
    if (event->button == Button1 && event->y >= 6 && event->y < TAB_BAR_HEIGHT - 4) {
        int x = 8;
        for (int i = 0; i < MAX_TABS; i++) {
            if (!tabs[i].active) continue;
            char label[16];
            snprintf(label, sizeof(label), "%d", i + 1);
            int width = text_width(label, strlen(label)) + 28;
            if (event->x >= x && event->x < x + width) {
                /* The trailing x area closes; the rest selects. */
                if (event->x >= x + width - 22) close_tab(i);
                else { current_tab = i; draw_window(); }
                return;
            }
            x += width + 5;
        }
        if (event->x >= x && event->x < x + 34) add_tab();
        return;
    }
    if (event->button == Button1 && event->y >= window_height -
        (font->ascent + font->descent + 12)) {
        Tab *tab = &tabs[current_tab];
        const char *prompt = tab->search_mode ? "reverse-i-search> " : "user@terminator> ";
        int target = event->x - LEFT_MARGIN - text_width(prompt, strlen(prompt));
        if (target < 0) target = 0;
        int width = 0;
        size_t position = 0;
        while (position < tab->input_length) {
            int char_width = text_width(tab->input + position, 1);
            if (target < width + char_width / 2) break;
            width += char_width;
            position++;
        }
        tab->cursor = position;
        tab->input_selection_active = 0;
        draw_window();
        return;
    }
    if (event->button == Button1 && event->y < window_height -
        (font->ascent + font->descent + 12)) {
        selection_tab = current_tab;
        selection_start = selection_end = text_position_at(current_tab, event->x, event->y);
        selection_dragging = 1;
        draw_window();
    }
    if (event->button == Button4) {
        if (tabs[current_tab].scroll_lines < 10000) tabs[current_tab].scroll_lines++;
        draw_window();
    } else if (event->button == Button5) {
        if (tabs[current_tab].scroll_lines > 0) tabs[current_tab].scroll_lines--;
        draw_window();
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "useProcess=True") == 0) process_mode = 1;
    if (!process_mode) signal(SIGPIPE, SIG_IGN);
    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "terminator: cannot open X display; ensure X11 is available and DISPLAY is set\n");
        return 1;
    }
    screen = DefaultScreen(display);
    color_background = named_color("#0f1722", BlackPixel(display, screen));
    color_panel = named_color("#172332", BlackPixel(display, screen));
    color_tab = named_color("#1b2939", BlackPixel(display, screen));
    color_tab_active = named_color("#214052", BlackPixel(display, screen));
    color_border = named_color("#34485d", WhitePixel(display, screen));
    color_text = named_color("#e4edf6", WhitePixel(display, screen));
    color_muted = named_color("#91a2b5", WhitePixel(display, screen));
    color_accent = named_color("#58d6bf", WhitePixel(display, screen));
    window = XCreateSimpleWindow(display, RootWindow(display, screen), 40, 40,
                                 (unsigned)window_width, (unsigned)window_height, 1,
                                 color_border, color_background);
    XStoreName(display, window, "Terminator");
    XSelectInput(display, window, ExposureMask | KeyPressMask | ButtonPressMask |
                 ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);
    wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete_window, 1);
    clipboard_atom = XInternAtom(display, "CLIPBOARD", False);
    utf8_atom = XInternAtom(display, "UTF8_STRING", False);
    targets_atom = XInternAtom(display, "TARGETS", False);
    clipboard_property = XInternAtom(display, "TERMINATOR_CLIPBOARD", False);
    load_keybindings();
    gc = XCreateGC(display, window, 0, NULL);
    font = XLoadQueryFont(display, "16x32");
    if (!font) font = XLoadQueryFont(display,
        "-misc-fixed-medium-r-normal--32-*-*-*-*-*-*-*");
    if (!font) font = XLoadQueryFont(display, "14x28");
    if (!font) font = XLoadQueryFont(display,
        "-misc-fixed-medium-r-normal--28-*-*-*-*-*-*-*");
    if (!font) font = XLoadQueryFont(display, "12x24");
    if (!font) font = XLoadQueryFont(display,
        "-misc-fixed-medium-r-normal--24-*-*-*-*-*-*-*");
    if (!font) font = XLoadQueryFont(display, "10x20");
    if (!font) font = XLoadQueryFont(display,
        "-misc-fixed-medium-r-normal--20-*-*-*-*-*-*-*");
    if (!font) font = XLoadQueryFont(display, "9x18");
    if (!font) font = XLoadQueryFont(display, "fixed");
    if (!font) {
        fprintf(stderr, "terminator: cannot load the X11 fixed font\n");
        XFreeGC(display, gc);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return 1;
    }
    XSetFont(display, gc, font->fid);
    XSetForeground(display, gc, color_text);
    if (!getcwd(initial_cwd, sizeof(initial_cwd))) {
        fprintf(stderr, "terminator: cannot determine the starting directory: %s\n", strerror(errno));
        XFreeFont(display, font);
        XFreeGC(display, gc);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return 1;
    }
    const char *home = getenv("HOME");
    if (home && snprintf(history_path, sizeof(history_path), "%s/.bash_history", home) >=
                    (int)sizeof(history_path)) history_path[0] = '\0';
    shared_history = mmap(NULL, sizeof(*shared_history), PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared_history == MAP_FAILED) {
        fprintf(stderr, "terminator: cannot allocate shared history: %s\n", strerror(errno));
        XFreeFont(display, font);
        XFreeGC(display, gc);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return 1;
    }
    memset(shared_history, 0, sizeof(*shared_history));
    if (sem_init(&shared_history->mutex, 1, 1) < 0) {
        fprintf(stderr, "terminator: cannot initialize shared history lock: %s\n", strerror(errno));
        munmap(shared_history, sizeof(*shared_history));
        XFreeFont(display, font);
        XFreeGC(display, gc);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return 1;
    }
    shared_control = mmap(NULL, sizeof(*shared_control), PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared_control == MAP_FAILED) {
        fprintf(stderr, "terminator: cannot allocate shared job control: %s\n", strerror(errno));
        munmap(shared_history, sizeof(*shared_history));
        XFreeFont(display, font);
        XFreeGC(display, gc);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return 1;
    }
    memset(shared_control, 0, sizeof(*shared_control));
    if (sem_init(&shared_control->mutex, 1, 1) < 0) {
        fprintf(stderr, "terminator: cannot initialize job control lock: %s\n", strerror(errno));
        munmap(shared_control, sizeof(*shared_control));
        munmap(shared_history, sizeof(*shared_history));
        XFreeFont(display, font);
        XFreeGC(display, gc);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return 1;
    }
    history_load_file(shared_history);
    XMapWindow(display, window);
    XFlush(display);
    if (add_tab() < 0) {
        fprintf(stderr, "terminator: could not start initial tab\n");
        XCloseDisplay(display);
        return 1;
    }

    int x_fd = ConnectionNumber(display);
    while (active_count() > 0) {
        struct pollfd fds[MAX_TABS + 1];
        int slots[MAX_TABS];
        nfds_t count = 1;
        fds[0] = (struct pollfd){x_fd, POLLIN, 0};
        int slot_count = 0;
        for (int i = 0; i < MAX_TABS; i++) {
            if (!tabs[i].active || tabs[i].output_fd < 0) continue;
            fds[count] = (struct pollfd){tabs[i].output_fd, POLLIN | POLLHUP, 0};
            slots[slot_count++] = i;
            count++;
        }
        int ready;
        do { ready = poll(fds, count, -1); } while (ready < 0 && errno == EINTR);
        if (ready < 0) { perror("terminator: poll"); break; }

        if (fds[0].revents & POLLIN) {
            while (XPending(display)) {
                XEvent event;
                XNextEvent(display, &event);
                if (event.type == Expose) draw_window();
                else if (event.type == ConfigureNotify) {
                    window_width = event.xconfigure.width;
                    window_height = event.xconfigure.height;
                    draw_window();
                } else if (event.type == KeyPress) handle_key(&event.xkey);
                else if (event.type == ButtonPress) handle_click(&event.xbutton);
                else if (event.type == MotionNotify)
                    update_drag_selection(event.xmotion.x, event.xmotion.y);
                else if (event.type == ButtonRelease && event.xbutton.button == Button1)
                    finish_drag_selection();
                else if (event.type == SelectionRequest)
                    handle_selection_request(&event.xselectionrequest);
                else if (event.type == SelectionNotify)
                    handle_selection_notify(&event.xselection);
                else if (event.type == SelectionClear) selection_owner = 0;
                else if (event.type == ClientMessage &&
                         (Atom)event.xclient.data.l[0] == wm_delete_window) {
                    shutting_down = 1;
                    for (int i = 0; i < MAX_TABS; i++) if (tabs[i].active) close_tab(i);
                }
            }
        }
        if (shutting_down) break;
        for (int p = 0; p < slot_count; p++) {
            int slot = slots[p];
            if (!tabs[slot].active || tabs[slot].output_fd != fds[p + 1].fd ||
                !(fds[p + 1].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char output[4096];
            ssize_t bytes = read(tabs[slot].output_fd, output, sizeof(output));
            if (bytes > 0) {
                append_text(&tabs[slot], output, (size_t)bytes);
                if (slot == current_tab) draw_window();
            } else if (bytes == 0 || (bytes < 0 && errno != EINTR && errno != EAGAIN)) {
                close_tab(slot);
            }
        }
    }
    shutting_down = 1;
    for (int i = 0; i < MAX_TABS; i++) if (tabs[i].active) close_tab(i);
    if (font) XFreeFont(display, font);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
