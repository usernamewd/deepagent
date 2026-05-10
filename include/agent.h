#ifndef DEEPAGENT_AGENT_H
#define DEEPAGENT_AGENT_H

#include "cJSON.h"
#include "config.h"

typedef struct {
    const Config *config;
    cJSON *messages;
    int allow_subagents;
    char *final_response;
} AgentState;

int agent_init(AgentState *state, const Config *cfg);
void agent_free(AgentState *state);
int agent_append_message(AgentState *state, const char *role, const char *content);
int agent_run_interactive(AgentState *state);
int agent_run_task(AgentState *state, const char *task);

#endif
