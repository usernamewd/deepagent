#ifndef DEEPAGENT_API_H
#define DEEPAGENT_API_H

#include "cJSON.h"
#include "config.h"

typedef struct {
    char *content;
    char *finish_reason;
    cJSON *message;
    cJSON *tool_calls;
    char *raw_json;
} ApiResponse;

int api_chat_completion(const Config *cfg, const char *api_key, cJSON *messages, cJSON *tools, ApiResponse *out);
void api_response_free(ApiResponse *res);

#endif
