#ifndef VMFC_UTIL_H
#define VMFC_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t s);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

int mkdir_p(const char *path, mode_t mode);
int path_exists(const char *path);
int is_directory(const char *path);

void path_join(char *out, size_t n, const char *a, const char *b);
const char *path_basename(const char *path);
void path_dirname(const char *path, char *out, size_t n);

char *read_text_file(const char *path);
int write_text_file(const char *path, const char *text);
int append_text_file(const char *path, const char *text);

char *str_trim(char *s);
int str_ieq(const char *a, const char *b);
int str_starts_with(const char *s, const char *prefix);
int str_ends_with(const char *s, const char *suffix);

int parse_size_mib(const char *text, uint64_t *out_mib);
int parse_u32(const char *text, uint32_t *out);
void format_gib(uint64_t mib, char *out, size_t n);
uint64_t now_usec(void);
void random_bytes(void *out, size_t n);
void random_hex(char *out, size_t n, size_t bytes);

int rm_rf(const char *path);
int copy_file(const char *src, const char *dst);
int touch_file(const char *path);

void vmfc_log(const char *fmt, ...);

#endif
