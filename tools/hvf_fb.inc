/*
 * ramfb: QEMU-compatible fw_cfg framebuffer ("etc/ramfb"). The guest firmware
 * (RamFbDxe) publishes a GOP backed by guest RAM; we only record the
 * configuration and can dump the framebuffer to a BMP file for inspection.
 */
static struct {
    uint64_t addr;
    uint32_t fourcc, width, height, stride;
    int valid;
} g_fb;

static uint64_t be64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v; }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static void fb_config(const uint8_t *c)
{
    g_fb.addr = be64(c);
    g_fb.fourcc = be32(c + 8);
    g_fb.width = be32(c + 16);
    g_fb.height = be32(c + 20);
    g_fb.stride = be32(c + 24);
    g_fb.valid = g_fb.addr && g_fb.width && g_fb.height;
    static unsigned n;
    if (n++ < 8)
        fprintf(stderr, "ramfb: addr=0x%llx fourcc=0x%x %ux%u stride=%u\n",
                (unsigned long long)g_fb.addr, g_fb.fourcc, g_fb.width,
                g_fb.height, g_fb.stride);
}

static int fb_dump_bmp(const char *path)
{
    if (!g_fb.valid || g_fb.width > 8192 || g_fb.height > 8192)
        return -1;
    uint32_t w = g_fb.width, h = g_fb.height, stride = g_fb.stride ? g_fb.stride : w * 4;
    const uint8_t *src = gpa(g_fb.addr, (uint64_t)stride * h);
    if (!src)
        return -1;
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    uint32_t imgsz = w * h * 4, off = 54, fsz = off + imgsz;
    uint8_t hd[54] = { 'B', 'M' };
    memcpy(hd + 2, &fsz, 4);
    memcpy(hd + 10, &off, 4);
    uint32_t v = 40; memcpy(hd + 14, &v, 4);
    memcpy(hd + 18, &w, 4);
    int32_t nh = -(int32_t)h; memcpy(hd + 22, &nh, 4);   /* top-down */
    hd[26] = 1; hd[28] = 32;
    memcpy(hd + 34, &imgsz, 4);
    fwrite(hd, 1, 54, f);
    for (uint32_t y = 0; y < h; y++)
        fwrite(src + (uint64_t)y * stride, 1, w * 4, f);
    fclose(f);
    return 0;
}

static void *fb_thread(void *arg)
{
    (void)arg;
    const char *path = getenv("GMBOX_FB_DUMP");
    const char *iv = getenv("GMBOX_FB_INTERVAL");
    unsigned secs = iv ? (unsigned)atoi(iv) : 10;
    for (;;) {
        sleep(secs ? secs : 10);
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        if (fb_dump_bmp(tmp) == 0)
            rename(tmp, path);
    }
    return NULL;
}

/*
 * GMBOX_AUTOENTER=1: when the framebuffer shows the blue Windows Boot Manager
 * "Recovery" screen, type Enter on the emulated UART ("try again"). This
 * replaces a missing keyboard device while the VM disk still carries a failed
 * boot status.
 */
extern volatile int g_uart_inject;
static void *autoenter_thread(void *arg)
{
    (void)arg;
    int last = 0;
    for (;;) {
        sleep(2);
        if (!g_fb.valid)
            continue;
        uint32_t w = g_fb.width, h = g_fb.height, stride = g_fb.stride ? g_fb.stride : w * 4;
        const uint8_t *src = gpa(g_fb.addr, (uint64_t)stride * h);
        if (!src)
            continue;
        int blue = 0, n = 0;
        for (uint32_t y = 8; y < h; y += h / 12)
            for (uint32_t x = 8; x < w; x += w / 16) {
                const uint8_t *p = src + (uint64_t)y * stride + (uint64_t)x * 4;
                n++;
                if (p[0] > 0x90 && p[2] < 0x50) /* B high, R low */
                    blue++;
            }
        if (blue * 10 > n * 7 && !last) {
            fprintf(stderr, "autoenter: Recovery screen detected, sending Enter\n");
            g_uart_inject = '\r';
            last = 1;
        } else if (blue * 10 <= n * 7) {
            last = 0;
        }
    }
    return NULL;
}
