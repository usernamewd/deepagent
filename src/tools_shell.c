/*
 * tools_shell.c
 *
 * Linux shell and background process tools. Foreground commands use fork/exec
 * through /usr/bin/env sh -c, separate stdout/stderr pipes, poll-based output
 * collection, and timeout enforcement. The background table is one of the few
 * permitted mutable globals because process tracking is inherently session-wide.
 */

#define _POSIX_C_SOURCE 200809L
#include "tools_shell.h"

#include "config.h"
#include "ui.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_PROCS 64

typedef struct {
    pid_t pid;
    int fd;
    char command[512];
    int active;
    int exit_code;
} ProcEntry;

static ProcEntry g_procs[MAX_PROCS];
static int g_bash_confirmed;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static int sb_append_len(StrBuf *b, const char *s, size_t n) {
    char *tmp;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->len + n + 1) nc *= 2;
        tmp = realloc(b->data, nc);
        if (!tmp) return 0;
        b->data = tmp;
        b->cap = nc;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

static int sb_append(StrBuf *b, const char *s) {
    return sb_append_len(b, s, strlen(s));
}

static int sb_appendf(StrBuf *b, const char *fmt, ...) {
    va_list ap, cp;
    int n;
    char *tmp;
    va_start(ap, fmt);
    va_copy(cp, ap);
    n = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (n < 0) { va_end(ap); return 0; }
    tmp = malloc((size_t)n + 1);
    if (!tmp) { va_end(ap); return 0; }
    vsnprintf(tmp, (size_t)n + 1, fmt, ap);
    va_end(ap);
    n = sb_append(b, tmp);
    free(tmp);
    return n;
}

static int is_shell_boundary(char c) {
    return c == '\0' || c == ' ' || c == '\t' || c == '\n' ||
           c == ';' || c == '&' || c == '|' || c == ')' || c == '(' ||
           c == '<' || c == '>';
}

static int contains_root_rm(const char *cmd) {
    const char pattern[] = "rm -rf /";
    const size_t pattern_len = sizeof(pattern) - 1;
    const char *p = cmd;
    while ((p = strstr(p, pattern)) != NULL) {
        char next = p[pattern_len];
        if (next == '\0' || is_shell_boundary(next) || next == '*') return 1;
        p++;
    }
    return 0;
}

static int contains_device_token(const char *cmd, const char *dev) {
    size_t n = strlen(dev);
    const char *p = cmd;
    while ((p = strstr(p, dev)) != NULL) {
        if (is_shell_boundary(p[n])) return 1;
        p++;
    }
    return 0;
}

static int contains_mkfs_command(const char *cmd) {
    const char *p = cmd;
    while ((p = strstr(p, "mkfs.")) != NULL) {
        if (p == cmd || is_shell_boundary(p[-1])) return 1;
        p++;
    }
    return 0;
}

static int blocked_command(const char *cmd) {
    return contains_root_rm(cmd) || strstr(cmd, ":(){:|:&};:") ||
           contains_device_token(cmd, "/dev/sda") ||
           contains_device_token(cmd, "/dev/nvme") ||
           contains_mkfs_command(cmd);
}

static long elapsed_sec(struct timespec start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec - start.tv_sec;
}

static void drain_fd(int fd, StrBuf *out) {
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) sb_append_len(out, buf, (size_t)n);
}

static void child_exec_shell(const char *command) {
    execlp("env", "env", "sh", "-c", command, (char *)NULL);
    _exit(127);
}

static ToolResult tool_bash(cJSON *args) {
    const char *cmd = tools_json_get_string(args, "command", NULL);
    int timeout = tools_json_get_int(args, "timeout_seconds", 30);
    const Config *cfg = config_global();
    int outp[2], errp[2], status = 0, timed = 0;
    pid_t pid;
    struct pollfd pfds[2];
    StrBuf out = {0}, err = {0};
    struct timespec start;
    if (!cmd) return tool_result_error("command required");
    if (blocked_command(cmd)) return tool_result_error("blocked dangerous command pattern");
    if (timeout <= 0 || timeout > 3600) timeout = 30;
    if ((!cfg || !cfg->auto_approve) && !g_bash_confirmed) {
        if (!ui_confirm("Allow DeepAgent to execute shell commands this session?")) return tool_result_error("bash cancelled");
        g_bash_confirmed = 1;
    }
    if (pipe(outp) < 0) return tool_result_error("pipe: %s", strerror(errno));
    if (pipe(errp) < 0) {
        close(outp[0]);
        close(outp[1]);
        return tool_result_error("pipe: %s", strerror(errno));
    }
    pid = fork();
    if (pid < 0) {
        close(outp[0]);
        close(outp[1]);
        close(errp[0]);
        close(errp[1]);
        return tool_result_error("fork: %s", strerror(errno));
    }
    if (pid == 0) {
        close(outp[0]); close(errp[0]);
        dup2(outp[1], STDOUT_FILENO);
        dup2(errp[1], STDERR_FILENO);
        close(outp[1]); close(errp[1]);
        child_exec_shell(cmd);
    }
    close(outp[1]); close(errp[1]);
    fcntl(outp[0], F_SETFL, fcntl(outp[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(errp[0], F_SETFL, fcntl(errp[0], F_GETFL, 0) | O_NONBLOCK);
    pfds[0].fd = outp[0]; pfds[0].events = POLLIN;
    pfds[1].fd = errp[0]; pfds[1].events = POLLIN;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        int rc, done;
        poll(pfds, 2, 100);
        if (pfds[0].revents & POLLIN) {
            drain_fd(outp[0], &out);
        }
        if (pfds[1].revents & POLLIN) {
            drain_fd(errp[0], &err);
        }
        rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) break;
        if (elapsed_sec(start) >= timeout) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed = 1;
            break;
        }
        done = (pfds[0].revents & (POLLHUP | POLLERR)) && (pfds[1].revents & (POLLHUP | POLLERR));
        (void)done;
    }
    drain_fd(outp[0], &out);
    drain_fd(errp[0], &err);
    close(outp[0]); close(errp[0]);
    {
        int code = timed ? -1 : (WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
        cJSON *obj = cJSON_CreateObject();
        char *json;
        ToolResult r;
        cJSON_AddStringToObject(obj, "stdout", out.data ? out.data : "");
        cJSON_AddStringToObject(obj, "stderr", err.data ? err.data : "");
        cJSON_AddNumberToObject(obj, "exit_code", code);
        cJSON_AddBoolToObject(obj, "timed_out", timed);
        json = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
        free(out.data); free(err.data);
        r.output = json ? json : strdup("{\"error\":\"out of memory\"}");
        r.success = json != NULL;
        return r;
    }
}

static int store_proc(pid_t pid, int fd, const char *cmd) {
    int i;
    for (i = 0; i < MAX_PROCS; i++) {
        if (!g_procs[i].pid) {
            g_procs[i].pid = pid;
            g_procs[i].fd = fd;
            g_procs[i].active = 1;
            g_procs[i].exit_code = -1;
            snprintf(g_procs[i].command, sizeof(g_procs[i].command), "%s", cmd);
            return 1;
        }
    }
    return 0;
}

static ToolResult tool_background_process(cJSON *args) {
    const char *cmd = tools_json_get_string(args, "command", NULL);
    int pfd[2], pidfd[2];
    pid_t child;
    pid_t grand = -1;
    if (!cmd) return tool_result_error("command required");
    if (blocked_command(cmd)) return tool_result_error("blocked dangerous command pattern");
    if (pipe(pfd) < 0) return tool_result_error("pipe: %s", strerror(errno));
    if (pipe(pidfd) < 0) {
        close(pfd[0]); close(pfd[1]);
        return tool_result_error("pipe: %s", strerror(errno));
    }
    child = fork();
    if (child < 0) {
        close(pfd[0]);
        close(pfd[1]);
        close(pidfd[0]);
        close(pidfd[1]);
        return tool_result_error("fork: %s", strerror(errno));
    }
    if (child == 0) {
        close(pidfd[0]);
        grand = fork();
        if (grand < 0) _exit(127);
        if (grand > 0) {
            ssize_t ignored = write(pidfd[1], &grand, sizeof(grand));
            (void)ignored;
            _exit(0);
        }
        close(pidfd[1]);
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        child_exec_shell(cmd);
    }
    close(pidfd[1]);
    waitpid(child, NULL, 0);
    if (read(pidfd[0], &grand, sizeof(grand)) != sizeof(grand)) grand = -1;
    close(pidfd[0]);
    close(pfd[1]);
    if (grand <= 0) {
        close(pfd[0]);
        return tool_result_error("background launch failed");
    }
    fcntl(pfd[0], F_SETFL, fcntl(pfd[0], F_GETFL, 0) | O_NONBLOCK);
    if (!store_proc(grand, pfd[0], cmd)) {
        kill(grand, SIGTERM);
        close(pfd[0]);
        return tool_result_error("process table full");
    }
    return tool_result_ok("%d", (int)grand);
}

static ProcEntry *find_proc(pid_t pid) {
    int i;
    for (i = 0; i < MAX_PROCS; i++) if (g_procs[i].pid == pid) return &g_procs[i];
    return NULL;
}

static int signal_from_name(const char *s) {
    if (!s || strcmp(s, "SIGTERM") == 0) return SIGTERM;
    if (strcmp(s, "SIGKILL") == 0) return SIGKILL;
    if (strcmp(s, "SIGINT") == 0) return SIGINT;
    if (strcmp(s, "SIGHUP") == 0) return SIGHUP;
    if (strcmp(s, "SIGUSR1") == 0) return SIGUSR1;
    if (strcmp(s, "SIGUSR2") == 0) return SIGUSR2;
    return 0;
}

static ToolResult tool_kill_process(cJSON *args) {
    int pid = tools_json_get_int(args, "pid", -1);
    const char *sig = tools_json_get_string(args, "signal", "SIGTERM");
    const Config *cfg = config_global();
    ProcEntry *p = find_proc((pid_t)pid);
    int signum = signal_from_name(sig);
    char q[128];
    if (!p) return tool_result_error("pid not tracked");
    if (!signum) return tool_result_error("unsupported signal");
    if (!cfg || !cfg->auto_approve) {
        snprintf(q, sizeof(q), "Send %s to PID %d?", sig, pid);
        if (!ui_confirm(q)) return tool_result_error("kill cancelled");
    }
    if (kill((pid_t)pid, signum) < 0) return tool_result_error("kill: %s", strerror(errno));
    return tool_result_ok("signal sent");
}

static ToolResult tool_read_process_output(cJSON *args) {
    int pid = tools_json_get_int(args, "pid", -1);
    ProcEntry *p = find_proc((pid_t)pid);
    StrBuf out = {0};
    char buf[4096];
    ssize_t n;
    if (!p) return tool_result_error("pid not tracked");
    while ((n = read(p->fd, buf, sizeof(buf))) > 0) sb_append_len(&out, buf, (size_t)n);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        free(out.data);
        return tool_result_error("read: %s", strerror(errno));
    }
    if (!out.data) return tool_result_ok("no output yet");
    return (ToolResult){out.data, 1};
}

static ToolResult tool_list_processes(cJSON *args) {
    (void)args;
    StrBuf out = {0};
    int i;
    for (i = 0; i < MAX_PROCS; i++) if (g_procs[i].pid) sb_appendf(&out, "%d\t%s\n", (int)g_procs[i].pid, g_procs[i].command);
    if (!out.data) return tool_result_ok("");
    return (ToolResult){out.data, 1};
}

static ToolResult tool_get_process_status(cJSON *args) {
    int pid = tools_json_get_int(args, "pid", -1);
    ProcEntry *p = find_proc((pid_t)pid);
    if (!p) return tool_result_ok("not found");
    if (!p->active) return tool_result_ok("exited: unknown");
    if (kill((pid_t)pid, 0) == 0 || errno == EPERM) return tool_result_ok("running");
    if (errno == ESRCH) {
        p->active = 0;
        p->exit_code = -1;
        return tool_result_ok("exited: unknown");
    }
    return tool_result_error("status: %s", strerror(errno));
}

static void reg(const char *name, const char *desc, ToolFn fn, cJSON *s) { tools_register(name, desc, fn, s); }

void tools_shell_register(void) {
    cJSON *s;
    s = tools_schema_object(); tools_schema_add_string(s, "command", "Shell command", 1); tools_schema_add_integer(s, "timeout_seconds", "Timeout in seconds", 0); reg("bash", "Run a shell command with timeout", tool_bash, s);
    s = tools_schema_object(); tools_schema_add_string(s, "command", "Shell command to run in background", 1); reg("background_process", "Launch tracked background process", tool_background_process, s);
    s = tools_schema_object(); tools_schema_add_integer(s, "pid", "Tracked PID", 1); tools_schema_add_string(s, "signal", "Signal name", 0); reg("kill_process", "Signal a tracked process", tool_kill_process, s);
    s = tools_schema_object(); tools_schema_add_integer(s, "pid", "Tracked PID", 1); reg("read_process_output", "Read available background output", tool_read_process_output, s);
    s = tools_schema_object(); reg("list_processes", "List tracked background processes", tool_list_processes, s);
    s = tools_schema_object(); tools_schema_add_integer(s, "pid", "Tracked PID", 1); reg("get_process_status", "Return tracked process status", tool_get_process_status, s);
}
