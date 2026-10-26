/*
 * Emulated USB EHCI host controller (PCI bus 0 dev 2) with a HID boot keyboard
 * on root port 0 and a HID mouse on root port 1. Windows binds its inbox
 * usbehci/usbport/hidusb/kbdhid/mouhid drivers to it, which gives the VM an
 * input path without any guest driver.
 *
 * Everything runs on the vCPU thread: MMIO accesses plus a 1 kHz tick (a helper
 * thread forces a vCPU exit; the exit handler calls usb_tick()). Injecting the
 * SPI from the vCPU thread avoids the lost-interrupt race seen with helpers.
 *
 * Input comes from a FIFO (GMBOX_INPUT=path), one command per line:
 *   type <text>        key <name>        wait <ms>
 *   move <dx> <dy>     click l|r|m       down l|r|m   up l|r|m   wheel <n>
 * names: enter esc tab space bksp del up down left right home end pgup pgdn f1..f12
 *        win, and combos like ctrl+alt+del, shift+tab.
 *
 * Limited to what Windows needs: control transfers on EP0, interrupt-IN polling
 * from the periodic schedule, no iTD/siTD, no split transactions.
 */

#define USB_IPA        0x20004000UL     /* default BAR0, reassigned by the OS */
#define USB_INTID      37u              /* SPI 5: slot 2 INTA in the DTB / _PRT */
#define USB_NPORTS     2

static int usb_irq_level;
static uint32_t usb_cmd, usb_sts, usb_intr, usb_frindex_base, usb_ctrlds, usb_perbase,
                usb_asyncaddr, usb_cfgflag;
static uint32_t usb_portsc[USB_NPORTS];
static int usb_port_reset[USB_NPORTS];
static uint64_t usb_t0;
volatile int g_usb_tick;

/* ------------------------------------------------------------------ */
/* HID devices                                                         */

static const uint8_t kbd_report_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01,
    0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x81, 0x00, 0xC0
};
static const uint8_t mouse_report_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09,
    0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06, 0xC0, 0xC0
};

struct usb_dev {
    int is_kbd;
    int is_composite;
    int is_msc;
    uint8_t addr, pending_addr;
    int has_pending_addr;
    uint8_t config;
    /* control transfer state */
    uint8_t setup[8];
    uint8_t ctl_buf[256];
    int ctl_len, ctl_dir_in, ctl_stage;   /* stage: 0 idle, 1 data, 2 status */
    /* interrupt IN queue (8-byte reports; mouse uses 4) */
    uint8_t q[256][8];
    int qh, qt;
    uint8_t mq[256][8];
    int mqh, mqt;
};
static struct usb_dev usb_devs[USB_NPORTS];

static int msc_setup(struct usb_dev *d, const uint8_t *s);
static int msc_bulk(struct usb_dev *d, uint32_t ep, uint32_t pid,
                    uint32_t *qh, uint32_t total, uint32_t offset);
static int msc_config_desc(struct usb_dev *d, uint8_t *o);
static void msc_init(void);
static int msc_attach(const char *path);
static void msc_detach(void);

static void q_push(struct usb_dev *d, const uint8_t *r)
{
    int n = (d->qt + 1) & 255;
    if (n == d->qh)
        return;
    memcpy(d->q[d->qt], r, 8);
    d->qt = n;
}

static void mq_push(struct usb_dev *d, const uint8_t *r)
{
    int n = (d->mqt + 1) & 255;
    if (n == d->mqh)
        return;
    memcpy(d->mq[d->mqt], r, 8);
    d->mqt = n;
}

static int usb_str_desc(struct usb_dev *d, int idx, uint8_t *out)
{
    const char *s = NULL;
    if (idx == 0) { out[0] = 4; out[1] = 3; out[2] = 9; out[3] = 4; return 4; }
    if (idx == 1) s = "GMBox";
    else if (idx == 2) s = d->is_kbd ? "GMBox USB Keyboard" : "GMBox USB Mouse";
    else if (idx == 3) s = d->is_kbd ? "GMBKBD0001" : "GMBMOU0001";
    if (!s)
        return -1;
    int n = (int)strlen(s);
    out[0] = (uint8_t)(2 + 2 * n);
    out[1] = 3;
    for (int i = 0; i < n; i++) { out[2 + 2 * i] = (uint8_t)s[i]; out[3 + 2 * i] = 0; }
    return 2 + 2 * n;
}

static int usb_config_desc(struct usb_dev *d, uint8_t *o)
{
    if (d->is_msc)
        return msc_config_desc(d, o);
    if (d->is_composite) {
        static const uint8_t c[] = {
            9, 2, 59, 0, 1, 1, 0, 0x80, 50,
            9, 4, 0, 0, 1, 3, 1, 1, 0,
            9, 0x21, 0x11, 0x01, 0, 1, 0x22, 0x3f, 0,
            7, 5, 0x81, 3, 8, 0, 8,
            9, 4, 1, 0, 1, 3, 1, 2, 0,
            9, 0x21, 0x11, 0x01, 0, 1, 0x22, 0x32, 0,
            7, 5, 0x82, 3, 4, 0, 0x0a
        };
        memcpy(o, c, sizeof c);
        return (int)sizeof c;
    }
    int rdl = d->is_kbd ? (int)sizeof kbd_report_desc : (int)sizeof mouse_report_desc;
    int mp = d->is_kbd ? 8 : 4;
    uint8_t c[] = {
        9, 2, 34, 0, 1, 1, 0, 0xA0, 50,
        9, 4, 0, 0, 1, 3, 1, (uint8_t)(d->is_kbd ? 1 : 2), 0,
        9, 0x21, 0x11, 0x01, 0, 1, 0x22, (uint8_t)rdl, 0,
        7, 5, 0x81, 3, (uint8_t)mp, 0, 7
    };
    memcpy(o, c, sizeof c);
    return (int)sizeof c;
}

/* Handle a SETUP packet. Returns 0 ok, -1 stall. */
static int usb_setup(struct usb_dev *d, const uint8_t *s)
{
    memcpy(d->setup, s, 8);
    if (d->is_msc)
        return msc_setup(d, s);
    if (getenv("GMBOX_USBLOG")) fprintf(stderr, "USB dev%d addr=%u setup %02x %02x %02x%02x %02x%02x len=%u\n", (int)(d - usb_devs), d->addr, s[0], s[1], s[3], s[2], s[5], s[4], s[6] | (s[7] << 8));
    uint8_t bm = s[0], br = s[1];
    uint16_t wv = (uint16_t)(s[2] | (s[3] << 8)), wl = (uint16_t)(s[6] | (s[7] << 8));
    d->ctl_len = 0;
    d->ctl_dir_in = (bm & 0x80) != 0;
    d->ctl_stage = wl ? 1 : 2;
    int type = (bm >> 5) & 3;
    if (type == 0) {
        switch (br) {
        case 0x06: { /* GET_DESCRIPTOR */
            int t = wv >> 8, i = wv & 0xff;
            if (t == 1) {
                uint8_t dd[] = { 18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x27, 0x06,
                                 (uint8_t)(d->is_kbd ? 1 : 2), 0, 0x00, 0x01, 1, 2, 3, 1 };
                memcpy(d->ctl_buf, dd, 18);
                d->ctl_len = 18;
            } else if (t == 2) {
                d->ctl_len = usb_config_desc(d, d->ctl_buf);
            } else if (t == 3) {
                d->ctl_len = usb_str_desc(d, i, d->ctl_buf);
                if (d->ctl_len < 0) return -1;
            } else if (t == 6) { /* device qualifier: not supported by a HS-only model */
                return -1;
            } else if (bm == 0x81 && t == 0x22) {
                uint16_t wi = (uint16_t)(s[4] | (s[5] << 8));
                if (d->is_composite && wi == 1) { memcpy(d->ctl_buf, mouse_report_desc, sizeof mouse_report_desc); d->ctl_len = sizeof mouse_report_desc; }
                else if (d->is_composite) { memcpy(d->ctl_buf, kbd_report_desc, sizeof kbd_report_desc); d->ctl_len = sizeof kbd_report_desc; }
                else if (d->is_kbd) { memcpy(d->ctl_buf, kbd_report_desc, sizeof kbd_report_desc); d->ctl_len = sizeof kbd_report_desc; }
                else { memcpy(d->ctl_buf, mouse_report_desc, sizeof mouse_report_desc); d->ctl_len = sizeof mouse_report_desc; }
            } else if (bm == 0x81 && t == 0x21) {
                uint16_t wi = (uint16_t)(s[4] | (s[5] << 8));
                uint8_t c[64]; usb_config_desc(d, c);
                int off = (d->is_composite && wi == 1) ? 43 : 18;
                memcpy(d->ctl_buf, c + off, 9); d->ctl_len = 9;
            } else
                return -1;
            break;
        }
        case 0x05: d->pending_addr = (uint8_t)wv; d->has_pending_addr = 1; break;
        case 0x09: d->config = (uint8_t)wv; break;
        case 0x08: d->ctl_buf[0] = d->config; d->ctl_len = 1; break;
        case 0x00: d->ctl_buf[0] = d->ctl_buf[1] = 0; d->ctl_len = 2; break;
        case 0x01: case 0x03: case 0x0b: break;       /* CLEAR/SET_FEATURE, SET_INTERFACE */
        case 0x0a: d->ctl_buf[0] = 0; d->ctl_len = 1; break;
        default: return -1;
        }
    } else if (type == 1) { /* class: HID */
        switch (br) {
        case 0x0a: case 0x0b: break;                  /* SET_IDLE, SET_PROTOCOL */
        case 0x09: break;                             /* SET_REPORT: data stage ignored */
        case 0x01: { uint16_t wi = (uint16_t)(s[4] | (s[5] << 8)); memset(d->ctl_buf, 0, 8); d->ctl_len = (d->is_composite && wi == 1) ? 4 : (d->is_kbd ? 8 : 4); break; }
        case 0x03: d->ctl_buf[0] = 1; d->ctl_len = 1; break;
        case 0x02: d->ctl_buf[0] = 0; d->ctl_len = 1; break;
        default: return -1;
        }
    } else
        return -1;
    if (d->ctl_len > wl) d->ctl_len = wl;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Input script (FIFO)                                                 */

static int g_usb_fifo = -1;
static char usb_inbuf[512];
static int usb_inlen;
static uint64_t usb_wait_until;
static uint8_t mouse_btn;

static int hid_for_char(char c, int *shift)
{
    *shift = 0;
    if (c >= 'a' && c <= 'z') return 4 + (c - 'a');
    if (c >= 'A' && c <= 'Z') { *shift = 1; return 4 + (c - 'A'); }
    if (c >= '1' && c <= '9') return 0x1e + (c - '1');
    if (c == '0') return 0x27;
    static const struct { char c; int k; int s; } t[] = {
        {' ',0x2c,0},{'\n',0x28,0},{'\t',0x2b,0},{'-',0x2d,0},{'_',0x2d,1},{'=',0x2e,0},{'+',0x2e,1},
        {'[',0x2f,0},{'{',0x2f,1},{']',0x30,0},{'}',0x30,1},{'\\',0x31,0},{'|',0x31,1},{';',0x33,0},
        {':',0x33,1},{'\'',0x34,0},{'"',0x34,1},{'`',0x35,0},{'~',0x35,1},{',',0x36,0},{'<',0x36,1},
        {'.',0x37,0},{'>',0x37,1},{'/',0x38,0},{'?',0x38,1},{'!',0x1e,1},{'@',0x1f,1},{'#',0x20,1},
        {'$',0x21,1},{'%',0x22,1},{'^',0x23,1},{'&',0x24,1},{'*',0x25,1},{'(',0x26,1},{')',0x27,1},
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (t[i].c == c) { *shift = t[i].s; return t[i].k; }
    return 0;
}

static int hid_for_name(const char *n)
{
    static const struct { const char *n; int k; } t[] = {
        {"enter",0x28},{"esc",0x29},{"bksp",0x2a},{"tab",0x2b},{"space",0x2c},{"del",0x4c},
        {"right",0x4f},{"left",0x50},{"down",0x51},{"up",0x52},{"home",0x4a},{"end",0x4d},
        {"pgup",0x4b},{"pgdn",0x4e},{"win",0xe3},
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(t[i].n, n)) return t[i].k;
    if (n[0] == 'f' && n[1] >= '1' && n[1] <= '9') {
        int f = atoi(n + 1);
        if (f >= 1 && f <= 12) return 0x3a + f - 1;
    }
    if (n[1] == 0) { int s; return hid_for_char(n[0], &s); }
    return 0;
}

static void kbd_press(uint8_t mod, int key)
{
    struct usb_dev *d = &usb_devs[0];
    uint8_t r[8] = { mod, 0, (uint8_t)key, 0, 0, 0, 0, 0 };
    uint8_t z[8] = { 0 };
    if (key >= 0xe0 && key <= 0xe7) { r[0] |= (uint8_t)(1 << (key - 0xe0)); r[2] = 0; }
    q_push(d, r);
    q_push(d, z);
}

static void usb_cmd_line(char *l)
{
    char *arg = strchr(l, ' ');
    if (arg) *arg++ = 0;
    if (!strcmp(l, "type") && arg) {
        for (char *p = arg; *p; p++) {
            int sh; int k = hid_for_char(*p, &sh);
            if (k) kbd_press(sh ? 0x02 : 0, k);
        }
    } else if (!strcmp(l, "key") && arg) {
        uint8_t mod = 0; char *p = arg, *tok;
        char *plus;
        while ((plus = strchr(p, '+')) != NULL) {
            *plus = 0; tok = p; p = plus + 1;
            if (!strcmp(tok, "ctrl")) mod |= 0x01;
            else if (!strcmp(tok, "shift")) mod |= 0x02;
            else if (!strcmp(tok, "alt")) mod |= 0x04;
            else if (!strcmp(tok, "win")) mod |= 0x08;
        }
        int k = hid_for_name(p);
        if (k) kbd_press(mod, k);
    } else if (!strcmp(l, "usb") && arg) {
        if (!strncmp(arg, "attach ", 7)) msc_attach(arg + 7);
        else if (!strcmp(arg, "detach")) msc_detach();
    } else if (!strcmp(l, "wait") && arg) {
        usb_wait_until = mach_absolute_time() + (uint64_t)atoi(arg) * 24000ULL;
    } else if (!strcmp(l, "move") && arg) {
        int dx = 0, dy = 0; sscanf(arg, "%d %d", &dx, &dy);
        while (dx || dy) {
            int sx = dx > 127 ? 127 : dx < -127 ? -127 : dx, sy = dy > 127 ? 127 : dy < -127 ? -127 : dy;
            uint8_t r[8] = { mouse_btn, (uint8_t)(int8_t)sx, (uint8_t)(int8_t)sy, 0 };
            mq_push(&usb_devs[0], r);
            dx -= sx; dy -= sy;
        }
    } else if (!strcmp(l, "wheel") && arg) {
        uint8_t r[8] = { mouse_btn, 0, 0, (uint8_t)(int8_t)atoi(arg) };
        mq_push(&usb_devs[0], r);
    } else if ((!strcmp(l, "click") || !strcmp(l, "down") || !strcmp(l, "up")) && arg) {
        uint8_t b = arg[0] == 'r' ? 2 : arg[0] == 'm' ? 4 : 1;
        uint8_t r[8] = { 0 };
        if (l[0] != 'u') { mouse_btn |= b; r[0] = mouse_btn; mq_push(&usb_devs[0], r); }
        if (l[0] != 'd') { mouse_btn &= (uint8_t)~b; r[0] = mouse_btn; mq_push(&usb_devs[0], r); }
    }
}

static void usb_input_poll(void)
{
    if (g_usb_fifo < 0)
        return;
    if (usb_wait_until && mach_absolute_time() < usb_wait_until)
        return;
    usb_wait_until = 0;
    char c;
    while (read(g_usb_fifo, &c, 1) == 1) {
        if (c == '\n') {
            usb_inbuf[usb_inlen] = 0;
            usb_inlen = 0;
            usb_cmd_line(usb_inbuf);
            if (usb_wait_until)
                return;
        } else if (usb_inlen < (int)sizeof usb_inbuf - 1) {
            usb_inbuf[usb_inlen++] = c;
        }
    }
}

/* ------------------------------------------------------------------ */
/* EHCI                                                                */

static void usb_irq_update(void)
{
    int cmd_dis = (pci_cfg[0x10005] >> 2) & 1;
    int line = (usb_sts & usb_intr & 0x3f) != 0;
    if (line) pci_cfg[0x10006] |= 0x08; else pci_cfg[0x10006] &= (uint8_t)~0x08;
    line = line && !cmd_dis;
    if (line != usb_irq_level) {
        usb_irq_level = line;
        hv_gic_set_spi(USB_INTID, line != 0);
    }
}

static void usb_reset_regs(void)
{
    usb_cmd = 0x00080000;       /* ITC = 8 microframes */
    usb_sts = 0;
    usb_intr = 0;
    usb_perbase = usb_asyncaddr = usb_ctrlds = 0;
    usb_cfgflag = 0;
    for (int i = 0; i < USB_NPORTS; i++) {
        usb_portsc[i] = 0x1000 | 0x2;        /* PP, CSC only by default */
        usb_port_reset[i] = 0;
        usb_devs[i].addr = 0;
        usb_devs[i].has_pending_addr = 0;
        usb_devs[i].config = 0;
        usb_devs[i].ctl_stage = 0;
        usb_devs[i].qh = usb_devs[i].qt = 0;
        usb_devs[i].mqh = usb_devs[i].mqt = 0;
    }
    usb_portsc[0] = 0x1000 | 0x3;             /* composite HID keyboard+mouse */
    usb_portsc[1] = 0x1000 | 0x3;             /* storage or placeholder */
}

static uint32_t usb_frindex(void)
{
    if (!(usb_cmd & 1))
        return usb_frindex_base;
    return (uint32_t)(((mach_absolute_time() - usb_t0) / 3000) + usb_frindex_base) & 0x3fff;
}

static uint32_t usb_reg32(uint64_t off)
{
    if (getenv("GMBOX_USBLOG") && (off == 0x04 || off == 0x08 || off >= 0x64)) {
        static unsigned reglog;
        if (reglog++ < 80) fprintf(stderr, "USB reg read off=0x%llx\n", (unsigned long long)off);
    }
    switch (off) {
    case 0x00: return 0x01000020;                 /* HCIVERSION 1.00 | CAPLENGTH 0x20 */
    case 0x04: return USB_NPORTS;                 /* HCSPARAMS */
    case 0x08: return 0;                          /* HCCPARAMS */
    case 0x20: return usb_cmd;
    case 0x24: {
        uint32_t s = usb_sts & 0x3f;
        if (!(usb_cmd & 1)) s |= 0x1000;          /* HCHalted */
        if (usb_cmd & 0x10) s |= 0x4000;          /* periodic schedule status */
        if (usb_cmd & 0x20) s |= 0x8000;          /* async schedule status */
        return s;
    }
    case 0x28: return usb_intr;
    case 0x2c: return usb_frindex();
    case 0x30: return usb_ctrlds;
    case 0x34: return usb_perbase;
    case 0x38: return usb_asyncaddr;
    case 0x60: return usb_cfgflag;
    default:
        if (off >= 0x64 && off < 0x64 + 4 * USB_NPORTS)
            return usb_portsc[(off - 0x64) / 4];
        return 0;
    }
}

static uint64_t usb_read(uint64_t off, int size)
{
    uint32_t rv = usb_reg32(off & ~3ULL);
    uint32_t v = rv >> ((off & 3) * 8);
    if (getenv("GMBOX_USBLOG") && (off == 0x04 || off == 0x6c)) {
        static unsigned vlog;
        if (vlog++ < 30) fprintf(stderr, "USB reg value off=0x%llx v=0x%x\n", (unsigned long long)off, rv);
    }
    if (size == 1) v &= 0xff; else if (size == 2) v &= 0xffff;
    return v;
}

static void usb_write(uint64_t off, uint64_t val, int size)
{
    uint32_t v = (uint32_t)val;
    if (size != 4)
        return;
    switch (off) {
    case 0x20:
        if (v & 2) { usb_reset_regs(); v = usb_cmd; }   /* HCRESET self-clears */
        if ((v & 1) && !(usb_cmd & 1))
            usb_t0 = mach_absolute_time();
        if (!(v & 1) && (usb_cmd & 1))
            usb_frindex_base = usb_frindex();
        if (v & 0x40) { usb_sts |= 0x20; v &= ~0x40u; } /* IAAD -> IAA status */
        usb_cmd = v & ~2u;
        break;
    case 0x24: usb_sts &= ~(v & 0x3f); break;           /* W1C */
    case 0x28: usb_intr = v & 0x3f; break;
    case 0x2c: usb_frindex_base = v & 0x3fff; usb_t0 = mach_absolute_time(); break;
    case 0x30: usb_ctrlds = v; break;
    case 0x34: usb_perbase = v & ~0xfffu; break;
    case 0x38: usb_asyncaddr = v & ~0x1fu; break;
    case 0x60:
        usb_cfgflag = v & 1;
        break;
    default:
        if (off >= 0x64 && off < 0x64 + 4 * USB_NPORTS) {
            int p = (int)((off - 0x64) / 4);
            uint32_t old = usb_portsc[p];
            uint32_t n = old;
            n &= ~(v & 0x2a);                            /* W1C: CSC(1) PEDC(3) OCC(5) */
            if (!(v & 4)) n &= ~4u;                      /* PED can be cleared by software */
            if ((v & 0x100) && !(old & 0x100)) {         /* port reset begins */
                n |= 0x100; n &= ~4u;
                usb_port_reset[p] = 1;
            } else if (!(v & 0x100) && (old & 0x100)) {  /* port reset ends: HS device enabled */
                n &= ~0x100u;
                if (old & 1) { n |= 4 | 8; usb_devs[p].addr = 0; usb_devs[p].has_pending_addr = 0; }
                usb_port_reset[p] = 0;
            }
            n = (n & ~0x40u) | (v & 0x40);               /* force port resume bit: store */
            usb_portsc[p] = n | 0x1000;
        }
        break;
    }
    usb_irq_update();
}

/* guest-physical byte access through the five EHCI buffer pointers */
static int usb_buf_copy(const uint32_t *bp, uint32_t offset, uint8_t *data, uint32_t len, int to_guest)
{
    uint32_t done = 0;
    while (done < len) {
        uint32_t pos = offset + done;
        uint32_t pg = pos >> 12;
        if (pg > 4)
            return -1;
        uint64_t a = (uint64_t)(bp[pg] & ~0xfffu) + (pos & 0xfff);
        uint32_t chunk = 0x1000 - (pos & 0xfff);
        if (chunk > len - done) chunk = len - done;
        uint8_t *p = gpa(a, chunk);
        if (!p)
            return -1;
        if (to_guest) memcpy(p, data + done, chunk); else memcpy(data + done, p, chunk);
        done += chunk;
    }
    return 0;
}

static struct usb_dev *usb_find(uint32_t addr)
{
    for (int i = 0; i < USB_NPORTS; i++)
        if ((usb_portsc[i] & 4) && usb_devs[i].addr == addr)
            return &usb_devs[i];
    return NULL;
}

/* Run one queue head: execute active qTDs until NAK/empty. */
static void usb_qh(uint32_t qha, int periodic)
{
    uint32_t *qh = gpa(qha, 48);
    if (!qh)
        return;
    for (int it = 0; it < 16; it++) {
        uint32_t token = qh[6];
        if (!(token & 0x80)) {
            if (token & 0x40)
                return;                                  /* halted */
            uint32_t nx = qh[4];
            if (nx & 1)
                return;
            uint32_t *q = gpa(nx & ~0x1fu, 32);
            if (!q)
                return;
            qh[3] = nx & ~0x1fu;
            memcpy(&qh[4], q, 32);                       /* next, alt, token, buffers */
            continue;
        }
        uint32_t chr = qh[1];
        uint32_t addr = chr & 0x7f, ep = (chr >> 8) & 0xf;
        uint32_t pid = (token >> 8) & 3, total = (token >> 16) & 0x7fff;
        struct usb_dev *d = usb_find(addr);
        int n = 0, res = 0; /* res: 0 ok, 1 NAK, 2 stall, 3 no device */
        uint8_t buf[512];
        uint32_t offset = qh[7] & 0xfff;
        if (!d) {
            res = 3;
        } else if (ep == 0) {
            if (pid == 2) {                              /* SETUP */
                uint8_t s[8];
                if (usb_buf_copy(&qh[7], offset, s, 8, 0) < 0 || usb_setup(d, s) < 0)
                    res = 2;
                n = 8;
            } else if (pid == 1) {                       /* IN */
                if (d->ctl_stage == 1) {                 /* data stage */
                    n = d->ctl_len < (int)total ? d->ctl_len : (int)total;
                    if (n > 0 && usb_buf_copy(&qh[7], offset, d->ctl_buf, (uint32_t)n, 1) < 0)
                        res = 2;
                    d->ctl_stage = 2;
                } else {
                    n = 0;                               /* status stage (zero-length) */
                    d->ctl_stage = 0;
                }
            } else {                                     /* OUT */
                if (total && total <= sizeof buf)
                    usb_buf_copy(&qh[7], offset, buf, total, 0);
                n = (int)total;
                d->ctl_stage = (d->ctl_stage == 1 && !d->ctl_dir_in) ? 2 : 0;
            }
            if (res == 0 && d->has_pending_addr && pid != 2) {
                d->addr = d->pending_addr;
                d->has_pending_addr = 0;
            }
        } else if (d->is_msc) {
            int mr = msc_bulk(d, ep, pid, qh, total, offset);
            if (mr < 0) {
                if (mr == -1) return;
                res = 2;
            } else {
                n = mr;
            }
        } else if (ep == 1 && pid == 1) {                /* keyboard interrupt IN */
            if (d->qh == d->qt) {
                res = 1;
            } else {
                int len = 8;
                n = (int)total < len ? (int)total : len;
                usb_buf_copy(&qh[7], offset, d->q[d->qh], (uint32_t)n, 1);
                d->qh = (d->qh + 1) & 255;
            }
        } else if (d->is_composite && ep == 2 && pid == 1) {  /* mouse interrupt IN */
            if (d->mqh == d->mqt) {
                res = 1;
            } else {
                int len = 4;
                n = (int)total < len ? (int)total : len;
                usb_buf_copy(&qh[7], offset, d->mq[d->mqh], (uint32_t)n, 1);
                d->mqh = (d->mqh + 1) & 255;
            }
        } else {
            res = 2;
        }
        if (res == 1)
            return;                                      /* NAK: retry next tick */
        if (res == 3)
            return;                                      /* no such device yet */
        uint32_t next = qh[4];
        if (res == 2) {
            token = (token & ~0x80u) | 0x40;             /* Halted (STALL) */
        } else {
            uint32_t rem = total - (uint32_t)n;
            token = (token & ~(0x7fffu << 16) & ~0xffu) | (rem << 16);
            if (pid == 1 && (uint32_t)n < total && !(qh[5] & 1))
                next = qh[5];                            /* short packet: alternate */
            else
                next = qh[4];
        }
        qh[6] = token;
        uint32_t *cur = gpa(qh[3] & ~0x1fu, 32);
        if (cur)
            cur[2] = token;                              /* write status back to the qTD */
        if (token & 0x8000)
            usb_sts |= 1;                                /* IOC -> USBINT */
        if (res == 2)
            usb_sts |= 2;                                /* USBERRINT */
        qh[4] = next;
        (void)periodic;
        if (res == 2)
            return;
    }
}

static void usb_walk_list(uint32_t first, int periodic, uint32_t stop)
{
    uint32_t link = first;
    for (int guard = 0; guard < 64; guard++) {
        if (link & 1)
            return;
        uint32_t a = link & ~0x1fu;
        if (guard && stop && a == stop)
            return;
        uint32_t *p = gpa(a, 4);
        if (!p)
            return;
        if (((link >> 1) & 3) == 1)
            usb_qh(a, periodic);
        link = *p;
    }
}

static void usb_tick(void)
{
    usb_input_poll();
    if (!g_usb_enabled)
        return;
    for (int i = 0; i < USB_NPORTS; i++)
        if ((usb_portsc[i] & 0x2a) && (usb_cmd & 1)) {
            if (!(usb_sts & 4)) usb_sts |= 4;            /* port change detect */
        }
    if (usb_cmd & 1) {
        if ((usb_cmd & 0x20) && usb_asyncaddr)
            usb_walk_list(usb_asyncaddr | 2, 0, usb_asyncaddr);
        if ((usb_cmd & 0x10) && usb_perbase) {
            uint32_t idx = (usb_frindex() >> 3) & 0x3ff;
            uint32_t *fl = gpa(usb_perbase + idx * 4, 4);
            if (fl)
                usb_walk_list(*fl, 1, 0);
        }
    }
    usb_irq_update();
}

static void *usb_tick_thread(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(1000);
        g_usb_tick = 1;
        hv_vcpus_exit(&g_vcpu, 1);
    }
    return NULL;
}

#include "hvf_usb_msc.inc"

static void usb_init(void)
{
    const char *np = getenv("GMBOX_NO_USB");
    if (np && *np) {
        g_usb_enabled = 0;
        return;
    }
    usb_devs[0].is_kbd = 1;
    usb_devs[0].is_composite = 1;
    usb_devs[1].is_kbd = 0;
    usb_reset_regs();
    msc_init();
    /* PCI function: bus 0 dev 2 fn 0, class 0c03/20 (EHCI), Intel ICH9 ids */
    unsigned n = 0x10000;
    memset(pci_cfg + n, 0, 0x100);
    pci_cfg[n + 0x00] = 0x86; pci_cfg[n + 0x01] = 0x80;
    pci_cfg[n + 0x02] = 0x3a; pci_cfg[n + 0x03] = 0x29;
    pci_cfg[n + 0x08] = 0x02;                       /* revision */
    pci_cfg[n + 0x09] = 0x20;                       /* prog-if: EHCI */
    pci_cfg[n + 0x0a] = 0x20; pci_cfg[n + 0x0b] = 0x0c;
    usb_bar = USB_IPA;
    memcpy(pci_cfg + n + 0x10, &(uint32_t){ USB_IPA }, 4);
    pci_cfg[n + 0x2c] = 0xf4; pci_cfg[n + 0x2d] = 0x1a; pci_cfg[n + 0x2e] = 0x00; pci_cfg[n + 0x2f] = 0x11;
    pci_cfg[n + 0x3c] = 0x25; pci_cfg[n + 0x3d] = 0x01;
    pci_cfg[n + 0x60] = 0x20;                       /* SBRN */
    pci_cfg[n + 0x61] = 0x20;                       /* FLADJ */
    const char *fifo = getenv("GMBOX_INPUT");
    if (fifo && *fifo) {
        g_usb_fifo = open(fifo, O_RDONLY | O_NONBLOCK);
        if (g_usb_fifo < 0)
            fprintf(stderr, "usb: cannot open input fifo %s: %s\n", fifo, strerror(errno));
    }
    pthread_t t;
    pthread_create(&t, NULL, usb_tick_thread, NULL);
}
