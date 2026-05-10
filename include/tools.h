#ifndef DEEPAGENT_TOOLS_H
#define DEEPAGENT_TOOLS_H

#include "cJSON.h"

typedef struct {
    char *output;
    int success;
} ToolResult;

typedef ToolResult (*ToolFn)(cJSON *args);

typedef struct {
    const char *name;
    const char *description;
    ToolFn fn;
    cJSON *schema;
} ToolDef;

void tools_register_all(void);
ToolResult tools_dispatch(const char *name, cJSON *args);
cJSON *tools_build_nim_array(void);
void tools_free_all(void);
int tools_register(const char *name, const char *description, ToolFn fn, cJSON *schema);
ToolResult tool_result_ok(const char *fmt, ...);
ToolResult tool_result_error(const char *fmt, ...);
char *tools_json_get_string(cJSON *args, const char *key, const char *fallback);
int tools_json_get_int(cJSON *args, const char *key, int fallback);
int tools_json_get_bool(cJSON *args, const char *key, int fallback);
cJSON *tools_schema_object(void);
void tools_schema_add_string(cJSON *schema, const char *name, const char *description, int required);
void tools_schema_add_integer(cJSON *schema, const char *name, const char *description, int required);
void tools_schema_add_boolean(cJSON *schema, const char *name, const char *description, int required);

#endif
