/*
 * tools_network.c
 *
 * libcurl-powered network tools for GET, POST, downloads, and URL encoding.
 * HTTP responses are returned as JSON with status, body, and content type.
 */

#define _POSIX_C_SOURCE 200809L
#include "tools_network.h"

#include "tools_fs.h"

#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static struct curl_slist *headers_from_json(const char *headers_json) {
    cJSON *obj, *it;
    struct curl_slist *headers = NULL;
    if (!headers_json || !*headers_json) return NULL;
    obj = cJSON_Parse(headers_json);
    if (!cJSON_IsObject(obj)) {
        cJSON_Delete(obj);
        return NULL;
    }
    cJSON_ArrayForEach(it, obj) {
        if (cJSON_IsString(it) && it->string) {
            size_t n = strlen(it->string) + strlen(it->valuestring) + 3;
            char *line = malloc(n);
            if (line) {
                snprintf(line, n, "%s: %s", it->string, it->valuestring);
                headers = curl_slist_append(headers, line);
                free(line);
            }
        }
    }
    cJSON_Delete(obj);
    return headers;
}

static ToolResult make_http_json(long status, const char *body, const char *ctype) {
    cJSON *obj = cJSON_CreateObject();
    char *json;
    ToolResult r;
    cJSON_AddNumberToObject(obj, "status", (double)status);
    cJSON_AddStringToObject(obj, "body", body ? body : "");
    cJSON_AddStringToObject(obj, "content_type", ctype ? ctype : "");
    json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    r.output = json ? json : strdup("{\"error\":\"out of memory\"}");
    r.success = json != NULL;
    return r;
}

static ToolResult http_common(cJSON *args, int post) {
    const char *url = tools_json_get_string(args, "url", NULL);
    const char *headers_json = tools_json_get_string(args, "headers_json", NULL);
    const char *body = tools_json_get_string(args, "body", "");
    const char *ctype_arg = tools_json_get_string(args, "content_type", "application/json");
    CURL *curl;
    CURLcode rc;
    struct curl_slist *headers = headers_from_json(headers_json);
    Buffer buf = {0};
    long status = 0;
    char *ctype = NULL;
    if (!url) return tool_result_error("url required");
    curl = curl_easy_init();
    if (!curl) return tool_result_error("curl init failed");
    if (post && ctype_arg) {
        char h[256];
        snprintf(h, sizeof(h), "Content-Type: %s", ctype_arg);
        headers = curl_slist_append(headers, h);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (post) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    }
    rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        ToolResult r = tool_result_error("curl: %s", curl_easy_strerror(rc));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(buf.data);
        return r;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ctype);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    {
        ToolResult r = make_http_json(status, buf.data ? buf.data : "", ctype);
        free(buf.data);
        return r;
    }
}

static ToolResult tool_http_get(cJSON *args) { return http_common(args, 0); }
static ToolResult tool_http_post(cJSON *args) { return http_common(args, 1); }

static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)clientp; (void)ultotal; (void)ulnow;
    if (dltotal > 0) fprintf(stderr, "\rdownload: %lld/%lld bytes", (long long)dlnow, (long long)dltotal);
    return 0;
}

static ToolResult tool_download_file(cJSON *args) {
    const char *url = tools_json_get_string(args, "url", NULL);
    const char *dst = tools_json_get_string(args, "destination_path", NULL);
    CURL *curl;
    FILE *f;
    CURLcode rc;
    double size = 0.0;
    char *err = NULL;
    if (!url || !dst) return tool_result_error("url and destination_path required");
    {
        char *parent_copy = strdup(dst);
        char *slash = parent_copy ? strrchr(parent_copy, '/') : NULL;
        if (slash) {
            *slash = '\0';
            if (*parent_copy && fs_mkdir_p(parent_copy) < 0) {
                free(parent_copy);
                return tool_result_error("create parent: %s", strerror(errno));
            }
        }
        free(parent_copy);
    }
    f = fopen(dst, "wb");
    if (!f) return tool_result_error("open %s: %s", dst, strerror(errno));
    curl = curl_easy_init();
    if (!curl) { fclose(f); return tool_result_error("curl init failed"); }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    rc = curl_easy_perform(curl);
    fprintf(stderr, "\n");
    if (rc != CURLE_OK) err = strdup(curl_easy_strerror(rc));
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD, &size);
    curl_easy_cleanup(curl);
    if (fclose(f) != 0 && !err) err = strdup(strerror(errno));
    if (err) {
        ToolResult r = tool_result_error("download: %s", err);
        free(err);
        return r;
    }
    return tool_result_ok("%.0f bytes downloaded", size);
}

static ToolResult tool_url_encode(cJSON *args) {
    const char *s = tools_json_get_string(args, "string", NULL);
    CURL *curl;
    char *enc;
    ToolResult r;
    if (!s) return tool_result_error("string required");
    curl = curl_easy_init();
    if (!curl) return tool_result_error("curl init failed");
    enc = curl_easy_escape(curl, s, 0);
    r = enc ? tool_result_ok("%s", enc) : tool_result_error("escape failed");
    curl_free(enc);
    curl_easy_cleanup(curl);
    return r;
}

static ToolResult tool_url_decode(cJSON *args) {
    const char *s = tools_json_get_string(args, "string", NULL);
    CURL *curl;
    char *dec;
    int outlen = 0;
    ToolResult r;
    if (!s) return tool_result_error("string required");
    curl = curl_easy_init();
    if (!curl) return tool_result_error("curl init failed");
    dec = curl_easy_unescape(curl, s, 0, &outlen);
    r = dec ? tool_result_ok("%.*s", outlen, dec) : tool_result_error("unescape failed");
    curl_free(dec);
    curl_easy_cleanup(curl);
    return r;
}

static void reg(const char *name, const char *desc, ToolFn fn, cJSON *s) { tools_register(name, desc, fn, s); }

void tools_network_register(void) {
    cJSON *s;
    s = tools_schema_object(); tools_schema_add_string(s, "url", "URL to fetch", 1); tools_schema_add_string(s, "headers_json", "Optional JSON object of headers", 0); reg("http_get", "HTTP GET request", tool_http_get, s);
    s = tools_schema_object(); tools_schema_add_string(s, "url", "URL to post", 1); tools_schema_add_string(s, "body", "Request body", 1); tools_schema_add_string(s, "content_type", "Content-Type header", 0); tools_schema_add_string(s, "headers_json", "Optional JSON object of headers", 0); reg("http_post", "HTTP POST request", tool_http_post, s);
    s = tools_schema_object(); tools_schema_add_string(s, "url", "URL to download", 1); tools_schema_add_string(s, "destination_path", "Destination path", 1); reg("download_file", "Download URL to file", tool_download_file, s);
    s = tools_schema_object(); tools_schema_add_string(s, "string", "String to encode", 1); reg("url_encode", "URL-encode a string", tool_url_encode, s);
    s = tools_schema_object(); tools_schema_add_string(s, "string", "String to decode", 1); reg("url_decode", "URL-decode a string", tool_url_decode, s);
}
