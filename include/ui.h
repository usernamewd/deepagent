#ifndef DEEPAGENT_UI_H
#define DEEPAGENT_UI_H

#include <stdarg.h>
#include "tools.h"

void ui_print_agent(const char *text);
void ui_print_tool_call(const char *name, const char *args_json);
void ui_print_tool_result(const ToolResult *r);
void ui_print_error(const char *fmt, ...);
void ui_print_subagent(const char *task);
void ui_print_system(const char *fmt, ...);
int ui_confirm(const char *question);
void ui_set_color_enabled(int enabled);
char *ui_read_multiline(const char *prompt);

#endif
