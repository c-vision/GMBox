#define _DARWIN_C_SOURCE
#include "vmdk.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VMDK_MAGIC 0x564d444bu
#define MAX_EXTENTS 256

enum extent_type {
    EXT_SPARSE = 1,
    EXT_FLAT = 2,
    EXT_ZERO = 3,
};

struct extent {
    char path[4096];
    uint64_t sectors;
    uint64_t offset_sectors;
    enum extent_type type;
};

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const unsigned char *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void set_err(char *err, size_t n, const char *msg, const char *detail)
{
    if (detail)
        snprintf(err, n, "%s: %s", msg, detail);
    else
        snprintf(err, n, "%s", msg);
}

static int parse_extent_line(const char *line, struct extent *ex)
{
    const char *p = line;
    if (str_starts_with(p, "#") || *p == 0)
        return 0;
    if (str_starts_with(p, "RW "))
        p += 3;
    else if (str_starts_with(p, "RDONLY "))
        p += 7;
    else if (str_starts_with(p, "NOACCESS "))
        p += 9;
    else
        return 0;
    while (*p == ' ' || *p == '\t')
        p++;
    char *end = NULL;
    errno = 0;
    unsigned long long sectors = strtoull(p, &end, 10);
    if (errno || end == p)
        return 0;
    p = end;
    while (*p == ' ' || *p == '\t')
        p++;
    char type[32];
    size_t ti = 0;
    while (*p && !isspace((unsigned char)*p) && ti + 1 < sizeof(type))
        type[ti++] = *p++;
    type[ti] = 0;
    while (*p == ' ' || *p == '\t')
        p++;
    const char *q1 = strchr(p, '"');
    if (!q1)
        return 0;
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2)
        return 0;
    size_t nlen = (size_t)(q2 - q1 - 1);
    if (nlen >= sizeof(ex->path))
        return -1;
    memcpy(ex->path, q1 + 1, nlen);
    ex->path[nlen] = 0;
    p = q2 + 1;
    while (*p == ' ' || *p == '\t')
        p++;
    ex->offset_sectors = 0;
    if (*p)
        ex->offset_sectors = strtoull(p, NULL, 10);
    ex->sectors = sectors;
    if (str_ieq(type, "SPARSE"))
        ex->type = EXT_SPARSE;
    else if (str_ieq(type, "FLAT"))
        ex->type = EXT_FLAT;
    else if (str_ieq(type, "ZERO"))
        ex->type = EXT_ZERO;
    else
        return -1;
    return 1;
}

static int load_extents(const char *path, struct extent *extents, int *count,
                        char *err, size_t errlen);
static int load_extents(const char *path, struct extent *extents, int *count,
                        char *err, size_t errlen)
{
    *count = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_err(err, errlen, "impossibile aprire", strerror(errno));
        return -1;
    }
    unsigned char hdr[512];
    ssize_t got = pread(fd, hdr, sizeof(hdr), 0);
    if (got < 32) {
        close(fd);
        set_err(err, errlen, "file VMDK troppo corto", NULL);
        return -1;
    }
    if (rd32(hdr) == VMDK_MAGIC) {
        close(fd);
        struct extent *ex = &extents[0];
        snprintf(ex->path, sizeof(ex->path), "%s", path);
        ex->sectors = rd64(hdr + 12);
        ex->offset_sectors = 0;
        ex->type = EXT_SPARSE;
        *count = 1;
        return 0;
    }
    close(fd);

    char *text = read_text_file(path);
    if (!text) {
        set_err(err, errlen, "impossibile leggere il descriptor", strerror(errno));
        return -1;
    }
    if (!strstr(text, "Disk DescriptorFile")) {
        free(text);
        set_err(err, errlen, "formato VMDK non riconosciuto", NULL);
        return -1;
    }
    char dir[4096];
    path_dirname(path, dir, sizeof(dir));
    char *save = NULL;
    char *line = strtok_r(text, "\n", &save);
    while (line && *count < MAX_EXTENTS) {
        char *t = str_trim(line);
        struct extent ex;
        memset(&ex, 0, sizeof(ex));
        int rc = parse_extent_line(t, &ex);
        if (rc < 0) {
            free(text);
            set_err(err, errlen, "extent VMDK non supportato", t);
            return -1;
        }
        if (rc > 0) {
            char full[4096];
            if (ex.path[0] == '/')
                snprintf(full, sizeof(full), "%s", ex.path);
            else
                path_join(full, sizeof(full), dir, ex.path);
            snprintf(ex.path, sizeof(ex.path), "%s", full);
            extents[(*count)++] = ex;
        }
        line = strtok_r(NULL, "\n", &save);
    }
    free(text);
    if (*count == 0) {
        set_err(err, errlen, "nessun extent nel descriptor", NULL);
        return -1;
    }
    return 0;
}

int vmdk_probe(const char *path, uint64_t *total_sectors, char *err, size_t errlen)
{
    struct extent extents[MAX_EXTENTS];
    int count = 0;
    if (load_extents(path, extents, &count, err, errlen) != 0)
        return -1;
    uint64_t total = 0;
    for (int i = 0; i < count; i++)
        total += extents[i].sectors;
    if (total == 0) {
        set_err(err, errlen, "disco VMDK vuoto", NULL);
        return -1;
    }
    *total_sectors = total;
    return 0;
}

static int extent_read(const struct extent *ex, uint64_t offset, void *buf, size_t len,
                       char *err, size_t errlen)
{
    if (ex->type == EXT_ZERO) {
        memset(buf, 0, len);
        return 0;
    }
    int fd = open(ex->path, O_RDONLY);
    if (fd < 0) {
        set_err(err, errlen, "impossibile aprire l'extent", strerror(errno));
        return -1;
    }
    if (ex->type == EXT_FLAT) {
        ssize_t n = pread(fd, buf, len, (off_t)(ex->offset_sectors * 512ULL + offset));
        close(fd);
        if (n != (ssize_t)len)
            return -1;
        return 0;
    }
    unsigned char hdr[512];
    if (pread(fd, hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr) || rd32(hdr) != VMDK_MAGIC) {
        close(fd);
        set_err(err, errlen, "header sparse non valido", ex->path);
        return -1;
    }
    uint64_t grain_size = rd64(hdr + 20);
    uint32_t num_gtes = rd32(hdr + 44);
    uint64_t gd_offset = rd64(hdr + 56);
    if (!grain_size || !num_gtes) {
        close(fd);
        return -1;
    }
    uint64_t grain_bytes = grain_size * 512ULL;
    unsigned char *tmp = xmalloc((size_t)grain_bytes);
    size_t done = 0;
    int rc = 0;
    while (done < len) {
        uint64_t abs = offset + done;
        uint64_t grain_index = abs / grain_bytes;
        uint64_t within = abs % grain_bytes;
        size_t chunk = len - done;
        if (chunk > grain_bytes - within)
            chunk = (size_t)(grain_bytes - within);
        uint64_t gde = grain_index / num_gtes;
        uint32_t gte_index = (uint32_t)(grain_index % num_gtes);
        unsigned char raw[4];
        uint32_t gt_sector = 0, gte = 0;
        if (pread(fd, raw, 4, (off_t)(gd_offset * 512ULL + gde * 4)) != 4) {
            rc = -1;
            break;
        }
        gt_sector = rd32(raw);
        if (gt_sector) {
            if (pread(fd, raw, 4, (off_t)((uint64_t)gt_sector * 512ULL + gte_index * 4)) != 4) {
                rc = -1;
                break;
            }
            gte = rd32(raw);
        }
        if (gte == 0) {
            memset((unsigned char *)buf + done, 0, chunk);
        } else {
            if (pread(fd, tmp, (size_t)grain_bytes, (off_t)((uint64_t)gte * 512ULL)) !=
                (ssize_t)grain_bytes) {
                rc = -1;
                break;
            }
            memcpy((unsigned char *)buf + done, tmp + within, chunk);
        }
        done += chunk;
    }
    free(tmp);
    close(fd);
    if (rc != 0)
        set_err(err, errlen, "lettura del settore non riuscita", ex->path);
    return rc;
}

static int vmdk_read_loaded(const struct extent *extents, int count, uint64_t offset,
                            void *buf, size_t len, char *err, size_t errlen)
{
    uint64_t pos = offset;
    size_t done = 0;
    uint64_t base = 0;
    for (int i = 0; i < count && done < len; i++) {
        uint64_t end = base + extents[i].sectors * 512ULL;
        if (pos >= base && pos < end) {
            size_t chunk = (size_t)(end - pos);
            if (chunk > len - done)
                chunk = len - done;
            if (extent_read(&extents[i], pos - base, (unsigned char *)buf + done, chunk,
                            err, errlen) != 0)
                return -1;
            done += chunk;
            pos += chunk;
        }
        base = end;
    }
    if (done != len)
        return -1;
    return 0;
}

static uint64_t le64_at(const unsigned char *p)
{
    return rd64(p);
}

static uint32_t le32_at(const unsigned char *p)
{
    return rd32(p);
}

int vmdk_detect_bitlocker(const char *path, int *partition_index, uint64_t *offset,
                          char *err, size_t errlen)
{
    struct extent extents[MAX_EXTENTS];
    int count = 0;
    if (load_extents(path, extents, &count, err, errlen) != 0)
        return -1;
    unsigned char hdr[512];
    if (vmdk_read_loaded(extents, count, 512, hdr, sizeof(hdr), err, errlen) != 0)
        return -1;
    if (memcmp(hdr, "EFI PART", 8) != 0)
        return 0;
    uint64_t entries_lba = le64_at(hdr + 72);
    uint32_t num = le32_at(hdr + 80);
    uint32_t esize = le32_at(hdr + 84);
    if (num == 0 || num > 128 || esize < 128 || esize > 4096)
        return 0;
    size_t table_bytes = (size_t)num * esize;
    unsigned char *table = xmalloc(table_bytes);
    int rc = vmdk_read_loaded(extents, count, entries_lba * 512, table, table_bytes,
                              err, errlen);
    if (rc == 0) {
        rc = 0;
        for (uint32_t i = 0; i < num; i++) {
            const unsigned char *e = table + (size_t)i * esize;
            int nonzero = 0;
            for (int b = 0; b < 16; b++)
                if (e[b])
                    nonzero = 1;
            if (!nonzero)
                continue;
            uint64_t start = le64_at(e + 32);
            unsigned char vol[512];
            if (vmdk_read_loaded(extents, count, start * 512, vol, sizeof(vol),
                                 err, errlen) != 0) {
                rc = -1;
                break;
            }
            if (memcmp(vol + 3, "-FVE-FS-", 8) == 0) {
                if (partition_index)
                    *partition_index = (int)i + 1;
                if (offset)
                    *offset = start * 512;
                rc = 1;
                break;
            }
        }
    }
    free(table);
    return rc;
}

static int copy_range(int in_fd, int out_fd, uint64_t in_off, uint64_t out_off,
                      uint64_t bytes)
{
    unsigned char buf[1024 * 1024];
    while (bytes > 0) {
        size_t chunk = bytes > sizeof(buf) ? sizeof(buf) : (size_t)bytes;
        ssize_t n = pread(in_fd, buf, chunk, (off_t)in_off);
        if (n <= 0)
            return -1;
        ssize_t w = 0;
        while (w < n) {
            ssize_t k = pwrite(out_fd, buf + w, (size_t)(n - w), (off_t)(out_off + (uint64_t)w));
            if (k <= 0)
                return -1;
            w += k;
        }
        in_off += (uint64_t)n;
        out_off += (uint64_t)n;
        bytes -= (uint64_t)n;
    }
    return 0;
}

static int convert_sparse(const char *path, uint64_t base_sector, int out_fd,
                          uint64_t *converted_bytes, uint64_t total_bytes,
                          char *err, size_t errlen)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_err(err, errlen, "impossibile aprire l'extent", strerror(errno));
        return -1;
    }
    unsigned char hdr[512];
    if (pread(fd, hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr) || rd32(hdr) != VMDK_MAGIC) {
        close(fd);
        set_err(err, errlen, "header sparse non valido", path);
        return -1;
    }
    uint32_t flags = rd32(hdr + 8);
    uint64_t capacity = rd64(hdr + 12);
    uint64_t grain_size = rd64(hdr + 20);
    uint32_t num_gtes = rd32(hdr + 44);
    uint64_t gd_offset = rd64(hdr + 56);
    if (flags & 0x10000u) {
        close(fd);
        set_err(err, errlen, "VMDK compresso non supportato", path);
        return -1;
    }
    if (grain_size == 0 || num_gtes == 0 || gd_offset == 0) {
        close(fd);
        set_err(err, errlen, "parametri sparse non validi", path);
        return -1;
    }
    uint64_t grains = (capacity + grain_size - 1) / grain_size;
    uint64_t gd_entries = (grains + num_gtes - 1) / num_gtes;
    if (gd_entries > 1024 * 1024) {
        close(fd);
        set_err(err, errlen, "grain directory troppo grande", path);
        return -1;
    }
    uint32_t *gd = xmalloc((size_t)gd_entries * 4);
    if (pread(fd, gd, (size_t)gd_entries * 4, (off_t)(gd_offset * 512)) !=
        (ssize_t)(gd_entries * 4)) {
        free(gd);
        close(fd);
        set_err(err, errlen, "lettura grain directory non riuscita", path);
        return -1;
    }
    uint32_t *gt = xmalloc((size_t)num_gtes * 4);
    unsigned char *grain = xmalloc((size_t)grain_size * 512);
    int rc = 0;
    for (uint64_t g = 0; g < gd_entries && rc == 0; g++) {
        uint32_t gt_sector = gd[g];
        if (gt_sector == 0)
            continue;
        if (pread(fd, gt, (size_t)num_gtes * 4, (off_t)((uint64_t)gt_sector * 512)) !=
            (ssize_t)(num_gtes * 4)) {
            set_err(err, errlen, "lettura grain table non riuscita", path);
            rc = -1;
            break;
        }
        for (uint32_t i = 0; i < num_gtes; i++) {
            uint64_t grain_index = g * num_gtes + i;
            if (grain_index >= grains)
                break;
            uint32_t gte = gt[i];
            if (gte == 0)
                continue;
            uint64_t sectors = grain_size;
            if (grain_index * grain_size + sectors > capacity)
                sectors = capacity - grain_index * grain_size;
            uint64_t bytes = sectors * 512;
            if (pread(fd, grain, (size_t)bytes, (off_t)((uint64_t)gte * 512)) !=
                (ssize_t)bytes) {
                set_err(err, errlen, "lettura grain non riuscita", path);
                rc = -1;
                break;
            }
            uint64_t out_off = (base_sector + grain_index * grain_size) * 512;
            ssize_t w = 0;
            while (w < (ssize_t)bytes) {
                ssize_t k = pwrite(out_fd, grain + w, (size_t)((ssize_t)bytes - w),
                                   (off_t)(out_off + (uint64_t)w));
                if (k <= 0) {
                    set_err(err, errlen, "scrittura del disco non riuscita", strerror(errno));
                    rc = -1;
                    break;
                }
                w += k;
            }
            if (rc == 0) {
                *converted_bytes += bytes;
                if (total_bytes && *converted_bytes % (256ULL * 1024 * 1024) < bytes) {
                    unsigned pct = (unsigned)((*converted_bytes * 100) / total_bytes);
                    printf("\r  conversione: %3u%%", pct > 99 ? 99 : pct);
                    fflush(stdout);
                }
            }
        }
    }
    free(grain);
    free(gt);
    free(gd);
    close(fd);
    return rc;
}

int vmdk_to_raw(const char *path, const char *raw_path, uint64_t *total_sectors,
                char *err, size_t errlen)
{
    struct extent extents[MAX_EXTENTS];
    int count = 0;
    if (load_extents(path, extents, &count, err, errlen) != 0)
        return -1;

    uint64_t total = 0;
    for (int i = 0; i < count; i++)
        total += extents[i].sectors;
    if (total == 0) {
        set_err(err, errlen, "disco VMDK vuoto", NULL);
        return -1;
    }
    uint64_t total_bytes = total * 512ULL;

    int out_fd = open(raw_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        set_err(err, errlen, "impossibile creare il disco di destinazione", strerror(errno));
        return -1;
    }
    if (ftruncate(out_fd, (off_t)total_bytes) != 0) {
        set_err(err, errlen, "impossibile dimensionare il disco", strerror(errno));
        close(out_fd);
        return -1;
    }

    int rc = 0;
    uint64_t base = 0;
    uint64_t converted = 0;
    printf("  conversione VMDK -> raw (%llu MiB)\n", (unsigned long long)(total_bytes / (1024 * 1024)));
    for (int i = 0; i < count && rc == 0; i++) {
        struct extent *ex = &extents[i];
        switch (ex->type) {
        case EXT_SPARSE:
            rc = convert_sparse(ex->path, base, out_fd, &converted, total_bytes, err, errlen);
            break;
        case EXT_FLAT: {
            int in_fd = open(ex->path, O_RDONLY);
            if (in_fd < 0) {
                set_err(err, errlen, "impossibile aprire l'extent flat", strerror(errno));
                rc = -1;
                break;
            }
            uint64_t bytes = ex->sectors * 512ULL;
            rc = copy_range(in_fd, out_fd, ex->offset_sectors * 512ULL, base * 512ULL, bytes);
            if (rc != 0)
                set_err(err, errlen, "copia dell'extent flat non riuscita", ex->path);
            else
                converted += bytes;
            close(in_fd);
            break;
        }
        case EXT_ZERO:
            break;
        }
        base += ex->sectors;
    }
    fsync(out_fd);
    close(out_fd);
    printf("\r  conversione: 100%%\n");
    if (rc != 0)
        return -1;
    *total_sectors = total;
    return 0;
}
