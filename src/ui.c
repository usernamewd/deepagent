/*
 * ui.c
 *
 * Centralized terminal UI. All user-facing output flows through this module so
 * color, prefixes, prompts, and multiline input are consistent.
 */

#define _POSIX_C_SOURCE 200809L
#include "ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_color_enabled = 1;

static const char *color(const char *code) {
    return g_color_enabled ? code : "";
}

void ui_set_color_enabled(int enabled) {
    g_color_enabled = enabled;
}

static void print_prefixed(const char *prefix, const char *c, const char *text) {
    printf("%s%s%s %s\n", color(c), prefix, color("\033[0m"), text ? text : "");
    fflush(stdout);
}

void ui_print_agent(const char *text) {
    print_prefixed("[DeepAgent]", "\033[36m", text);
}

void ui_print_tool_call(const char *name, const char *args_json) {
    char *line;
    size_t n = strlen(name ? name : "") + strlen(args_json ? args_json : "") + 4;
    line = malloc(n);
    if (!line) {
        print_prefixed("[Tool]", "\033[33m", name);
        return;
    }
    snprintf(line, n, "%s(%s)", name ? name : "", args_json ? args_json : "");
    print_prefixed("[Tool]", "\033[33m", line);
    free(line);
}

void ui_print_tool_result(const ToolResult *r) {
    const char *out = (r && r->output) ? r->output : "";
    char preview[256];
    size_t i;
    for (i = 0; i < sizeof(preview) - 1 && out[i] && i < 200; i++) {
        preview[i] = (out[i] == '\n' || out[i] == '\r') ? ' ' : out[i];
    }
    preview[i] = '\0';
    if (out[i]) {
        strncat(preview, "...", sizeof(preview) - strlen(preview) - 1);
    }
    print_prefixed("[Result]", "\033[32m", preview);
}

static void vprint_fmt(const char *prefix, const char *c, const char *fmt, va_list ap) {
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    print_prefixed(prefix, c, buf);
}

void ui_print_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprint_fmt("[Error]", "\033[31m", fmt, ap);
    va_end(ap);
}

void ui_print_subagent(const char *task) {
    print_prefixed("[SubAgent]", "\033[35m", task);
}

void ui_print_system(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprint_fmt("[System]", "\033[34m", fmt, ap);
    va_end(ap);
}

int ui_confirm(const char *question) {
    char line[64];
    printf("%s[System]%s %s [y/N]: ", color("\033[34m"), color("\033[0m"), question ? question : "Confirm?");
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) {
        return 0;
    }
    return tolower((unsigned char)line[0]) == 'y';
}

char *ui_read_multiline(const char *prompt) {
    char *buf = NULL;
    size_t cap = 0;
    size_t len = 0;
    char *line = NULL;
    size_t linecap = 0;
    ssize_t got;

    printf("%s[System]%s %s\n", color("\033[34m"), color("\033[0m"), prompt ? prompt : "Enter prompt; '.' ends input:");
    fflush(stdout);

    while ((got = getline(&line, &linecap, stdin)) != -1) {
        if ((got == 2 && line[0] == '.' && line[1] == '\n') || (got == 1 && line[0] == '.')) {
            break;
        }
        if (len + (size_t)got + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 1024;
            char *tmp;
            while (ncap < len + (size_t)got + 1) {
                ncap *= 2;
            }
            tmp = realloc(buf, ncap);
            if (!tmp) {
                free(buf);
                free(line);
                return NULL;
            }
            buf = tmp;
            cap = ncap;
        }
        memcpy(buf + len, line, (size_t)got);
        len += (size_t)got;
        buf[len] = '\0';
    }
    free(line);
    if (!buf) {
        buf = malloc(1);
        if (buf) {
            buf[0] = '\0';
        }
    }
    return buf;
}
