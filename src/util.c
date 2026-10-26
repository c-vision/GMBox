#define _DARWIN_C_SOURCE
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "vmfc: out of memory\n");
        exit(1);
    }
    return p;
}

void *xcalloc(size_t n, size_t s)
{
    void *p = calloc(n ? n : 1, s ? s : 1);
    if (!p) {
        fprintf(stderr, "vmfc: out of memory\n");
        exit(1);
    }
    return p;
}

char *xstrdup(const char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

int mkdir_p(const char *path, mode_t mode)
{
    char tmp[4096];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, mode) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

int path_exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

int is_directory(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void path_join(char *out, size_t n, const char *a, const char *b)
{
    if (!a || !*a)
        snprintf(out, n, "%s", b ? b : "");
    else if (!b || !*b)
        snprintf(out, n, "%s", a);
    else if (a[strlen(a) - 1] == '/')
        snprintf(out, n, "%s%s", a, b);
    else
        snprintf(out, n, "%s/%s", a, b);
}

const char *path_basename(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

void path_dirname(const char *path, char *out, size_t n)
{
    const char *p = strrchr(path, '/');
    if (!p) {
        snprintf(out, n, ".");
        return;
    }
    size_t len = (size_t)(p - path);
    if (len == 0)
        len = 1;
    if (len >= n)
        len = n - 1;
    memcpy(out, path, len);
    out[len] = 0;
}

char *read_text_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = xmalloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

int write_text_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t len = strlen(text);
    int ok = fwrite(text, 1, len, f) == len ? 0 : -1;
    if (fclose(f) != 0)
        ok = -1;
    return ok;
}

int append_text_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "ab");
    if (!f)
        return -1;
    size_t len = strlen(text);
    int ok = fwrite(text, 1, len, f) == len ? 0 : -1;
    if (fclose(f) != 0)
        ok = -1;
    return ok;
}

char *str_trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s)
        return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e))
        *e-- = 0;
    return s;
}

int str_ieq(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

int str_starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int str_ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

int parse_u32(const char *text, uint32_t *out)
{
    if (!text || !*text)
        return -1;
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(text, &end, 10);
    if (errno || !end || *end || v > UINT32_MAX)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

int parse_size_mib(const char *text, uint64_t *out_mib)
{
    if (!text || !*text)
        return -1;
    char *end = NULL;
    errno = 0;
    double v = strtod(text, &end);
    if (errno || end == text || v < 0)
        return -1;
    while (*end && isspace((unsigned char)*end))
        end++;
    uint64_t mult = 1;
    if (*end) {
        if (str_ieq(end, "K") || str_ieq(end, "KB") || str_ieq(end, "KIB")) {
            mult = 1;
            v /= 1024.0;
        } else if (str_ieq(end, "M") || str_ieq(end, "MB") || str_ieq(end, "MIB")) {
            mult = 1;
        } else if (str_ieq(end, "G") || str_ieq(end, "GB") || str_ieq(end, "GIB")) {
            mult = 1024;
        } else if (str_ieq(end, "T") || str_ieq(end, "TB") || str_ieq(end, "TIB")) {
            mult = 1024 * 1024;
        } else {
            return -1;
        }
    }
    double mib = v * (double)mult;
    if (mib < 1 || mib > 1e9)
        return -1;
    *out_mib = (uint64_t)(mib + 0.5);
    return 0;
}

void format_gib(uint64_t mib, char *out, size_t n)
{
    if (mib >= 1024 && mib % 1024 == 0)
        snprintf(out, n, "%llu GB", (unsigned long long)(mib / 1024));
    else
        snprintf(out, n, "%llu MB", (unsigned long long)mib);
}

uint64_t now_usec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void random_bytes(void *out, size_t n)
{
    arc4random_buf(out, n);
}

void random_hex(char *out, size_t n, size_t bytes)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char buf[64];
    if (bytes > sizeof(buf))
        bytes = sizeof(buf);
    if (n < bytes * 2 + 1)
        bytes = (n - 1) / 2;
    arc4random_buf(buf, bytes);
    for (size_t i = 0; i < bytes; i++) {
        out[i * 2] = hex[buf[i] >> 4];
        out[i * 2 + 1] = hex[buf[i] & 15];
    }
    out[bytes * 2] = 0;
}

int rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d)
            return -1;
        struct dirent *e;
        int rc = 0;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char sub[4096];
            path_join(sub, sizeof(sub), path, e->d_name);
            if (rm_rf(sub) != 0)
                rc = -1;
        }
        closedir(d);
        if (rmdir(path) != 0)
            rc = -1;
        return rc;
    }
    return unlink(path);
}

int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in)
        return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    if (ferror(in))
        rc = -1;
    fclose(in);
    if (fclose(out) != 0)
        rc = -1;
    return rc;
}

int touch_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

void vmfc_log(const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fprintf(stderr, "vmfc: %s\n", line);
}
