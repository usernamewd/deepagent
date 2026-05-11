/*
 * tools_memory.c
 *
 * Persistent JSON key-value memory. The default path is
 * ~/.deepagent/memory.json, overrideable by --memory-file. Updates are atomic:
 * write a temporary JSON file and rename it over the original.
 */

#define _POSIX_C_SOURCE 200809L
#include "tools_memory.h"

#include "config.h"
#include "tools_fs.h"
#include "ui.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *memory_file_path(void) {
    const Config *cfg = config_global();
    const char *home;
    char *path;
    if (cfg && cfg->memory_file) return strdup(cfg->memory_file);
    home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : ".";
    }
    path = malloc(strlen(home) + strlen("/.deepagent/memory.json") + 1);
    if (!path) return NULL;
    sprintf(path, "%s/.deepagent/memory.json", home);
    return path;
}

static int ensure_memory_parent(const char *path) {
    char *copy = strdup(path);
    char *slash;
    int rc = 0;
    if (!copy) return -1;
    slash = strrchr(copy, '/');
    if (slash) {
        *slash = '\0';
        if (*copy) rc = fs_mkdir_p(copy);
    }
    free(copy);
    return rc;
}

static cJSON *load_memory(char **path_out) {
    char *path = memory_file_path();
    FILE *f;
    long size;
    char *buf;
    cJSON *json;
    if (!path) return NULL;
    if (ensure_memory_parent(path) < 0) { free(path); return NULL; }
    f = fopen(path, "rb");
    if (!f) {
        f = fopen(path, "wb");
        if (!f) { free(path); return NULL; }
        fwrite("{}", 1, 2, f);
        fclose(f);
        f = fopen(path, "rb");
        if (!f) { free(path); return NULL; }
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); free(path); return NULL;
    }
    buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); free(path); return NULL; }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size && ferror(f)) {
        free(buf); fclose(f); free(path); return NULL;
    }
    fclose(f);
    buf[size] = '\0';
    json = cJSON_Parse(buf);
    free(buf);
    if (!cJSON_IsObject(json)) {
        cJSON_Delete(json);
        json = cJSON_CreateObject();
    }
    *path_out = path;
    return json;
}

static int save_memory(const char *path, cJSON *json) {
    char *printed = cJSON_Print(json);
    char *tmp;
    FILE *f;
    int ok = 0;
    if (!printed) return -1;
    tmp = malloc(strlen(path) + 5);
    if (!tmp) { free(printed); return -1; }
    sprintf(tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (f && fwrite(printed, 1, strlen(printed), f) == strlen(printed) && fclose(f) == 0 && rename(tmp, path) == 0) ok = 1;
    else if (f) fclose(f);
    if (!ok) unlink(tmp);
    free(tmp); free(printed);
    return ok ? 0 : -1;
}

static ToolResult tool_remember(cJSON *args) {
    const char *key = tools_json_get_string(args, "key", NULL);
    const char *value = tools_json_get_string(args, "value", NULL);
    char *path = NULL;
    cJSON *mem;
    if (!key || !value) return tool_result_error("key and value required");
    mem = load_memory(&path);
    if (!mem) return tool_result_error("load memory failed");
    cJSON_DeleteItemFromObject(mem, key);
    cJSON_AddStringToObject(mem, key, value);
    if (save_memory(path, mem) < 0) {
        cJSON_Delete(mem); free(path);
        return tool_result_error("save memory: %s", strerror(errno));
    }
    cJSON_Delete(mem); free(path);
    return tool_result_ok("stored");
}

static ToolResult tool_recall(cJSON *args) {
    const char *key = tools_json_get_string(args, "key", NULL);
    char *path = NULL;
    cJSON *mem, *v;
    ToolResult r;
    if (!key) return tool_result_error("key required");
    mem = load_memory(&path);
    if (!mem) return tool_result_error("load memory failed");
    v = cJSON_GetObjectItemCaseSensitive(mem, key);
    if (cJSON_IsString(v)) r = tool_result_ok("%s", v->valuestring);
    else r = tool_result_ok("not found");
    cJSON_Delete(mem); free(path);
    return r;
}

static ToolResult tool_forget(cJSON *args) {
    const char *key = tools_json_get_string(args, "key", NULL);
    char *path = NULL;
    cJSON *mem, *v;
    if (!key) return tool_result_error("key required");
    mem = load_memory(&path);
    if (!mem) return tool_result_error("load memory failed");
    v = cJSON_GetObjectItemCaseSensitive(mem, key);
    if (!v) { cJSON_Delete(mem); free(path); return tool_result_ok("key not found"); }
    cJSON_DeleteItemFromObject(mem, key);
    if (save_memory(path, mem) < 0) {
        cJSON_Delete(mem); free(path);
        return tool_result_error("save memory failed");
    }
    cJSON_Delete(mem); free(path);
    return tool_result_ok("forgotten");
}

static ToolResult tool_list_memory(cJSON *args) {
    char *path = NULL;
    cJSON *mem, *it;
    size_t cap = 1024, len = 0;
    char *out = malloc(cap);
    (void)args;
    if (!out) return tool_result_error("out of memory");
    out[0] = '\0';
    mem = load_memory(&path);
    if (!mem) { free(out); return tool_result_error("load memory failed"); }
    cJSON_ArrayForEach(it, mem) {
        if (it->string && cJSON_IsString(it)) {
            size_t need = strlen(it->string) + strlen(it->valuestring) + 8;
            if (len + need + 1 > cap) {
                char *tmp;
                while (len + need + 1 > cap) cap *= 2;
                tmp = realloc(out, cap);
                if (!tmp) { free(out); cJSON_Delete(mem); free(path); return tool_result_error("out of memory"); }
                out = tmp;
            }
            snprintf(out + len, cap - len, "%s\t%s\n", it->string, it->valuestring);
            len = strlen(out);
        }
    }
    cJSON_Delete(mem); free(path);
    return (ToolResult){out, 1};
}

static ToolResult tool_clear_memory(cJSON *args) {
    char *path = memory_file_path();
    FILE *f;
    const Config *cfg = config_global();
    (void)args;
    if (!path) return tool_result_error("memory path failed");
    if (!cfg || !cfg->auto_approve) {
        if (!ui_confirm("Clear all DeepAgent memory?")) { free(path); return tool_result_error("clear cancelled"); }
    }
    if (ensure_memory_parent(path) < 0) { free(path); return tool_result_error("create parent failed"); }
    f = fopen(path, "wb");
    if (!f) { free(path); return tool_result_error("open memory: %s", strerror(errno)); }
    if (fwrite("{}", 1, 2, f) != 2 || fclose(f) != 0) {
        free(path);
        return tool_result_error("write memory failed");
    }
    free(path);
    return tool_result_ok("cleared");
}

static void reg(const char *name, const char *desc, ToolFn fn, cJSON *s) { tools_register(name, desc, fn, s); }

void tools_memory_register(void) {
    cJSON *s;
    s = tools_schema_object(); tools_schema_add_string(s, "key", "Memory key", 1); tools_schema_add_string(s, "value", "Memory value", 1); reg("remember", "Store a persistent memory value", tool_remember, s);
    s = tools_schema_object(); tools_schema_add_string(s, "key", "Memory key", 1); reg("recall", "Recall a persistent memory value", tool_recall, s);
    s = tools_schema_object(); tools_schema_add_string(s, "key", "Memory key", 1); reg("forget", "Forget a memory key", tool_forget, s);
    s = tools_schema_object(); reg("list_memory", "List persistent memory entries", tool_list_memory, s);
    s = tools_schema_object(); reg("clear_memory", "Clear all memory values", tool_clear_memory, s);
}
