/*
 * Minimal virtio-mmio block device for GMBox.
 *
 * Modern virtio 1.0, one request queue, polling completion.  Included by
 * hvf_win.c after gpa()/disk_fd/disk_bytes/lba_shift are declared.
 */

#define VIRTIO_MMIO_IPA   0x0a000000UL
#define VIRTIO_MMIO_SIZE  0x200UL
#define VIRTIO_QMAX       64

struct vqueue {
    uint32_t num, ready;
    uint64_t desc, avail, used;
    uint16_t last_avail;
};

static struct vqueue vq;
static uint32_t virtio_status, virtio_qsel, virtio_feat_sel;
static uint64_t virtio_driver_features;
static uint32_t virtio_intr;

static uint32_t vle32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t vle64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static void vst32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static int vread(uint64_t addr, void *dst, size_t n)
{
    uint8_t *p = gpa(addr, n);
    if (!p)
        return 0;
    memcpy(dst, p, n);
    return 1;
}

static int vwrite(uint64_t addr, const void *src, size_t n)
{
    uint8_t *p = gpa(addr, n);
    if (!p)
        return 0;
    memcpy(p, src, n);
    return 1;
}

static void virtio_complete(uint16_t head, uint32_t len)
{
    uint16_t idx = 0;
    uint8_t e[8];
    if (!vq.used || !vq.num)
        return;
    vread(vq.used + 2, &idx, 2);
    vst32(e, head);
    vst32(e + 4, len);
    vwrite(vq.used + 4 + (uint64_t)(idx % vq.num) * 8, e, 8);
    idx++;
    vwrite(vq.used + 2, &idx, 2);
    virtio_intr |= 1;
}

static void virtio_process(void)
{
    uint16_t aidx = 0;
    unsigned guard = 0;

    if (!vq.ready || !vq.num || !vq.desc || !vq.avail || !vq.used)
        return;
    if (!vread(vq.avail + 2, &aidx, 2))
        return;

    while (vq.last_avail != aidx && guard++ < 2 * vq.num) {
        uint16_t head = 0;
        uint16_t idx, n = 0;
        uint64_t addr[VIRTIO_QMAX];
        uint32_t lens[VIRTIO_QMAX];
        uint16_t next[VIRTIO_QMAX];

        if (!vread(vq.avail + 4 + (uint64_t)(vq.last_avail % vq.num) * 2,
                   &head, 2))
            return;
        vq.last_avail++;

        idx = head;
        while (n < VIRTIO_QMAX) {
            uint8_t d[16];
            uint16_t flags;
            if (!vread(vq.desc + (uint64_t)idx * 16, d, sizeof(d)))
                break;
            addr[n] = vle64(d);
            lens[n] = vle32(d + 8);
            flags = (uint16_t)vle32(d + 12);
            next[n] = (uint16_t)(vle32(d + 12) >> 16);
            n++;
            if (!(flags & 1))
                break;
            idx = next[n - 1];
        }

        if (n < 2) {
            virtio_complete(head, 0);
            continue;
        }

        {
            uint8_t hdr[16];
            uint32_t type;
            uint64_t sector;
            uint32_t total = 0;
            uint8_t status = 0;

            if (!vread(addr[0], hdr, sizeof(hdr))) {
                virtio_complete(head, 0);
                continue;
            }
            type = vle32(hdr);
            sector = vle64(hdr + 8);

            for (uint16_t i = 1; i + 1 < n; i++) {
                uint32_t len = lens[i];
                uint8_t *tmp;

                if (len > (1u << 20)) {
                    status = 1;
                    break;
                }
                tmp = malloc(len);
                if (!tmp) {
                    status = 1;
                    break;
                }
                if (type == 0) {
                    ssize_t got = pread(disk_fd, tmp, len,
                                        (off_t)(sector * 512ULL + total));
                    if (got < 0 || !vwrite(addr[i], tmp, (size_t)got))
                        status = 1;
                } else if (type == 1) {
                    if (!vread(addr[i], tmp, len))
                        status = 1;
                    else if (pwrite(disk_fd, tmp, len,
                                     (off_t)(sector * 512ULL + total)) < 0)
                        status = 1;
                }
                free(tmp);
                total += len;
            }
            if (type == 4)
                fsync(disk_fd);
            else if (type != 0 && type != 1)
                status = 1;

            vwrite(addr[n - 1], &status, 1);
            virtio_complete(head, 0);
        }
    }
}

static uint64_t virtio_read(uint64_t off, int size)
{
    uint64_t v = 0;

    switch (off) {
    case 0x000: v = 0x74726976; break; /* "virt" */
    case 0x004: v = 2; break;          /* modern */
    case 0x008: v = 2; break;          /* block */
    case 0x00c: v = 0x554d4551; break; /* "QEMU" */
    case 0x010: v = virtio_feat_sel == 0 ? 0x24eULL : 1ULL; break;
    case 0x020:
        v = virtio_feat_sel == 0 ? (virtio_driver_features & 0xffffffffULL)
                                 : (virtio_driver_features >> 32);
        break;
    case 0x030: v = virtio_qsel; break;
    case 0x034: v = VIRTIO_QMAX; break;
    case 0x038: v = vq.num; break;
    case 0x044: v = vq.ready; break;
    case 0x060: v = virtio_intr; break;
    case 0x070: v = virtio_status; break;
    case 0x080: v = vq.desc & 0xffffffffULL; break;
    case 0x084: v = vq.desc >> 32; break;
    case 0x090: v = vq.avail & 0xffffffffULL; break;
    case 0x094: v = vq.avail >> 32; break;
    case 0x0a0: v = vq.used & 0xffffffffULL; break;
    case 0x0a4: v = vq.used >> 32; break;
    case 0x0fc: v = 0; break;
    case 0x100: v = disk_bytes >> lba_shift; break; /* capacity */
    case 0x108: v = 1u << lba_shift; break;          /* blk_size */
    case 0x10c: v = 126; break;                       /* seg_max */
    case 0x110: v = 1u << 20; break;                  /* size_max */
    case 0x114: v = 1; break;
    case 0x118: v = 1; break;
    default: break;
    }
    if (size == 4)
        v &= 0xffffffffULL;
    return v;
}

static void virtio_write(uint64_t off, uint64_t value, int size)
{
    if (size == 4)
        value &= 0xffffffffULL;

    switch (off) {
    case 0x014: virtio_feat_sel = (uint32_t)value; break;
    case 0x024:
        if (virtio_feat_sel == 0)
            virtio_driver_features =
                (virtio_driver_features & 0xffffffff00000000ULL) |
                (uint32_t)value;
        else
            virtio_driver_features =
                (virtio_driver_features & 0xffffffffULL) | (value << 32);
        break;
    case 0x030: virtio_qsel = (uint32_t)value; break;
    case 0x038:
        vq.num = (uint32_t)value;
        if (vq.num > VIRTIO_QMAX)
            vq.num = VIRTIO_QMAX;
        break;
    case 0x044:
        vq.ready = (uint32_t)value;
        if (vq.ready)
            vq.last_avail = 0;
        break;
    case 0x050: virtio_process(); break;
    case 0x064: virtio_intr &= ~(uint32_t)value; break;
    case 0x070:
        virtio_status = (uint32_t)value;
        if (virtio_status == 0) {
            vq.ready = 0;
            vq.num = 0;
            vq.last_avail = 0;
            virtio_intr = 0;
            virtio_driver_features = 0;
        }
        break;
    case 0x080: vq.desc = (vq.desc & 0xffffffff00000000ULL) | (uint32_t)value; break;
    case 0x084: vq.desc = (vq.desc & 0xffffffffULL) | (value << 32); break;
    case 0x090: vq.avail = (vq.avail & 0xffffffff00000000ULL) | (uint32_t)value; break;
    case 0x094: vq.avail = (vq.avail & 0xffffffffULL) | (value << 32); break;
    case 0x0a0: vq.used = (vq.used & 0xffffffff00000000ULL) | (uint32_t)value; break;
    case 0x0a4: vq.used = (vq.used & 0xffffffffULL) | (value << 32); break;
    default: break;
    }
}
