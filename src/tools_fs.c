/*
 * tools_fs.c
 *
 * File-system tools: safe whole-file and range reads, writes with parent
 * creation, directory walks, metadata, recursive search, copy, move, and delete.
 */

#define _POSIX_C_SOURCE 200809L
#include "tools_fs.h"

#include "config.h"
#include "ui.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static int sb_append(StrBuf *b, const char *s) {
    size_t n = strlen(s);
    char *tmp;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->len + n + 1) nc *= 2;
        tmp = realloc(b->data, nc);
        if (!tmp) return 0;
        b->data = tmp;
        b->cap = nc;
    }
    memcpy(b->data + b->len, s, n + 1);
    b->len += n;
    return 1;
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

static char *parent_dir(const char *path) {
    char *copy = strdup(path);
    char *slash;
    if (!copy) return NULL;
    slash = strrchr(copy, '/');
    if (!slash) {
        copy[0] = '.';
        copy[1] = '\0';
    } else if (slash == copy) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    return copy;
}

int fs_mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    char *p;
    size_t len;
    if (!path || !*path) return -1;
    len = strlen(path);
    if (len >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(tmp, path);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) < 0 && errno != EEXIST) return -1;
    return 0;
}

static int ensure_parent(const char *path) {
    char *dir = parent_dir(path);
    int rc;
    if (!dir) return -1;
    rc = fs_mkdir_p(dir);
    free(dir);
    return rc;
}

static ToolResult tool_read_file(cJSON *args) {
    const char *path = tools_json_get_string(args, "path", NULL);
    FILE *f;
    long size;
    char *buf;
    size_t got;
    if (!path) return tool_result_error("path required");
    f = fopen(path, "rb");
    if (!f) return tool_result_error("open %s: %s", path, strerror(errno));
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return tool_result_error("seek %s: %s", path, strerror(errno));
    }
    buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return tool_result_error("out of memory");
    }
    got = fread(buf, 1, (size_t)size, f);
    if (got != (size_t)size && ferror(f)) {
        free(buf);
        fclose(f);
        return tool_result_error("read %s failed", path);
    }
    fclose(f);
    buf[got] = '\0';
    return (ToolResult){buf, 1};
}

static ToolResult write_common(cJSON *args, const char *mode) {
    const char *path = tools_json_get_string(args, "path", NULL);
    const char *content = tools_json_get_string(args, "content", NULL);
    FILE *f;
    size_t n;
    if (!path || !content) return tool_result_error("path and content required");
    if (ensure_parent(path) < 0) return tool_result_error("create parent dirs: %s", strerror(errno));
    f = fopen(path, mode);
    if (!f) return tool_result_error("open %s: %s", path, strerror(errno));
    n = fwrite(content, 1, strlen(content), f);
    if (n != strlen(content) || fclose(f) != 0) {
        return tool_result_error("write %s failed", path);
    }
    return tool_result_ok("%zu bytes written", n);
}

static ToolResult tool_write_file(cJSON *args) { return write_common(args, "wb"); }
static ToolResult tool_append_file(cJSON *args) { return write_common(args, "ab"); }

static ToolResult tool_delete_file(cJSON *args) {
    const char *path = tools_json_get_string(args, "path", NULL);
    const Config *cfg = config_global();
    char q[PATH_MAX + 64];
    if (!path) return tool_result_error("path required");
    if (!cfg || !cfg->auto_approve) {
        snprintf(q, sizeof(q), "Delete file '%s'?", path);
        if (!ui_confirm(q)) return tool_result_error("delete cancelled");
    }
    if (unlink(path) < 0) return tool_result_error("unlink %s: %s", path, strerror(errno));
    return tool_result_ok("deleted");
}

static int list_walk(const char *path, int recursive, StrBuf *out) {
    DIR *dir = opendir(path);
    struct dirent *de;
    if (!dir) return sb_appendf(out, "Error opening %s: %s\n", path, strerror(errno));
    while ((de = readdir(dir)) != NULL) {
        char child[PATH_MAX];
        struct stat st;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        if (lstat(child, &st) < 0) {
            sb_appendf(out, "[?] %s (%s)\n", child, strerror(errno));
            continue;
        }
        sb_appendf(out, "%s %s\n", S_ISDIR(st.st_mode) ? "[D]" : "[F]", child);
        if (recursive && S_ISDIR(st.st_mode)) list_walk(child, recursive, out);
    }
    closedir(dir);
    return 1;
}

static ToolResult tool_list_directory(cJSON *args) {
    const char *path = tools_json_get_string(args, "path", ".");
    int recursive = tools_json_get_bool(args, "recursive", 0);
    StrBuf out = {0};
    if (!list_walk(path, recursive, &out)) {
        free(out.data);
        return tool_result_error("list failed");
    }
    if (!out.data) return tool_result_ok("");
    return (ToolResult){out.data, 1};
}

static const char *file_type(mode_t m) {
    if (S_ISREG(m)) return "file";
    if (S_ISDIR(m)) return "directory";
    if (S_ISLNK(m)) return "symlink";
    if (S_ISCHR(m)) return "char";
    if (S_ISBLK(m)) return "block";
    if (S_ISFIFO(m)) return "fifo";
    if (S_ISSOCK(m)) return "socket";
    return "unknown";
}

static ToolResult tool_stat_file(cJSON *args) {
    const char *path = tools_json_get_string(args, "path", NULL);
    struct stat st;
    char buf[512];
    if (!path) return tool_result_error("path required");
    if (lstat(path, &st) < 0) return tool_result_error("stat %s: %s", path, strerror(errno));
    snprintf(buf, sizeof(buf), "{\"size\":%lld,\"permissions\":\"%04o\",\"atime\":%lld,\"mtime\":%lld,\"ctime\":%lld,\"type\":\"%s\"}",
             (long long)st.st_size, st.st_mode & 07777, (long long)st.st_atime,
             (long long)st.st_mtime, (long long)st.st_ctime, file_type(st.st_mode));
    return tool_result_ok("%s", buf);
}

int fs_copy_file_path(const char *src, const char *dst, char **err) {
    int in = -1, out = -1;
    struct stat st;
    char buf[65536];
    ssize_t n;
    if (stat(src, &st) < 0) goto fail;
    if (ensure_parent(dst) < 0) goto fail;
    in = open(src, O_RDONLY);
    if (in < 0) goto fail;
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
    if (out < 0) goto fail;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t wr = write(out, buf + off, (size_t)(n - off));
            if (wr < 0) goto fail;
            off += wr;
        }
    }
    if (n < 0) goto fail;
    if (close(in) < 0) { in = -1; goto fail; }
    if (close(out) < 0) { out = -1; goto fail; }
    return 0;
fail:
    if (err) {
        size_t len = strlen(strerror(errno)) + 1;
        *err = malloc(len);
        if (*err) snprintf(*err, len, "%s", strerror(errno));
    }
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    return -1;
}

static ToolResult tool_copy_file(cJSON *args) {
    const char *src = tools_json_get_string(args, "src", NULL);
    const char *dst = tools_json_get_string(args, "dst", NULL);
    char *err = NULL;
    if (!src || !dst) return tool_result_error("src and dst required");
    if (fs_copy_file_path(src, dst, &err) < 0) {
        ToolResult r = tool_result_error("copy: %s", err ? err : "failed");
        free(err);
        return r;
    }
    return tool_result_ok("copied");
}

static ToolResult tool_move_file(cJSON *args) {
    const char *src = tools_json_get_string(args, "src", NULL);
    const char *dst = tools_json_get_string(args, "dst", NULL);
    char *err = NULL;
    if (!src || !dst) return tool_result_error("src and dst required");
    if (ensure_parent(dst) < 0) return tool_result_error("create parent dirs: %s", strerror(errno));
    if (rename(src, dst) == 0) return tool_result_ok("moved");
    if (errno != EXDEV) return tool_result_error("rename: %s", strerror(errno));
    if (fs_copy_file_path(src, dst, &err) < 0) {
        ToolResult r = tool_result_error("copy fallback: %s", err ? err : "failed");
        free(err);
        return r;
    }
    if (unlink(src) < 0) return tool_result_error("unlink source: %s", strerror(errno));
    return tool_result_ok("moved");
}

static int search_walk(const char *dir, const char *pattern, StrBuf *out) {
    DIR *d = opendir(dir);
    struct dirent *de;
    if (!d) return 0;
    while ((de = readdir(d)) != NULL) {
        char path[PATH_MAX];
        struct stat st;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (lstat(path, &st) < 0) continue;
        if (fnmatch(pattern, de->d_name, 0) == 0 || fnmatch(pattern, path, 0) == 0) sb_appendf(out, "%s\n", path);
        if (S_ISDIR(st.st_mode)) search_walk(path, pattern, out);
    }
    closedir(d);
    return 1;
}

static ToolResult tool_search_files(cJSON *args) {
    const char *dir = tools_json_get_string(args, "directory", ".");
    const char *pattern = tools_json_get_string(args, "pattern", NULL);
    StrBuf out = {0};
    if (!pattern) return tool_result_error("pattern required");
    if (!search_walk(dir, pattern, &out)) return tool_result_error("open %s: %s", dir, strerror(errno));
    if (!out.data) return tool_result_ok("");
    return (ToolResult){out.data, 1};
}

static ToolResult tool_make_directory(cJSON *args) {
    const char *path = tools_json_get_string(args, "path", NULL);
    if (!path) return tool_result_error("path required");
    if (fs_mkdir_p(path) < 0) return tool_result_error("mkdir -p %s: %s", path, strerror(errno));
    return tool_result_ok("created");
}

static ToolResult tool_read_file_range(cJSON *args) {
    const char *path = tools_json_get_string(args, "path", NULL);
    int start = tools_json_get_int(args, "start_line", 1);
    int end = tools_json_get_int(args, "end_line", start);
    FILE *f;
    char *line = NULL;
    size_t cap = 0;
    ssize_t got;
    int lineno = 0;
    StrBuf out = {0};
    if (!path || start < 1 || end < start) return tool_result_error("invalid path/start_line/end_line");
    f = fopen(path, "r");
    if (!f) return tool_result_error("open %s: %s", path, strerror(errno));
    while ((got = getline(&line, &cap, f)) != -1) {
        (void)got;
        lineno++;
        if (lineno >= start && lineno <= end) sb_append(&out, line);
        if (lineno > end) break;
    }
    free(line);
    fclose(f);
    if (!out.data) return tool_result_ok("");
    return (ToolResult){out.data, 1};
}

static void reg2(const char *name, const char *desc, ToolFn fn, cJSON *s) {
    tools_register(name, desc, fn, s);
}

void tools_fs_register(void) {
    cJSON *s;
    s = tools_schema_object(); tools_schema_add_string(s, "path", "File path", 1); reg2("read_file", "Read entire file contents", tool_read_file, s);
    s = tools_schema_object(); tools_schema_add_string(s, "path", "File path", 1); tools_schema_add_string(s, "content", "Content to write", 1); reg2("write_file", "Create or overwrite a file", tool_write_file, s);
    s = tools_schema_object(); tools_schema_add_string(s, "path", "File path", 1); tools_schema_add_string(s, "content", "Content to append", 1); reg2("append_file", "Append content to a file", tool_append_file, s);
    s = tools_schema_object(); tools_schema_add_string(s, "path", "File path", 1); reg2("delete_file", "Delete a file with confirmation", tool_delete_file, s);
    s = tools_schema_object(); tools_schema_add_string(s, "path", "Directory path", 1); tools_schema_add_boolean(s, "recursive", "Recurse into directories", 0); reg2("list_directory", "List directory entries", tool_list_directory, s);
    s = tools_schema_object(); tools_schema_add_string(s, "path", "Path to stat", 1); reg2("stat_file", "Return JSON file metadata", tool_stat_file, s);
    s = tools_schema_object(); tools_schema_add_string(s, "src", "Source path", 1); tools_schema_add_string(s, "dst", "Destination path", 1); reg2("move_file", "Move or rename a file", tool_move_file, s);
    s = tools_schema_object(); tools_schema_add_string(s, "src", "Source path", 1); tools_schema_add_string(s, "dst", "Destination path", 1); reg2("copy_file", "Copy a file preserving mode", tool_copy_file, s);
    s = tools_schema_object(); tools_schema_add_string(s, "directory", "Root directory", 1); tools_schema_add_string(s, "pattern", "fnmatch pattern", 1); reg2("search_files", "Search file names recursively", tool_search_files, s);
    s = tools_schema_object(); tools_schema_add_string(s, "path", "Directory path", 1); reg2("make_directory", "Create directories recursively", tool_make_directory, s);
    s = tools_schema_object(); tools_schema_add_string(s, "path", "File path", 1); tools_schema_add_integer(s, "start_line", "1-indexed start line", 1); tools_schema_add_integer(s, "end_line", "1-indexed end line", 1); reg2("read_file_range", "Read selected line range", tool_read_file_range, s);
}
