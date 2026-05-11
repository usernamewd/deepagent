/*
 * agent.c
 *
 * ReAct loop orchestration. The agent stores OpenAI-compatible conversation
 * history, sends full history and tool definitions to the configured provider,
 * dispatches requested tool calls, appends observations, and repeats until a
 * final answer.
 */

#define _POSIX_C_SOURCE 200809L
#include "agent.h"

#include "api.h"
#include "tools.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int agent_init(AgentState *state, const Config *cfg) {
    memset(state, 0, sizeof(*state));
    state->config = cfg;
    state->allow_subagents = 1;
    state->messages = cJSON_CreateArray();
    if (!state->messages) return 0;
    return agent_append_message(state, "system", cfg->system_prompt);
}

void agent_free(AgentState *state) {
    if (!state) return;
    cJSON_Delete(state->messages);
    free(state->final_response);
    memset(state, 0, sizeof(*state));
}

int agent_append_message(AgentState *state, const char *role, const char *content) {
    cJSON *msg = cJSON_CreateObject();
    if (!msg) return 0;
    cJSON_AddStringToObject(msg, "role", role);
    cJSON_AddStringToObject(msg, "content", content ? content : "");
    cJSON_AddItemToArray(state->messages, msg);
    return 1;
}

static int append_assistant_message(AgentState *state, cJSON *message) {
    cJSON *dup = cJSON_Duplicate(message, 1);
    if (!dup) return 0;
    cJSON_AddItemToArray(state->messages, dup);
    return 1;
}

static int append_tool_result(AgentState *state, const char *id, const char *output) {
    cJSON *msg = cJSON_CreateObject();
    if (!msg) return 0;
    cJSON_AddStringToObject(msg, "role", "tool");
    cJSON_AddStringToObject(msg, "tool_call_id", id ? id : "");
    cJSON_AddStringToObject(msg, "content", output ? output : "");
    cJSON_AddItemToArray(state->messages, msg);
    return 1;
}

static int handle_tool_calls(AgentState *state, ApiResponse *res) {
    int i, n;
    if (!append_assistant_message(state, res->message)) return 0;
    n = cJSON_GetArraySize(res->tool_calls);
    for (i = 0; i < n; i++) {
        cJSON *tc = cJSON_GetArrayItem(res->tool_calls, i);
        cJSON *id = cJSON_GetObjectItemCaseSensitive(tc, "id");
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
        cJSON *name = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
        cJSON *argstr = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
        cJSON *args = cJSON_IsString(argstr) ? cJSON_Parse(argstr->valuestring) : cJSON_CreateObject();
        ToolResult tr;
        if (!cJSON_IsObject(args)) {
            cJSON_Delete(args);
            args = cJSON_CreateObject();
        }
        ui_print_tool_call(cJSON_IsString(name) ? name->valuestring : "unknown", cJSON_IsString(argstr) ? argstr->valuestring : "{}");
        tr = tools_dispatch(cJSON_IsString(name) ? name->valuestring : NULL, args);
        ui_print_tool_result(&tr);
        append_tool_result(state, cJSON_IsString(id) ? id->valuestring : "", tr.output ? tr.output : "");
        free(tr.output);
        cJSON_Delete(args);
    }
    return 1;
}

static int react_until_done(AgentState *state) {
    const char *env_var = config_provider_env_var(state->config);
    const char *api_key = getenv(env_var);
    int iter;
    if (!config_api_key_valid(state->config, api_key)) {
        ui_print_error("%s is required for %s%s", env_var, config_provider_display_name(state->config),
                       strcmp(env_var, "NVIDIA_API_KEY") == 0 ? " and should start with nvapi-" : "");
        return 0;
    }
    for (iter = 0; iter < state->config->max_iterations; iter++) {
        cJSON *tool_defs = tools_build_nim_array();
        ApiResponse res;
        if (!tool_defs) return 0;
        if (!api_chat_completion(state->config, api_key, state->messages, tool_defs, &res)) {
            cJSON_Delete(tool_defs);
            ui_print_error("%s API request failed", config_provider_display_name(state->config));
            return 0;
        }
        cJSON_Delete(tool_defs);
        if (strcmp(res.finish_reason, "tool_calls") == 0) {
            int ok = handle_tool_calls(state, &res);
            api_response_free(&res);
            if (!ok) return 0;
            continue;
        }
        if (strcmp(res.finish_reason, "stop") == 0) {
            ui_print_agent(res.content);
            free(state->final_response);
            state->final_response = strdup(res.content ? res.content : "");
            agent_append_message(state, "assistant", res.content ? res.content : "");
            api_response_free(&res);
            return 1;
        }
        ui_print_error("Unsupported finish_reason: %s", res.finish_reason);
        api_response_free(&res);
        return 0;
    }
    ui_print_error("Maximum iterations exceeded");
    return 0;
}

int agent_run_task(AgentState *state, const char *task) {
    if (!agent_append_message(state, "user", task)) return 0;
    return react_until_done(state);
}

int agent_run_interactive(AgentState *state) {
    for (;;) {
        char *input = ui_read_multiline("Enter prompt; '.' alone submits, Ctrl-D exits:");
        if (!input) return 0;
        if (input[0] == '\0') {
            free(input);
            return 1;
        }
        agent_append_message(state, "user", input);
        free(input);
        react_until_done(state);
    }
}
