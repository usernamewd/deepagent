/*
 * tools_system.c
 *
 * System and environment inspection tools implemented with POSIX/Linux APIs
 * and direct reads from /proc and /etc.
 */

#define _POSIX_C_SOURCE 200809L
#include "tools_system.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

static ToolResult tool_get_env(cJSON *args) {
    const char *name = tools_json_get_string(args, "variable_name", NULL);
    const char *v;
    if (!name) return tool_result_error("variable_name required");
    v = getenv(name);
    return tool_result_ok("%s", v ? v : "not set");
}

static ToolResult tool_set_env(cJSON *args) {
    const char *name = tools_json_get_string(args, "variable_name", NULL);
    const char *value = tools_json_get_string(args, "value", NULL);
    if (!name || !value) return tool_result_error("variable_name and value required");
    if (setenv(name, value, 1) < 0) return tool_result_error("setenv: %s", strerror(errno));
    return tool_result_ok("ok");
}

static char *first_matching_value(const char *path, const char *prefix) {
    FILE *f = fopen(path, "r");
    char *line = NULL;
    size_t cap = 0, plen = strlen(prefix);
    if (!f) return strdup("");
    while (getline(&line, &cap, f) != -1) {
        if (strncmp(line, prefix, plen) == 0) {
            char *v = strdup(line + plen);
            char *nl;
            free(line);
            fclose(f);
            if (!v) return strdup("");
            nl = strchr(v, '\n');
            if (nl) *nl = '\0';
            if (*v == '"') {
                memmove(v, v + 1, strlen(v));
                nl = strrchr(v, '"');
                if (nl) *nl = '\0';
            }
            return v;
        }
    }
    free(line);
    fclose(f);
    return strdup("");
}

static long meminfo_value_kb(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r");
    char name[64], unit[32];
    long val;
    if (!f) return 0;
    while (fscanf(f, "%63s %ld %31s", name, &val, unit) == 3) {
        if (strcmp(name, key) == 0) {
            fclose(f);
            return val;
        }
    }
    fclose(f);
    return 0;
}

static ToolResult tool_system_info(cJSON *args) {
    (void)args;
    struct utsname un;
    struct statvfs sv;
    char *os = first_matching_value("/etc/os-release", "PRETTY_NAME=");
    char *cpu = first_matching_value("/proc/cpuinfo", "model name\t: ");
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    long ram_total = meminfo_value_kb("MemTotal:") / 1024;
    long ram_free = meminfo_value_kb("MemAvailable:") / 1024;
    cJSON *obj = cJSON_CreateObject();
    char *json;
    if (uname(&un) < 0) memset(&un, 0, sizeof(un));
    if (statvfs("/", &sv) < 0) memset(&sv, 0, sizeof(sv));
    cJSON_AddStringToObject(obj, "os", os);
    cJSON_AddStringToObject(obj, "kernel", un.release);
    cJSON_AddStringToObject(obj, "arch", un.machine);
    cJSON_AddStringToObject(obj, "cpu_model", cpu);
    cJSON_AddNumberToObject(obj, "cpu_cores", (double)cores);
    cJSON_AddNumberToObject(obj, "ram_total_mb", (double)ram_total);
    cJSON_AddNumberToObject(obj, "ram_free_mb", (double)ram_free);
    cJSON_AddNumberToObject(obj, "disk_used", (double)((sv.f_blocks - sv.f_bfree) * sv.f_frsize));
    cJSON_AddNumberToObject(obj, "disk_total", (double)(sv.f_blocks * sv.f_frsize));
    json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    free(os); free(cpu);
    return json ? (ToolResult){json, 1} : tool_result_error("out of memory");
}

static ToolResult tool_which(cJSON *args) {
    const char *bin = tools_json_get_string(args, "binary_name", NULL);
    char *path, *save, *tok;
    if (!bin) return tool_result_error("binary_name required");
    path = strdup(getenv("PATH") ? getenv("PATH") : "");
    if (!path) return tool_result_error("out of memory");
    for (tok = strtok_r(path, ":", &save); tok; tok = strtok_r(NULL, ":", &save)) {
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", tok, bin);
        if (access(full, X_OK) == 0) {
            free(path);
            return tool_result_ok("%s", full);
        }
    }
    free(path);
    return tool_result_ok("not found");
}

static ToolResult tool_get_working_directory(cJSON *args) {
    char buf[4096];
    (void)args;
    return getcwd(buf, sizeof(buf)) ? tool_result_ok("%s", buf) : tool_result_error("getcwd: %s", strerror(errno));
}

static ToolResult tool_set_working_directory(cJSON *args) {
    const char *path = tools_json_get_string(args, "path", NULL);
    if (!path) return tool_result_error("path required");
    return chdir(path) == 0 ? tool_result_ok("ok") : tool_result_error("chdir: %s", strerror(errno));
}

static ToolResult tool_list_path(cJSON *args) {
    const char *p = getenv("PATH");
    char *copy, *save, *tok;
    size_t len = 1;
    char *out;
    (void)args;
    if (!p) return tool_result_ok("");
    copy = strdup(p);
    if (!copy) return tool_result_error("out of memory");
    for (tok = strtok_r(copy, ":", &save); tok; tok = strtok_r(NULL, ":", &save)) len += strlen(tok) + 1;
    free(copy);
    out = malloc(len);
    if (!out) return tool_result_error("out of memory");
    out[0] = '\0';
    copy = strdup(p);
    if (!copy) { free(out); return tool_result_error("out of memory"); }
    for (tok = strtok_r(copy, ":", &save); tok; tok = strtok_r(NULL, ":", &save)) {
        strncat(out, tok, len - strlen(out) - 1);
        strncat(out, "\n", len - strlen(out) - 1);
    }
    free(copy);
    return (ToolResult){out, 1};
}

static ToolResult tool_get_hostname(cJSON *args) {
    char buf[256];
    (void)args;
    return gethostname(buf, sizeof(buf)) == 0 ? tool_result_ok("%s", buf) : tool_result_error("gethostname: %s", strerror(errno));
}

static ToolResult tool_get_username(cJSON *args) {
    struct passwd *pw;
    (void)args;
    pw = getpwuid(getuid());
    return pw ? tool_result_ok("%s", pw->pw_name) : tool_result_error("getpwuid failed");
}

static void reg(const char *name, const char *desc, ToolFn fn, cJSON *s) { tools_register(name, desc, fn, s); }

void tools_system_register(void) {
    cJSON *s;
    s = tools_schema_object(); tools_schema_add_string(s, "variable_name", "Environment variable name", 1); reg("get_env", "Get environment variable", tool_get_env, s);
    s = tools_schema_object(); tools_schema_add_string(s, "variable_name", "Environment variable name", 1); tools_schema_add_string(s, "value", "Value to set", 1); reg("set_env", "Set environment variable", tool_set_env, s);
    s = tools_schema_object(); reg("system_info", "Return system info JSON", tool_system_info, s);
    s = tools_schema_object(); tools_schema_add_string(s, "binary_name", "Executable name", 1); reg("which", "Find executable in PATH", tool_which, s);
    s = tools_schema_object(); reg("get_working_directory", "Get current working directory", tool_get_working_directory, s);
    s = tools_schema_object(); tools_schema_add_string(s, "path", "Directory path", 1); reg("set_working_directory", "Change working directory", tool_set_working_directory, s);
    s = tools_schema_object(); reg("list_path", "List PATH directories", tool_list_path, s);
    s = tools_schema_object(); reg("get_hostname", "Get hostname", tool_get_hostname, s);
    s = tools_schema_object(); reg("get_username", "Get username", tool_get_username, s);
}
