/*
 * api.c
 *
 * Provider chat completions HTTP layer. Builds OpenAI-compatible JSON, executes
 * POST requests with libcurl, collects response bodies dynamically, and retries
 * 429/5xx with exponential backoff.
 */

#define _POSIX_C_SOURCE 200809L
#include "api.h"

#include "ui.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    Buffer *b = userdata;
    size_t n = size * nmemb;
    char *tmp;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 8192;
        while (nc < b->len + n + 1) nc *= 2;
        tmp = realloc(b->data, nc);
        if (!tmp) return 0;
        b->data = tmp;
        b->cap = nc;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static cJSON *build_request(const Config *cfg, cJSON *messages, cJSON *tools) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", cfg->model);
    cJSON_AddItemToObject(root, "messages", cJSON_Duplicate(messages, 1));
    cJSON_AddItemToObject(root, "tools", cJSON_Duplicate(tools, 1));
    cJSON_AddStringToObject(root, "tool_choice", "auto");
    cJSON_AddBoolToObject(root, "parallel_tool_calls", 1);
    cJSON_AddBoolToObject(root, "stream", 0);
    cJSON_AddNumberToObject(root, "max_tokens", cfg->max_tokens);
    cJSON_AddNumberToObject(root, "temperature", cfg->temperature);
    return root;
}

static int post_once(const Config *cfg, const char *api_key, const char *body, Buffer *resp, long *status) {
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    CURLcode rc;
    char auth[512];
    if (!curl) return 0;
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth);
    curl_easy_setopt(curl, CURLOPT_URL, config_provider_endpoint(cfg));
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK;
}

static int parse_response(const char *json, ApiResponse *out) {
    cJSON *root = cJSON_Parse(json);
    cJSON *choices, *choice, *finish, *msg, *content, *tcalls;
    if (!root) return 0;
    choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    finish = choice ? cJSON_GetObjectItemCaseSensitive(choice, "finish_reason") : NULL;
    msg = choice ? cJSON_GetObjectItemCaseSensitive(choice, "message") : NULL;
    content = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
    tcalls = msg ? cJSON_GetObjectItemCaseSensitive(msg, "tool_calls") : NULL;
    if (!cJSON_IsString(finish) || !cJSON_IsObject(msg)) {
        cJSON_Delete(root);
        return 0;
    }
    out->finish_reason = strdup(finish->valuestring);
    out->content = cJSON_IsString(content) ? strdup(content->valuestring) : strdup("");
    out->message = cJSON_Duplicate(msg, 1);
    out->tool_calls = cJSON_IsArray(tcalls) ? cJSON_Duplicate(tcalls, 1) : NULL;
    cJSON_Delete(root);
    return out->finish_reason && out->content && out->message;
}

int api_chat_completion(const Config *cfg, const char *api_key, cJSON *messages, cJSON *tools, ApiResponse *out) {
    cJSON *req = build_request(cfg, messages, tools);
    char *body = cJSON_PrintUnformatted(req);
    int delays[] = {0, 2, 4, 8};
    int attempt;
    memset(out, 0, sizeof(*out));
    cJSON_Delete(req);
    if (!body) return 0;
    if (cfg->verbose) ui_print_system("Request JSON: %s", body);
    for (attempt = 0; attempt < 4; attempt++) {
        Buffer resp = {0};
        long status = 0;
        if (delays[attempt] > 0) sleep((unsigned int)delays[attempt]);
        if (!post_once(cfg, api_key, body, &resp, &status)) {
            free(resp.data);
            if (attempt < 3) continue;
            free(body);
            return 0;
        }
        if (cfg->verbose) ui_print_system("Response JSON: %s", resp.data ? resp.data : "");
        if ((status == 429 || status >= 500) && attempt < 3) {
            free(resp.data);
            continue;
        }
        if (status < 200 || status >= 300) {
            ui_print_error("%s API HTTP %ld: %s", config_provider_display_name(cfg), status, resp.data ? resp.data : "");
            free(resp.data);
            free(body);
            return 0;
        }
        out->raw_json = resp.data ? strdup(resp.data) : strdup("");
        if (!parse_response(resp.data ? resp.data : "", out)) {
            api_response_free(out);
            free(resp.data);
            free(body);
            return 0;
        }
        free(resp.data);
        free(body);
        return 1;
    }
    free(body);
    return 0;
}

void api_response_free(ApiResponse *res) {
    if (!res) return;
    free(res->content);
    free(res->finish_reason);
    free(res->raw_json);
    cJSON_Delete(res->message);
    cJSON_Delete(res->tool_calls);
    memset(res, 0, sizeof(*res));
}
