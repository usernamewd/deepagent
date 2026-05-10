/*
 * tools.c
 *
 * Master tool registry for DeepAgent. The single mutable registry array stores
 * tool metadata, dispatch functions, and cJSON schemas used to build the NIM
 * OpenAI-compatible function-calling payload.
 */

#define _POSIX_C_SOURCE 200809L
#include "tools.h"

#include "subagent.h"
#include "tools_code.h"
#include "tools_fs.h"
#include "tools_memory.h"
#include "tools_network.h"
#include "tools_shell.h"
#include "tools_system.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOOLS 128

static ToolDef g_tools[MAX_TOOLS];
static size_t g_tool_count;

static char *vfmt_alloc(const char *fmt, va_list ap) {
    va_list cp;
    int n;
    char *buf;
    va_copy(cp, ap);
    n = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (n < 0) {
        return NULL;
    }
    buf = malloc((size_t)n + 1);
    if (!buf) {
        return NULL;
    }
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    return buf;
}

ToolResult tool_result_ok(const char *fmt, ...) {
    va_list ap;
    ToolResult r;
    va_start(ap, fmt);
    r.output = vfmt_alloc(fmt, ap);
    va_end(ap);
    r.success = r.output != NULL;
    if (!r.output) {
        r.output = strdup("Error: out of memory");
        r.success = 0;
    }
    return r;
}

ToolResult tool_result_error(const char *fmt, ...) {
    va_list ap;
    char *msg;
    ToolResult r;
    va_start(ap, fmt);
    msg = vfmt_alloc(fmt, ap);
    va_end(ap);
    if (!msg) {
        msg = strdup("out of memory");
    }
    if (!msg) {
        r.output = NULL;
    } else {
        size_t n = strlen(msg) + 8;
        r.output = malloc(n);
        if (r.output) {
            snprintf(r.output, n, "Error: %s", msg);
        }
    }
    free(msg);
    r.success = 0;
    return r;
}

int tools_register(const char *name, const char *description, ToolFn fn, cJSON *schema) {
    if (g_tool_count >= MAX_TOOLS || !name || !description || !fn || !schema) {
        cJSON_Delete(schema);
        return 0;
    }
    g_tools[g_tool_count].name = name;
    g_tools[g_tool_count].description = description;
    g_tools[g_tool_count].fn = fn;
    g_tools[g_tool_count].schema = schema;
    g_tool_count++;
    return 1;
}

void tools_register_all(void) {
    if (g_tool_count) {
        return;
    }
    tools_fs_register();
    tools_code_register();
    tools_shell_register();
    tools_network_register();
    tools_system_register();
    tools_memory_register();
    {
        cJSON *schema = tools_schema_object();
        tools_schema_add_string(schema, "task", "Task for the child agent to complete", 1);
        tools_schema_add_string(schema, "context", "Optional context to inject as a system message", 0);
        tools_schema_add_string(schema, "model", "Optional model override", 0);
        tools_register("spawn_subagent", "Spawn a one-level child ReAct agent for a focused task", tool_spawn_subagent, schema);
    }
}

void tools_free_all(void) {
    size_t i;
    for (i = 0; i < g_tool_count; i++) {
        cJSON_Delete(g_tools[i].schema);
        g_tools[i].schema = NULL;
    }
    g_tool_count = 0;
}

ToolResult tools_dispatch(const char *name, cJSON *args) {
    size_t i;
    if (!name) {
        return tool_result_error("missing tool name");
    }
    for (i = 0; i < g_tool_count; i++) {
        if (strcmp(g_tools[i].name, name) == 0) {
            return g_tools[i].fn(args);
        }
    }
    return tool_result_error("unknown tool: %s", name);
}

cJSON *tools_build_nim_array(void) {
    cJSON *arr = cJSON_CreateArray();
    size_t i;
    if (!arr) {
        return NULL;
    }
    for (i = 0; i < g_tool_count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON_AddStringToObject(fn, "name", g_tools[i].name);
        cJSON_AddStringToObject(fn, "description", g_tools[i].description);
        cJSON_AddItemToObject(fn, "parameters", cJSON_Duplicate(g_tools[i].schema, 1));
        cJSON_AddItemToObject(tool, "function", fn);
        cJSON_AddItemToArray(arr, tool);
    }
    return arr;
}

char *tools_json_get_string(cJSON *args, const char *key, const char *fallback) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(args, key);
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return (char *)fallback;
}

int tools_json_get_int(cJSON *args, const char *key, int fallback) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(args, key);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return fallback;
}

int tools_json_get_bool(cJSON *args, const char *key, int fallback) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(args, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return fallback;
}

cJSON *tools_schema_object(void) {
    cJSON *schema = cJSON_CreateObject();
    cJSON *props = cJSON_CreateObject();
    cJSON *req = cJSON_CreateArray();
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddItemToObject(schema, "properties", props);
    cJSON_AddItemToObject(schema, "required", req);
    return schema;
}

static void schema_add(cJSON *schema, const char *name, const char *type, const char *description, int required) {
    cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    cJSON *req = cJSON_GetObjectItemCaseSensitive(schema, "required");
    cJSON *prop = cJSON_CreateObject();
    cJSON_AddStringToObject(prop, "type", type);
    cJSON_AddStringToObject(prop, "description", description);
    cJSON_AddItemToObject(props, name, prop);
    if (required) {
        cJSON_AddItemToArray(req, cJSON_CreateString(name));
    }
}

void tools_schema_add_string(cJSON *schema, const char *name, const char *description, int required) {
    schema_add(schema, name, "string", description, required);
}

void tools_schema_add_integer(cJSON *schema, const char *name, const char *description, int required) {
    schema_add(schema, name, "integer", description, required);
}

void tools_schema_add_boolean(cJSON *schema, const char *name, const char *description, int required) {
    schema_add(schema, name, "boolean", description, required);
}
