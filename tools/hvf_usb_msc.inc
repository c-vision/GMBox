/*
 * USB mass-storage passthrough: file-backed BOT/SCSI device on EHCI root
 * port 2. Enabled by GMBOX_USB_DISK=/path. GMBOX_USB_RO=1 keeps it read-only.
 * Host devices are never opened implicitly; this is a file-backed passthrough
 * boundary.
 */

#define MSC_BLOCK 512u
struct msc_state {
    int fd;
    int ro;
    uint64_t blocks;
    uint32_t tag;
    uint32_t data_len, data_sent;
    uint64_t lba;
    int phase;                 /* 0 idle, 1 data, 2 csw */
    int dir;                   /* 1 IN, 0 OUT */
    uint8_t cbw[31];
    uint8_t csw[13];
    uint8_t sense[18];
    uint8_t inq[36];
};
static struct msc_state msc_dev[USB_NPORTS];

static int msc_setup(struct usb_dev *d, const uint8_t *s);
static int msc_bulk(struct usb_dev *d, uint32_t ep, uint32_t pid,
                    uint32_t *qh, uint32_t total, uint32_t offset);
static int msc_config_desc(struct usb_dev *d, uint8_t *o);
static void msc_reset(int idx);
static void msc_init(void);
static int msc_attach(const char *path);
static void msc_detach(void);

static uint16_t msc_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t msc_be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void msc_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static int msc_config_desc(struct usb_dev *d, uint8_t *o)
{
    (void)d;
    static const uint8_t c[] = {
        9, 2, 32, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 2, 8, 6, 0x50, 0,
        7, 5, 0x01, 2, 0x00, 0x02, 0,
        7, 5, 0x82, 2, 0x00, 0x02, 0
    };
    memcpy(o, c, sizeof c);
    return (int)sizeof c;
}

static int msc_dev_desc(struct usb_dev *d, uint8_t *o)
{
    (void)d;
    static const uint8_t dd[] = { 18, 1, 0x00, 0x02, 0, 0, 0, 64,
                                  0x27, 0x06, 0, 0, 0x00, 0x01, 1, 2, 3, 1 };
    memcpy(o, dd, sizeof dd);
    return (int)sizeof dd;
}

static int msc_str(struct usb_dev *d, int idx, uint8_t *out)
{
    (void)d;
    const char *s = idx == 1 ? "GMBox" : idx == 2 ? "GMBox USB Storage" : idx == 3 ? "GMBMSC0001" : NULL;
    if (!s) return -1;
    int n = (int)strlen(s);
    out[0] = (uint8_t)(2 + 2 * n); out[1] = 3;
    for (int i = 0; i < n; i++) { out[2 + 2 * i] = (uint8_t)s[i]; out[3 + 2 * i] = 0; }
    return 2 + 2 * n;
}

static void msc_sense(int idx, uint8_t key, uint8_t asc, uint8_t ascq)
{
    struct msc_state *m = &msc_dev[idx];
    memset(m->sense, 0, sizeof m->sense);
    m->sense[0] = 0x70;
    m->sense[2] = key;
    m->sense[7] = 10;
    m->sense[12] = asc;
    m->sense[13] = ascq;
}

static void msc_reset(int idx)
{
    struct msc_state *m = &msc_dev[idx];
    m->phase = 0; m->dir = 0; m->data_len = 0; m->data_sent = 0; m->lba = 0; m->tag = 0;
    memset(m->sense, 0, sizeof m->sense);
    msc_sense(idx, 0, 0, 0);
}

static void msc_prepare_csw(int idx, uint8_t status)
{
    struct msc_state *m = &msc_dev[idx];
    memcpy(m->csw, "USBS", 4);
    msc_put32(m->csw + 4, m->tag);
    msc_put32(m->csw + 8, m->data_len - m->data_sent);
    m->csw[12] = status;
    m->phase = 2;
}

static int msc_cdb(int idx)
{
    struct msc_state *m = &msc_dev[idx];
    const uint8_t *c = m->cbw + 15;
    uint8_t op = c[0];
    uint32_t alloc = msc_be16(m->cbw + 7);
    uint64_t lba = 0; uint32_t n = 0;
    if (op == 0x00 || op == 0x1b || op == 0x35) {
        m->data_len = 0; m->dir = 1;
        if (op == 0x35 && m->fd >= 0) fsync(m->fd);
        msc_prepare_csw(idx, 0);
        return 0;
    }
    if (op == 0x03) { /* REQUEST SENSE */
        m->data_len = alloc < sizeof m->sense ? alloc : sizeof m->sense;
        m->dir = 1; m->data_sent = 0;
        return 0;
    }
    if (op == 0x12) { /* INQUIRY */
        m->data_len = alloc < sizeof m->inq ? alloc : sizeof m->inq;
        m->dir = 1; m->data_sent = 0;
        return 0;
    }
    if (op == 0x1a) { /* MODE SENSE(6): minimal header */
        m->data_len = alloc < 4 ? alloc : 4;
        m->dir = 1; m->data_sent = 0;
        return 0;
    }
    if (op == 0x25) { /* READ CAPACITY(10) */
        m->data_len = alloc < 8 ? alloc : 8;
        m->dir = 1; m->data_sent = 0;
        return 0;
    }
    if (op == 0x28 || op == 0x2a) { /* READ/WRITE(10) */
        lba = msc_be32(c + 2); n = msc_be16(c + 7);
        m->lba = lba; m->data_len = n * MSC_BLOCK;
        m->dir = (op == 0x28); m->data_sent = 0;
        if (op == 0x2a && m->ro) { msc_sense(idx, 0x27, 0x27, 0); msc_prepare_csw(idx, 1); return 0; }
        return 0;
    }
    if (op == 0x88 || op == 0x8a) { /* READ/WRITE(16) */
        lba = ((uint64_t)msc_be32(c + 2) << 32) | msc_be32(c + 6);
        n = msc_be32(c + 10);
        m->lba = lba; m->data_len = n * MSC_BLOCK;
        m->dir = (op == 0x88); m->data_sent = 0;
        if (op == 0x8a && m->ro) { msc_sense(idx, 0x27, 0x27, 0); msc_prepare_csw(idx, 1); return 0; }
        return 0;
    }
    msc_sense(idx, 0x05, 0x20, 0); /* INVALID COMMAND */
    msc_prepare_csw(idx, 1);
    return 0;
}

static int msc_setup(struct usb_dev *d, const uint8_t *s)
{
    int idx = (int)(d - usb_devs);
    uint8_t bm = s[0], br = s[1];
    uint16_t wv = (uint16_t)(s[2] | (s[3] << 8)), wl = (uint16_t)(s[6] | (s[7] << 8));
    d->ctl_len = 0;
    d->ctl_dir_in = (bm & 0x80) != 0;
    d->ctl_stage = wl ? 1 : 2;
    int type = (bm >> 5) & 3;
    if (type == 0) {
        switch (br) {
        case 0x06: {
            uint8_t t = (uint8_t)(wv >> 8), i = (uint8_t)wv;
            if (t == 1) { d->ctl_len = msc_dev_desc(d, d->ctl_buf); break; }
            if (t == 2) { d->ctl_len = msc_config_desc(d, d->ctl_buf); break; }
            if (t == 3) { d->ctl_len = msc_str(d, i, d->ctl_buf); if (d->ctl_len < 0) return -1; break; }
            return -1;
        }
        case 0x05: d->pending_addr = (uint8_t)wv; d->has_pending_addr = 1; break;
        case 0x09: d->config = (uint8_t)wv; break;
        case 0x08: d->ctl_buf[0] = d->config; d->ctl_len = 1; break;
        case 0x00: d->ctl_buf[0] = d->ctl_buf[1] = 0; d->ctl_len = 2; break;
        case 0x0a: d->ctl_buf[0] = 0; d->ctl_len = 1; break;
        case 0x01: case 0x03: case 0x0b: break;
        default: return -1;
        }
        if (d->ctl_len > wl) d->ctl_len = wl;
        return 0;
    }
    if (type == 1 && bm == 0xa1 && br == 0xfe) { d->ctl_buf[0] = 0; d->ctl_len = 1; if (d->ctl_len > wl) d->ctl_len = wl; return 0; }
    if (type == 1 && bm == 0x21 && br == 0xff) { msc_reset(idx); d->ctl_len = 0; d->ctl_stage = 2; return 0; }
    return -1;
}

static int msc_bulk(struct usb_dev *d, uint32_t ep, uint32_t pid,
                    uint32_t *qh, uint32_t total, uint32_t offset)
{
    int idx = (int)(d - usb_devs);
    struct msc_state *m = &msc_dev[idx];
    uint8_t tmp[512];
    if (m->fd < 0) return -1;
    if (ep == 1 && pid == 0) { /* OUT: CBW or WRITE data */
        if (m->phase == 0) {
            if (total < 31) return -1;
            if (usb_buf_copy(qh, offset, m->cbw, 31, 0) < 0) return -2;
            m->tag = msc_be32(m->cbw + 4);
            m->phase = 1; m->data_sent = 0;
            msc_cdb(idx);
            return 31;
        }
        if (m->phase == 1 && m->dir == 0) {
            uint32_t n = total < m->data_len - m->data_sent ? total : m->data_len - m->data_sent;
            if (n > sizeof tmp) n = sizeof tmp;
            if (usb_buf_copy(qh, offset, tmp, n, 0) < 0) return -2;
            if (!m->ro) {
                ssize_t w = pwrite(m->fd, tmp, n, (off_t)((m->lba * MSC_BLOCK) + m->data_sent));
                if (w != (ssize_t)n) { msc_sense(idx, 0x03, 0x0c, 0); msc_prepare_csw(idx, 1); return (int)n; }
            }
            m->data_sent += n;
            if (m->data_sent >= m->data_len) msc_prepare_csw(idx, 0);
            return (int)n;
        }
        return -1;
    }
    if (ep == 2 && pid == 1) { /* IN: READ data or CSW */
        if (m->phase == 1 && m->dir == 1) {
            uint32_t rem = m->data_len - m->data_sent;
            uint32_t n = total < rem ? total : rem;
            if (n > sizeof tmp) n = sizeof tmp;
            if (m->data_sent == 0) {
                if (m->cbw[15] == 0x12) {
                    memcpy(tmp, m->inq, sizeof m->inq);
                    memset(tmp + sizeof m->inq, 0, n > sizeof m->inq ? n - sizeof m->inq : 0);
                } else if (m->cbw[15] == 0x25) {
                    msc_put32(tmp, (uint32_t)(m->blocks ? m->blocks - 1 : 0));
                    msc_put32(tmp + 4, MSC_BLOCK);
                } else if (m->cbw[15] == 0x03) {
                    memcpy(tmp, m->sense, sizeof m->sense);
                } else if (m->cbw[15] == 0x1a) {
                    memset(tmp, 0, 4);
                    tmp[0] = 3; tmp[3] = 0;
                }
            }
            if (m->cbw[15] == 0x28 || m->cbw[15] == 0x88) {
                ssize_t r = pread(m->fd, tmp, n, (off_t)((m->lba * MSC_BLOCK) + m->data_sent));
                if (r != (ssize_t)n) { msc_sense(idx, 0x03, 0x11, 0); msc_prepare_csw(idx, 1); return (int)n; }
            }
            if (usb_buf_copy(qh, offset, tmp, n, 1) < 0) return -2;
            m->data_sent += n;
            if (m->data_sent >= m->data_len) msc_prepare_csw(idx, 0);
            return (int)n;
        }
        if (m->phase == 2) {
            uint32_t n = total < sizeof m->csw ? total : sizeof m->csw;
            if (usb_buf_copy(qh, offset, m->csw, n, 1) < 0) return -2;
            if (n >= sizeof m->csw) { m->phase = 0; msc_reset(idx); }
            return (int)n;
        }
        return -1;
    }
    return -1;
}

static int msc_attach(const char *path)
{
    if (!path || !*path) return -1;
    int fd = open(path, O_RDWR);
    int ro = 0;
    if (fd < 0) { fd = open(path, O_RDONLY); ro = 1; }
    if (fd < 0) return -1;
    if (getenv("GMBOX_USB_RO")) ro = 1;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < MSC_BLOCK) { close(fd); return -1; }
    struct msc_state *m = &msc_dev[2];
    if (m->fd >= 0) close(m->fd);
    m->fd = fd; m->ro = ro; m->blocks = (uint64_t)st.st_size / MSC_BLOCK;
    memset(m->inq, 0, sizeof m->inq);
    m->inq[0] = 0x00; m->inq[1] = 0x80; m->inq[2] = 0x04; m->inq[3] = 0x02;
    memcpy(m->inq + 8, "GMBox   ", 8);
    memcpy(m->inq + 16, "USB Storage     ", 16);
    memcpy(m->inq + 32, "0001", 4);
    msc_reset(2);
    usb_devs[2].is_msc = 1;
    usb_portsc[2] |= 1 | 0x1000;
    usb_sts |= 4;
    usb_irq_update();
    fprintf(stderr, "usb: storage attached %s (%llu blocks%s) port2=0x%x ports=%d\n", path,
            (unsigned long long)m->blocks, ro ? ", read-only" : "", usb_portsc[2], USB_NPORTS);
    return 0;
}

static void msc_detach(void)
{
    struct msc_state *m = &msc_dev[2];
    if (m->fd >= 0) close(m->fd);
    m->fd = -1; m->blocks = 0;
    usb_devs[2].is_msc = 0;
    usb_portsc[2] &= ~1u;
    usb_sts |= 4;
    usb_irq_update();
    fprintf(stderr, "usb: storage detached\n");
}

static void msc_init(void)
{
    for (int i = 0; i < USB_NPORTS; i++) {
        msc_dev[i].fd = -1;
        msc_dev[i].blocks = 0;
        msc_dev[i].ro = 0;
    }
    usb_portsc[2] = 0x1000 | 0x2;
    const char *path = getenv("GMBOX_USB_DISK");
    if (path && *path) msc_attach(path);
}
