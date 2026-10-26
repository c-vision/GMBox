/*
 * vars_inject: append NV variables to an EDK2 (authenticated-format) variable
 * store image (QEMU_VARS.fd layout, authenticated header).
 *
 *   vars_inject <vars.fd> <name> <guid> <attr-hex> <data-hex>
 *
 * C99, no dependencies.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_guid(const char *s, uint8_t *g)
{
    unsigned a, b, c, d[8];
    if (sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b, &c,
               &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]) != 11)
        return -1;
    g[0] = a; g[1] = a >> 8; g[2] = a >> 16; g[3] = a >> 24;
    g[4] = b; g[5] = b >> 8; g[6] = c; g[7] = c >> 8;
    for (int i = 0; i < 8; i++) g[8 + i] = (uint8_t)d[i];
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: %s vars.fd name guid attr-hex data-hex\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "r+b");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    uint8_t *m = malloc((size_t)sz);
    fseek(f, 0, SEEK_SET);
    if (!m || fread(m, 1, (size_t)sz, f) != (size_t)sz) return 1;

    uint32_t hdr = *(uint16_t *)(m + 0x30);              /* FV header length */
    uint32_t vsz = *(uint32_t *)(m + hdr + 16);
    uint32_t pos = hdr + 28, end = hdr + vsz;
    while (pos + 60 <= end && *(uint16_t *)(m + pos) == 0x55aa) {
        uint32_t ns = *(uint32_t *)(m + pos + 36), ds = *(uint32_t *)(m + pos + 40);
        pos += 60 + ns + ds;
        pos = (pos + 3) & ~3u;
    }

    size_t nl = strlen(argv[2]), dl = strlen(argv[5]) / 2;
    uint32_t nsz = (uint32_t)(nl + 1) * 2;
    if (pos + 60 + nsz + dl + 4 > end) { fprintf(stderr, "store full\n"); return 1; }
    uint8_t *v = m + pos;
    memset(v, 0, 60 + nsz + dl);
    v[0] = 0xaa; v[1] = 0x55; v[2] = 0x3f;
    uint32_t attr = (uint32_t)strtoul(argv[4], NULL, 16);
    memcpy(v + 4, &attr, 4);
    memcpy(v + 36, &nsz, 4);
    uint32_t d32 = (uint32_t)dl;
    memcpy(v + 40, &d32, 4);
    if (parse_guid(argv[3], v + 44) < 0) { fprintf(stderr, "bad guid\n"); return 1; }
    for (size_t i = 0; i < nl; i++) v[60 + 2 * i] = (uint8_t)argv[2][i];
    for (size_t i = 0; i < dl; i++)
        v[60 + nsz + i] = (uint8_t)(hexval(argv[5][2 * i]) * 16 + hexval(argv[5][2 * i + 1]));
    fseek(f, pos, SEEK_SET);
    fwrite(v, 1, 60 + nsz + dl, f);
    fclose(f);
    printf("appended %s at 0x%x (%zu data bytes)\n", argv[2], pos, dl);
    return 0;
}
