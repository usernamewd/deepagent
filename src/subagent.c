/*
 * subagent.c
 *
 * In-process one-level subagent tool. A global depth flag prevents recursive
 * subagent spawning. The child receives a fresh history and returns the final
 * assistant text to the parent agent.
 */

#define _POSIX_C_SOURCE 200809L
#include "subagent.h"

#include "agent.h"
#include "config.h"
#include "ui.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_subagent_depth;
static sig_atomic_t g_subagent_alarm;

static void alarm_handler(int sig) {
    (void)sig;
    g_subagent_alarm = 1;
}

ToolResult tool_spawn_subagent(cJSON *args) {
    const char *task = tools_json_get_string(args, "task", NULL);
    const char *context = tools_json_get_string(args, "context", NULL);
    const char *model = tools_json_get_string(args, "model", NULL);
    const Config *base = config_global();
    Config cfg;
    AgentState child;
    ToolResult r;
    void (*old_handler)(int);
    if (!task) return tool_result_error("task required");
    if (g_subagent_depth > 0) return tool_result_error("subagent depth limit reached");
    ui_print_subagent(task);
    config_init(&cfg);
    if (base) {
        cfg.max_iterations = base->max_iterations;
        cfg.max_tokens = base->max_tokens;
        cfg.temperature = base->temperature;
        cfg.auto_approve = base->auto_approve;
        cfg.subagent_timeout = base->subagent_timeout;
        cfg.color_enabled = base->color_enabled;
        cfg.verbose = base->verbose;
        free(cfg.provider);
        cfg.provider = base->provider ? strdup(base->provider) : NULL;
        free(cfg.memory_file);
        cfg.memory_file = base->memory_file ? strdup(base->memory_file) : NULL;
    }
    if (model && *model) {
        free(cfg.model);
        cfg.model = strdup(model);
    } else if (base && base->model) {
        free(cfg.model);
        cfg.model = strdup(base->model);
    }
    if (context && *context) {
        free(cfg.system_prompt);
        cfg.system_prompt = strdup(context);
    }
    g_subagent_depth++;
    g_subagent_alarm = 0;
    old_handler = signal(SIGALRM, alarm_handler);
    alarm((unsigned int)cfg.subagent_timeout);
    if (!agent_init(&child, &cfg)) {
        r = tool_result_error("subagent init failed");
    } else if (!agent_run_task(&child, task) || g_subagent_alarm) {
        r = tool_result_error(g_subagent_alarm ? "subagent timeout" : "subagent failed");
    } else {
        r = tool_result_ok("%s", child.final_response ? child.final_response : "");
    }
    alarm(0);
    signal(SIGALRM, old_handler);
    agent_free(&child);
    g_subagent_depth--;
    config_free(&cfg);
    return r;
}
