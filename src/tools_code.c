/*
 * tools_code.c
 *
 * Code/text tooling for regex grep, literal replacement, unified diff/patch,
 * line counting, truncation, and insertion. File rewrites use temporary files
 * followed by rename to avoid partial in-place updates.
 */

#define _POSIX_C_SOURCE 200809L
#include "tools_code.h"

#include "tools_fs.h"

#include <errno.h>
#include <limits.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static int sb_append_len(StrBuf *b, const char *s, size_t n) {
    char *tmp;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->len + n + 1) nc *= 2;
        tmp = realloc(b->data, nc);
        if (!tmp) return 0;
        b->data = tmp;
        b->cap = nc;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

static int sb_append(StrBuf *b, const char *s) {
    return sb_append_len(b, s, strlen(s));
}

static int sb_appendf(StrBuf *b, const char *fmt, ...) {
    va_list ap, cp;
    int n;
    char *tmp;
    va_start(ap, fmt);
    va_copy(cp, ap);
    n = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (n < 0) {
        va_end(ap);
        return 0;
    }
    tmp = malloc((size_t)n + 1);
    if (!tmp) {
        va_end(ap);
        return 0;
    }
    vsnprintf(tmp, (size_t)n + 1, fmt, ap);
    va_end(ap);
    n = sb_append(b, tmp);
    free(tmp);
    return n;
}

static ToolResult read_entire(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    long size;
    size_t got;
    if (!f) return tool_result_error("open %s: %s", path, strerror(errno));
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return tool_result_error("seek %s failed", path);
    }
    *out = malloc((size_t)size + 1);
    if (!*out) {
        fclose(f);
        return tool_result_error("out of memory");
    }
    got = fread(*out, 1, (size_t)size, f);
    if (got != (size_t)size && ferror(f)) {
        free(*out);
        fclose(f);
        return tool_result_error("read %s failed", path);
    }
    fclose(f);
    (*out)[got] = '\0';
    *out_len = got;
    return (ToolResult){NULL, 1};
}

static int write_atomic(const char *path, const char *data, size_t len) {
    char tmp[PATH_MAX];
    int fd;
    FILE *f;
    snprintf(tmp, sizeof(tmp), "%s.tmpXXXXXX", path);
    fd = mkstemp(tmp);
    if (fd < 0) return -1;
    f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (fwrite(data, 1, len, f) != len || fclose(f) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static ToolResult tool_grep_search(cJSON *args) {
    const char *path = tools_json_get_string(args, "path", NULL);
    const char *pattern = tools_json_get_string(args, "pattern", NULL);
    int context = tools_json_get_int(args, "context_lines", 0);
    FILE *f;
    regex_t re;
    char **lines = NULL;
    size_t count = 0, cap = 0, linecap = 0, i;
    char *line = NULL;
    StrBuf out = {0};
    if (!path || !pattern) return tool_result_error("path and pattern required");
    if (regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB) != 0) return tool_result_error("invalid regex");
    f = fopen(path, "r");
    if (!f) {
        regfree(&re);
        return tool_result_error("open %s: %s", path, strerror(errno));
    }
    while (getline(&line, &linecap, f) != -1) {
        if (count == cap) {
            char **tmp;
            cap = cap ? cap * 2 : 256;
            tmp = realloc(lines, cap * sizeof(*lines));
            if (!tmp) break;
            lines = tmp;
        }
        lines[count++] = strdup(line);
    }
    free(line);
    fclose(f);
    for (i = 0; i < count; i++) {
        if (regexec(&re, lines[i], 0, NULL, 0) == 0) {
            size_t start = i > (size_t)context ? i - (size_t)context : 0;
            size_t end = i + (size_t)context < count ? i + (size_t)context : count - 1;
            size_t j;
            for (j = start; j <= end; j++) sb_appendf(&out, "line_%zu: %s", j + 1, lines[j]);
        }
    }
    for (i = 0; i < count; i++) free(lines[i]);
    free(lines);
    regfree(&re);
    if (!out.data) return tool_result_ok("");
    return (ToolResult){out.data, 1};
}

static ToolResult tool_find_replace(cJSON *args) {
    const char *path = tools_json_get_string(args, "path", NULL);
    const char *old = tools_json_get_string(args, "old_string", NULL);
    const char *new_s = tools_json_get_string(args, "new_string", "");
    int all = tools_json_get_bool(args, "replace_all", 0);
    char *data = NULL;
    size_t len = 0, old_len, new_len, count = 0;
    ToolResult rr;
    StrBuf out = {0};
    char *p, *cur;
    if (!path || !old || !*old) return tool_result_error("path and non-empty old_string required");
    rr = read_entire(path, &data, &len);
    if (!rr.success) return rr;
    old_len = strlen(old);
    new_len = strlen(new_s);
    cur = data;
    while ((p = strstr(cur, old)) != NULL) {
        sb_append_len(&out, cur, (size_t)(p - cur));
        sb_append_len(&out, new_s, new_len);
        cur = p + old_len;
        count++;
        if (!all) break;
    }
    sb_append(&out, cur);
    if (write_atomic(path, out.data ? out.data : "", out.len) < 0) {
        free(data);
        free(out.data);
        return tool_result_error("write %s: %s", path, strerror(errno));
    }
    free(data);
    free(out.data);
    return tool_result_ok("%zu replacements", count);
}

static char *shell_quote(const char *s) {
    StrBuf b = {0};
    sb_append(&b, "'");
    while (*s) {
        if (*s == '\'') sb_append(&b, "'\\''");
        else sb_append_len(&b, s, 1);
        s++;
    }
    sb_append(&b, "'");
    return b.data;
}

static ToolResult popen_capture(const char *cmd) {
    FILE *p = popen(cmd, "r");
    char buf[4096];
    StrBuf out = {0};
    int rc;
    if (!p) return tool_result_error("popen failed: %s", strerror(errno));
    while (fgets(buf, sizeof(buf), p)) sb_append(&out, buf);
    rc = pclose(p);
    (void)rc;
    if (!out.data) return tool_result_ok("");
    return (ToolResult){out.data, 1};
}

static ToolResult tool_diff_files(cJSON *args) {
    const char *a = tools_json_get_string(args, "path_a", NULL);
    const char *b = tools_json_get_string(args, "path_b", NULL);
    char *qa, *qb, *cmd;
    ToolResult r;
    if (!a || !b) return tool_result_error("path_a and path_b required");
    qa = shell_quote(a); qb = shell_quote(b);
    if (!qa || !qb) { free(qa); free(qb); return tool_result_error("out of memory"); }
    cmd = malloc(strlen(qa) + strlen(qb) + 16);
    if (!cmd) { free(qa); free(qb); return tool_result_error("out of memory"); }
    sprintf(cmd, "diff -u %s %s", qa, qb);
    r = popen_capture(cmd);
    free(qa); free(qb); free(cmd);
    return r;
}

static ToolResult tool_patch_file(cJSON *args) {
    const char *path = tools_json_get_string(args, "path", NULL);
    const char *diff = tools_json_get_string(args, "unified_diff", NULL);
    char tmpl[] = "/tmp/deepagent_patch_XXXXXX";
    int fd;
    FILE *f;
    char *qpath, *qtmp, *cmd;
    ToolResult r;
    if (!path || !diff) return tool_result_error("path and unified_diff required");
    fd = mkstemp(tmpl);
    if (fd < 0) return tool_result_error("mkstemp: %s", strerror(errno));
    f = fdopen(fd, "wb");
    if (!f) { close(fd); unlink(tmpl); return tool_result_error("fdopen failed"); }
    if (fwrite(diff, 1, strlen(diff), f) != strlen(diff) || fclose(f) != 0) {
        unlink(tmpl);
        return tool_result_error("write temp diff failed");
    }
    qpath = shell_quote(path); qtmp = shell_quote(tmpl);
    if (!qpath || !qtmp) { free(qpath); free(qtmp); unlink(tmpl); return tool_result_error("out of memory"); }
    cmd = malloc(strlen(qpath) + strlen(qtmp) + 16);
    if (!cmd) { free(qpath); free(qtmp); unlink(tmpl); return tool_result_error("out of memory"); }
    sprintf(cmd, "patch %s %s", qpath, qtmp);
    r = popen_capture(cmd);
    free(qpath); free(qtmp); free(cmd); unlink(tmpl);
    return r;
}

static ToolResult tool_count_lines(cJSON *args) {
    const char *path = tools_json_get_string(args, "path", NULL);
    FILE *f;
    char buf[65536];
    size_t n, count = 0;
    if (!path) return tool_result_error("path required");
    f = fopen(path, "rb");
    if (!f) return tool_result_error("open %s: %s", path, strerror(errno));
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        size_t i;
        for (i = 0; i < n; i++) if (buf[i] == '\n') count++;
    }
    if (ferror(f)) { fclose(f); return tool_result_error("read failed"); }
    fclose(f);
    return tool_result_ok("%zu", count);
}

static ToolResult tool_truncate_file(cJSON *args) {
    const char *path = tools_json_get_string(args, "path", NULL);
    int keep = tools_json_get_int(args, "keep_lines", 0);
    FILE *f;
    char *line = NULL;
    size_t cap = 0;
    int n = 0;
    StrBuf out = {0};
    if (!path || keep < 0) return tool_result_error("path and keep_lines required");
    f = fopen(path, "r");
    if (!f) return tool_result_error("open %s: %s", path, strerror(errno));
    while (n < keep && getline(&line, &cap, f) != -1) {
        sb_append(&out, line);
        n++;
    }
    free(line);
    fclose(f);
    if (write_atomic(path, out.data ? out.data : "", out.len) < 0) {
        free(out.data);
        return tool_result_error("write %s: %s", path, strerror(errno));
    }
    free(out.data);
    return tool_result_ok("kept %d lines", n);
}

static ToolResult tool_insert_lines(cJSON *args) {
    const char *path = tools_json_get_string(args, "path", NULL);
    const char *content = tools_json_get_string(args, "content", NULL);
    int after = tools_json_get_int(args, "after_line", 0);
    FILE *f;
    char *line = NULL;
    size_t cap = 0;
    int n = 0, inserted = 0;
    StrBuf out = {0};
    size_t content_len;
    if (!path || !content || after < 0) return tool_result_error("path, after_line, content required");
    content_len = strlen(content);
    f = fopen(path, "r");
    if (!f) return tool_result_error("open %s: %s", path, strerror(errno));
    if (after == 0) { sb_append(&out, content); if (content_len == 0 || content[content_len - 1] != '\n') sb_append(&out, "\n"); inserted = 1; }
    while (getline(&line, &cap, f) != -1) {
        n++;
        sb_append(&out, line);
        if (!inserted && n == after) {
            sb_append(&out, content);
            if (content_len == 0 || content[content_len - 1] != '\n') sb_append(&out, "\n");
            inserted = 1;
        }
    }
    free(line);
    fclose(f);
    if (!inserted) {
        free(out.data);
        return tool_result_error("after_line beyond EOF");
    }
    if (write_atomic(path, out.data, out.len) < 0) {
        free(out.data);
        return tool_result_error("write %s: %s", path, strerror(errno));
    }
    free(out.data);
    return tool_result_ok("inserted");
}

static void regtool(const char *name, const char *desc, ToolFn fn, cJSON *s) {
    tools_register(name, desc, fn, s);
}

void tools_code_register(void) {
    cJSON *s;
    s = tools_schema_object(); tools_schema_add_string(s, "path", "Path to search", 1); tools_schema_add_string(s, "pattern", "POSIX ERE pattern", 1); tools_schema_add_integer(s, "context_lines", "Context lines", 0); regtool("grep_search", "Search file text with POSIX regex", tool_grep_search, s);
    s = tools_schema_object(); tools_schema_add_string(s, "path", "File path", 1); tools_schema_add_string(s, "old_string", "Literal text to replace", 1); tools_schema_add_string(s, "new_string", "Replacement text", 1); tools_schema_add_boolean(s, "replace_all", "Replace all occurrences", 0); regtool("find_replace", "Literal find and replace", tool_find_replace, s);
    s = tools_schema_object(); tools_schema_add_string(s, "path_a", "First path", 1); tools_schema_add_string(s, "path_b", "Second path", 1); regtool("diff_files", "Return unified diff between files", tool_diff_files, s);
    s = tools_schema_object(); tools_schema_add_string(s, "path", "File to patch", 1); tools_schema_add_string(s, "unified_diff", "Unified diff text", 1); regtool("patch_file", "Apply unified diff using patch", tool_patch_file, s);
    s = tools_schema_object(); tools_schema_add_string(s, "path", "File path", 1); regtool("count_lines", "Count lines efficiently", tool_count_lines, s);
    s = tools_schema_object(); tools_schema_add_string(s, "path", "File path", 1); tools_schema_add_integer(s, "keep_lines", "Lines to keep", 1); regtool("truncate_file", "Keep first N lines", tool_truncate_file, s);
    s = tools_schema_object(); tools_schema_add_string(s, "path", "File path", 1); tools_schema_add_integer(s, "after_line", "Insert after line, 0 prepends", 1); tools_schema_add_string(s, "content", "Content to insert", 1); regtool("insert_lines", "Insert lines into file", tool_insert_lines, s);
}
