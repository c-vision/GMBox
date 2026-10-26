/*
 * GMBox native Hypervisor.framework VMM: EDK2 + PCIe ECAM + NVMe.
 *
 * Boots the QEMU AArch64 EDK2 firmware with a supplied device tree, emulates
 * a GICv3 (HVF in-kernel) redistributor, PL011, PL031, a PCIe ECAM host
 * bridge and one NVMe controller backed by a raw disk image.
 *
 * C99. No QEMU, no Python.
 */

#include <Hypervisor/Hypervisor.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <mach/mach_time.h>
#include <libkern/OSCacheControl.h>

#define FW_IPA      0x00000000UL
#define FW_SIZE     (64UL * 1024 * 1024)
#define VARS_IPA    0x04000000UL
#define VARS_SIZE   (64UL * 1024 * 1024)
#define RAM_IPA     0x40000000UL
#define RAM_SIZE    (8UL * 1024 * 1024 * 1024)
#define DTB_IPA     (RAM_IPA + 0x10000UL)

#define GICD_IPA    0x08000000UL
#define GICR_IPA    0x080A0000UL
#define UART_IPA    0x09000000UL
#define RTC_IPA     0x09010000UL
#define ECAM_IPA    0x3F000000UL
#define ECAM_SIZE   0x01000000UL
#define MMIO32_IPA  0x10000000UL
#define MMIO32_SIZE 0x2EFF0000UL
#define NVME_IPA    0x20000000UL
#define NVME_SIZE   0x00004000UL
#define FWCFG_IPA   0x09020000UL
#define FWCFG_SIZE  0x00000018UL
#define NVME_INTID  36u /* GIC SPI 4: DTB interrupt-map slot 1 INTA */

#define FW_PATH     "/Users/gaetano/Documents/Projects/GMBox/tools/firmware/edk2-aarch64-gmbox-pci2-boot-code.fd"
#define VARS_PATH   "/Users/gaetano/Documents/Projects/GMBox/vms/Windows 11 Pro.vmfc/vars.fd"
#define DISK_PATH   "/Users/gaetano/Documents/Projects/GMBox/vms/Windows 11 Pro.vmfc/disk.raw"
#define DTB_PATH    "/Users/gaetano/Documents/Projects/GMBox/tools/qemu-virt-nvme-gic3-flash.dtb"

#define NVME_QMAX   16
#define NVME_QSIZE  64

static void *g_fw, *g_vars, *g_ram;
static uint8_t pci_cfg[0x18000];
#define USB_SIZE 0x1000UL
static uint64_t usb_bar;
static int g_usb_enabled = 1;
static void usb_irq_update(void);
static pthread_mutex_t nvme_mu = PTHREAD_MUTEX_INITIALIZER;
static void nvme_update_irq(void);
static int disk_fd = -1;
static uint64_t disk_bytes;
static int lba_shift = 9;

/* ------------------------------------------------------------------ */
/* CFI NOR flash emulation (VARS region 0x04000000-0x08000000)       */
/* EDK2's FlashNvStorage probes this device for NVRAM variable storage.*/
/* We intercept writes (commands) and pre-populate reads (CFI data,   */
/* status register) so EDK2 can install Variable Arch Protocol and    */
/* reach BDS/PCI enumeration without hanging.                         */

#define CFI_VARS_BASE  VARS_IPA
#define CFI_VARS_SIZE  VARS_SIZE

static uint8_t cfi_data[4096];             /* small CFI query buffer */

/* Minimal CFI query structure (device geometry) */
static void cfi_init_query(void)
{
    memset(cfi_data, 0, sizeof(cfi_data));
    cfi_data[0x00] = 'Q'; cfi_data[0x01] = 'R'; cfi_data[0x02] = 'Y';
    cfi_data[0x03] = 0x02; cfi_data[0x04] = 0x00; /* CFI v1.0 */
    cfi_data[0x05] = 0x01; /* Address sensitive unlock */
    cfi_data[0x06] = 0x00;
    cfi_data[0x07] = 0x00;
    cfi_data[0x08] = 0x00; /* Vcc min */
    cfi_data[0x09] = 0x00;
    cfi_data[0x0a] = 0x1b; /* Vcc max 2.7V */
    cfi_data[0x0b] = 0x00;
    cfi_data[0x0c] = 0x00; /* Vpp min */
    cfi_data[0x0d] = 0x00;
    cfi_data[0x0e] = 0x00; /* Vpp max */
    cfi_data[0x0f] = 0x00;
    /* Word write timeout = 2^0 * 1ms = 1ms */
    cfi_data[0x14] = 0x01; cfi_data[0x15] = 0x00;
    /* Block erase timeout = 2^1 * 1ms = 2ms */
    cfi_data[0x16] = 0x02; cfi_data[0x17] = 0x00;
    /* Device size: 2^22 bytes = 4 MiB */
    cfi_data[0x1a] = 0x1a; /* 64 MiB */
    /* Block size: 2^12 bytes = 4 KiB regions */
    cfi_data[0x1c] = 0x12; /* 256 KiB */
    /* Number of erase block regions */
    cfi_data[0x2a] = 0x01;
    cfi_data[0x2b] = 0x00;
    /* Region 0: 0x400 blocks of 4 KiB */
    cfi_data[0x2c] = 0x00; cfi_data[0x2d] = 0xff;
    cfi_data[0x2e] = 0x00; cfi_data[0x2f] = 0x04;
}

/* Queue pair slot i holds submission queue i (sq_*) and completion queue i
 * (cq_*); an SQ completes into nq[cqid]: several SQs may share one CQ. */
struct nvme_q {
    uint32_t sq_head, sq_tail, cq_head, cq_tail;
    uint32_t sqsize, cqsize;
    uint64_t sq_base, cq_base;
    uint16_t cqid;
    uint8_t phase;
    int ien;
    int live;
};
/* NVMe status codes: SCT<<8 | SC, DNR = bit 14 */
#define NVME_SC_INVALID_OPCODE   (0x4000 | 0x0001)
#define NVME_SC_INVALID_FIELD    (0x4000 | 0x0002)
#define NVME_SC_INVALID_LOG_PAGE (0x4000 | (1 << 8) | 0x0009)
static struct nvme_q nq[NVME_QMAX];
static uint64_t nvme_cc, nvme_csts, nvme_aqa, nvme_asq, nvme_acq;
static uint64_t nvme_bar = NVME_IPA;
static int nvme_enabled;
static int nvme_irq_level;
static uint16_t fw_sel;
static uint32_t fw_pos;
#define FW_FILES 5
static uint8_t fw_data[FW_FILES][8192];
static uint32_t fw_sizes[FW_FILES];
static uint8_t fw_dir[4 + FW_FILES * 64];
static uint32_t fw_dir_len;
static const char *fw_names[FW_FILES] = {
    "etc/hardware-info", "etc/acpi/tables", "etc/acpi/rsdp", "etc/table-loader",
    "etc/ramfb"
};

/* QEMU PCIHostBridgeInfo structure for etc/hardware-info (fw_cfg).
 * Field order must match QEMU's hw/i386/pc.c exactly: version, segment,
 * ecam_base, io_base, io_limit, mem32_base, mem32_limit, mem64_base,
 * mem64_limit.  All little-endian. */
typedef struct __attribute__((packed)) {
    uint16_t version;              /* 1 */
    uint16_t pci_express_segment;  /* 0 */
    uint64_t pci_express_base_address; /* ECAM_IPA */
    uint16_t pci_io_base;          /* 0 (no IO space on ARM) */
    uint16_t pci_io_limit;         /* 0 */
    uint64_t pci_mem32_base;       /* MMIO32_IPA */
    uint64_t pci_mem32_limit;      /* MMIO32_IPA + MMIO32_SIZE */
    uint64_t pci_mem64_base;       /* 0 */
    uint64_t pci_mem64_limit;      /* 0 */
} pci_host_bridge_info_t;
static unsigned ecam_hits, nvme_hits;
static uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }

static hv_vcpu_t g_vcpu;
static volatile int g_watchdog_fired;

static void fail(hv_return_t r, const char *what)
{
    if (r == HV_SUCCESS)
        return;
    fprintf(stderr, "%s: hv_return_t=0x%x\n", what, (unsigned)r);
    exit(1);
}

static void *map_anon(size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    return p;
}

static void load_file(const char *path, void *dst, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        exit(1);
    }
    size_t n = fread(dst, 1, max, f);
    if (ferror(f)) {
        fprintf(stderr, "%s: read error\n", path);
        exit(1);
    }
    fclose(f);
    printf("loaded %s: %zu bytes\n", path, n);
}

static void *gpa(uint64_t a, uint64_t len)
{
    if (a < RAM_IPA || a + len < a || a + len > RAM_IPA + RAM_SIZE)
        return NULL;
    return (uint8_t *)g_ram + (a - RAM_IPA);
}

static uint32_t guest_read32(uint64_t a)
{
    uint32_t v = 0xffffffffu;
    if (a >= FW_IPA && a + 4 <= FW_IPA + FW_SIZE)
        memcpy(&v, (uint8_t *)g_fw + (a - FW_IPA), 4);
    else if (a >= VARS_IPA && a + 4 <= VARS_IPA + VARS_SIZE)
        memcpy(&v, (uint8_t *)g_vars + (a - VARS_IPA), 4);
    else {
        void *p = gpa(a, 4);
        if (p)
            memcpy(&v, p, 4);
    }
    return v;
}

static int insn_is_hvc_smc(uint32_t w)
{
    return (w & 0xffe0001fu) == 0xd4000002u ||
           (w & 0xffe0001fu) == 0xd4000003u;
}

/* ------------------------------------------------------------------ */
/* PCI configuration space (bus 0 only; ECAM covers 16 buses)          */

static uint64_t cfg_read(unsigned off, int size)
{
    uint64_t v = 0;
    if (off >= sizeof(pci_cfg))
        return 0xffffffffffffffffULL;
    memcpy(&v, pci_cfg + off, (size_t)size);
    return v;
}

static void cfg_write(unsigned off, int size, uint64_t value)
{
    if (off >= sizeof(pci_cfg))
        return;
    if (getenv("GMBOX_PCILOG") && off >= 0x8000 && off < 0x8100) fprintf(stderr, "PCICFGW off=0x%x size=%d val=0x%llx\n", off - 0x8000, size, (unsigned long long)value);

    /* Unimplemented BARs (BAR1-5 at 0x14..0x27, expansion ROM at 0x30) must be
     * read-only zero, otherwise a sizing write of all-ones reads back as a
     * 4 GiB I/O BAR and Windows endlessly tries to rebalance resources. */
    if (((off & 0xfff) >= 0x14 && (off & 0xfff) < 0x28) || ((off & 0xfff) >= 0x30 && (off & 0xfff) < 0x34))
        return;
    if ((off & 0xf000) == 0x0000 && off >= 0x10000) { /* dev 2: USB EHCI */
        unsigned r = off & 0xfff;
        if (r == 0x10) {
            uint32_t v = (uint32_t)value;
            if (size == 4 && v == 0xffffffffu) { uint32_t m = (uint32_t)~(USB_SIZE - 1); memcpy(pci_cfg + off, &m, 4); return; }
            v &= ~(uint32_t)(USB_SIZE - 1);
            memcpy(pci_cfg + off, &v, 4);
            usb_bar = v;
            return;
        }
        if (r == 4 && size == 4) { uint16_t c = (uint16_t)value, cl = (uint16_t)(value >> 16); memcpy(pci_cfg + off, &c, 2); pci_cfg[0x10006] &= (uint8_t)~(cl & 0xe8 & ~0x18); pci_cfg[0x10007] &= (uint8_t)~(cl >> 8); usb_irq_update(); return; }
        if (r == 4 && size == 2) { memcpy(pci_cfg + off, &value, 2); usb_irq_update(); return; }
        if (r == 6 && size == 2) { pci_cfg[0x10006] &= (uint8_t)~((uint16_t)value & 0xe8); pci_cfg[0x10007] &= (uint8_t)~((uint16_t)value >> 8); return; }
        if (r == 0x3c || (r >= 0x0c && r < 0x10)) { memcpy(pci_cfg + off, &value, (size_t)size); return; }
        return;
    }
    /* BAR0 of the NVMe device (bus 0, dev 1, fn 0, offset 0x10). */
    if (off == 0x8010) {
        if (size == 4 && (value & 0xffffffffu) == 0xffffffffu) {
            uint32_t mask = (uint32_t)~(NVME_SIZE - 1);
            memcpy(pci_cfg + off, &mask, 4);
            return;
        }
        uint32_t v = (uint32_t)value & ~(NVME_SIZE - 1);
        memcpy(pci_cfg + off, &v, 4);
        nvme_bar = v;
        fprintf(stderr, "PCI BAR0 assigned 0x%x (window 0x%lx+0x%lx)\n",
                v, (unsigned long)MMIO32_IPA, (unsigned long)MMIO32_SIZE);
        return;
    }
    /* PCI Status (0x8006) is write-1-to-clear for the error bits; bit 3
     * (Interrupt Status) and bit 4 (capabilities) are read-only. */
    if (off == 0x8004 && size == 4) {
        uint16_t cmd = (uint16_t)value, clr = (uint16_t)(value >> 16);
        memcpy(pci_cfg + 0x8004, &cmd, 2);
        pci_cfg[0x8006] &= (uint8_t)~(clr & 0x00ff & ~0x18);
        pci_cfg[0x8007] &= (uint8_t)~(clr >> 8);
        pthread_mutex_lock(&nvme_mu);
        nvme_update_irq();
        pthread_mutex_unlock(&nvme_mu);
        return;
    }
    if (off == 0x8006 && size == 2) {
        pci_cfg[0x8006] &= (uint8_t)~((uint16_t)value & 0x00ff & ~0x18);
        pci_cfg[0x8007] &= (uint8_t)~((uint16_t)value >> 8);
        return;
    }
    memcpy(pci_cfg + off, &value, (size_t)size);
    if (off >= 0x8004 && off < 0x8006) {
        pthread_mutex_lock(&nvme_mu);
        nvme_update_irq();
        pthread_mutex_unlock(&nvme_mu);
    }
}

static void pci_init(void)
{
    memset(pci_cfg, 0xff, sizeof(pci_cfg));

    /* Host bridge at bus 0 dev 0 fn 0 (Red Hat QEMU PCIe host). */
    memset(pci_cfg, 0, 0x40);
    pci_cfg[0x00] = 0x36; pci_cfg[0x01] = 0x1b;
    pci_cfg[0x02] = 0x08; pci_cfg[0x03] = 0x00;
    pci_cfg[0x0a] = 0x00;
    pci_cfg[0x0b] = 0x06;

    unsigned n = 0x8000; /* dev 1, fn 0 */
    memset(pci_cfg + n, 0, 0x40);
    pci_cfg[n + 0x00] = 0x36; pci_cfg[n + 0x01] = 0x1b;
    pci_cfg[n + 0x02] = 0x10; pci_cfg[n + 0x03] = 0x00;
    pci_cfg[n + 0x08] = 0x01;
    pci_cfg[n + 0x09] = 0x02;
    pci_cfg[n + 0x0a] = 0x08;
    pci_cfg[n + 0x0b] = 0x01;
    pci_cfg[n + 0x10] = (uint8_t)(NVME_IPA >> 0);
    pci_cfg[n + 0x11] = (uint8_t)(NVME_IPA >> 8);
    pci_cfg[n + 0x12] = (uint8_t)(NVME_IPA >> 16);
    pci_cfg[n + 0x13] = (uint8_t)(NVME_IPA >> 24);
    pci_cfg[n + 0x2c] = 0xf4; pci_cfg[n + 0x2d] = 0x1a;
    pci_cfg[n + 0x2e] = 0x00; pci_cfg[n + 0x2f] = 0x11;
    pci_cfg[n + 0x3c] = 0x24;
    pci_cfg[n + 0x3d] = 0x01;
    nvme_bar = NVME_IPA;
}

/* ------------------------------------------------------------------ */
/* NVMe                                                                */

static void nvme_irq(int level)
{
    if (getenv("GMBOX_NVME_NOIRQ")) return;
    if (nvme_irq_level == level)
        return;
    nvme_irq_level = level;
    hv_gic_set_spi(NVME_INTID, level != 0);
    if (level && getenv("GMBOX_NVME_PULSE")) { /* edge-like pulse (experiment) */
        usleep(200);
        hv_gic_set_spi(NVME_INTID, false);
        nvme_irq_level = 0;
    }
}

static uint32_t nvme_intms;

/* INTx line = some completion queue has unconsumed entries and vector 0 is
 * not masked via INTMS/INTMC. */
static void nvme_update_irq(void)
{
    int pending = 0;
    for (int i = 0; i < NVME_QMAX; i++)
        if (nq[i].cqsize && nq[i].ien && nq[i].cq_head != nq[i].cq_tail)
            pending = 1;
    int cmd_dis = (pci_cfg[0x8005] >> 2) & 1;       /* PCI command bit 10: INTx Disable */
    int line = pending && !(nvme_intms & 1u);
    /* PCI status bit 3: Interrupt Status (reflects the device, not the mask) */
    if (line) pci_cfg[0x8006] |= 0x08; else pci_cfg[0x8006] &= (uint8_t)~0x08;
    nvme_irq(line && !cmd_dis);
}

static void nvme_reset(void)
{
    nvme_intms = 0;
    memset(nq, 0, sizeof(nq));
    nvme_csts = 0;
    nvme_enabled = 0;
    nq[0].phase = 1;
    nq[0].ien = 1;
    nvme_irq(0);
}

/* Post a completion for a command taken from submission queue `sq`; it goes
 * to the completion queue that SQ was bound to at creation. */
static void nvme_complete(struct nvme_q *sq, uint16_t cid, uint16_t status,
                          uint32_t result)
{
    struct nvme_q *cq = &nq[sq->cqid];
    uint8_t *p = gpa(cq->cq_base, (uint64_t)cq->cqsize * 16);
    if (!p || !cq->cqsize)
        return;
    uint32_t *e = (uint32_t *)(p + (uint64_t)cq->cq_tail * 16);
    e[0] = result;
    e[1] = 0;
    e[2] = ((uint32_t)sq->sq_head & 0xffffu) |
           ((uint32_t)(sq - nq) << 16);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    e[3] = ((uint32_t)cid & 0xffffu) |
           ((uint32_t)(cq->phase & 1u) << 16) |
           ((uint32_t)status << 17);
    sys_dcache_flush((void *)e, 16); /* guest may map the queue non-cacheable */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cq->cq_tail++;
    if (cq->cq_tail >= cq->cqsize) {
        cq->cq_tail = 0;
        cq->phase ^= 1;
    }
    nvme_update_irq();
}

static int g_patch_pmu;
#include "hvf_walk.inc"
static unsigned g_vt_exits;
static int g_st_arm_now, g_st_wait_db;
static unsigned g_admin_db_at, g_cc_en_n;
static int g_pcsample;
static hv_vcpu_t g_pcs_vcpu;
static struct { uint64_t pc; unsigned n; uint8_t irqmasked; } g_pcs[2048];
static unsigned g_pcs_total;
static void pcs_add(uint64_t pc, uint64_t cpsr)
{
    g_pcs_total++;
    for (unsigned i = 0; i < 2048; i++) {
        if (g_pcs[i].n && g_pcs[i].pc == pc) { g_pcs[i].n++; return; }
        if (!g_pcs[i].n) { g_pcs[i].pc = pc; g_pcs[i].n = 1; g_pcs[i].irqmasked = (cpsr >> 7) & 1; return; }
    }
}
static struct { uint64_t e, l, f; unsigned n; } g_exs[256];
static void exs_add(uint64_t e, uint64_t l, uint64_t f)
{
    for (unsigned i = 0; i < 256; i++) {
        if (g_exs[i].n && g_exs[i].e == e && g_exs[i].l == l && g_exs[i].f == f) { g_exs[i].n++; return; }
        if (!g_exs[i].n) { g_exs[i].e = e; g_exs[i].l = l; g_exs[i].f = f; g_exs[i].n = 1; return; }
    }
}
static void pcs_dump(void)
{
    for (unsigned i = 0; i < 256 && g_exs[i].n; i++) fprintf(stderr, "EXSAMPLE esr=0x%llx elr=0x%llx far=0x%llx n=%u\n", (unsigned long long)g_exs[i].e, (unsigned long long)g_exs[i].l, (unsigned long long)g_exs[i].f, g_exs[i].n);
    fprintf(stderr, "PCSAMPLE total=%u\n", g_pcs_total);
    for (int k = 0; k < 20; k++) {
        unsigned best = 0, bi = 0;
        for (unsigned i = 0; i < 2048; i++) if (g_pcs[i].n > best) { best = g_pcs[i].n; bi = i; }
        if (!best) break;
        fprintf(stderr, "  pc=0x%llx n=%u irqmask=%u\n", (unsigned long long)g_pcs[bi].pc, best, g_pcs[bi].irqmasked);
        if (k < 8) { char nm[96]; snprintf(nm, sizeof nm, "/tmp/claude-501/hot%d.bin", k); dump_guest_va(g_pcs_vcpu, "HOT", g_pcs[bi].pc & ~0xfffULL, 0x1000, nm); }
        g_pcs[bi].n = 0;
    }
}
static void *pcs_thread(void *arg)
{
    (void)arg;
    for (;;) { usleep(5000); if (g_watchdog_fired) break; hv_vcpus_exit(&g_pcs_vcpu, 1); }
    return NULL;
}
static int g_kdbrk;
static void patch_pmu_access(void *buf, size_t len)
{
    uint32_t *w = (uint32_t *)buf;
    for (size_t i = 0; i + 4 <= len; i += 4) {
        uint32_t insn = w[i / 4];
        if ((insn & 0xffffff00u) == 0xd53b9c00u ||
            (insn & 0xffffff00u) == 0xd53b9d00u ||
            (insn & 0xffffff00u) == 0xd53b9e00u ||
            (insn & 0xffffff00u) == 0xd53b9f00u) {
            w[i / 4] = 0xd2800000u | (insn & 0x1fu);
        } else if ((insn & 0xffffff00u) == 0xd51b9c00u ||
                   (insn & 0xffffff00u) == 0xd5189c00u ||
                   (insn & 0xffffff00u) == 0xd5189e00u ||
                   (insn & 0xffffff00u) == 0xd51b9e00u ||
                   (insn & 0xffffff00u) == 0xd51b9f00u ||
                   (insn & 0xffffff00u) == 0xd51bef00u) {
            w[i / 4] = 0xd503201fu;
        }
    }
}

static int disk_io(uint64_t byte_off, void *buf, size_t len, int write)
{
    if (disk_fd < 0 || byte_off + len > disk_bytes)
        return -1;
    ssize_t r = write ? pwrite(disk_fd, buf, len, (off_t)byte_off)
                      : pread(disk_fd, buf, len, (off_t)byte_off);
    return r == (ssize_t)len ? 0 : -1;
}

static void nvme_prp_io(uint64_t prp1, uint64_t prp2, uint64_t len,
                        uint64_t disk_off, int write)
{
    size_t done = 0;

    if (len == 0)
        return;

    size_t chunk = 4096 - (prp1 & 0xfff);
    if (chunk > len)
        chunk = len;
    void *p = gpa(prp1, chunk);
    if (!p || disk_io(disk_off, p, chunk, write) < 0)
        return;
    if (!write && g_patch_pmu) patch_pmu_access(p, chunk);
    done = chunk;

    if (done == len)
        return;

    uint64_t rem = len - done;
    if (rem <= 4096) {
        p = gpa(prp2, rem);
        if (p) {
            disk_io(disk_off + done, p, rem, write);
            if (!write && g_patch_pmu) patch_pmu_access(p, rem);
        }
        return;
    }

    /* PRP2 is a PRP list (page of 64-bit entries). */
    unsigned idx = 0;
    while (rem > 0) {
        uint64_t *list = gpa(prp2, (uint64_t)(idx + 1) * 8);
        if (!list)
            return;
        uint64_t page = list[idx++];
        chunk = rem > 4096 ? 4096 : rem;
        p = gpa(page, chunk);
        if (!p || disk_io(disk_off + done, p, chunk, write) < 0)
            return;
        if (!write && g_patch_pmu) patch_pmu_access(p, chunk);
        done += chunk;
        rem -= chunk;
    }
}

static void identify_ctrl(void *buf)
{
    uint8_t *d = buf;
    memset(d, 0, 4096);
    d[0] = 0x36; d[1] = 0x1b;
    d[2] = 0xf4; d[3] = 0x1a;
    memset(d + 4, ' ', 20 + 40 + 8);           /* SN, MN, FR: space padded */
    memcpy(d + 4, "GMBOX000000000000001", 20);
    memcpy(d + 24, "GMBox NVMe Controller", 21);
    memcpy(d + 64, "1.0", 3);
    d[77] = 5;              /* MDTS: 128 KiB */
    d[78] = 1; d[79] = 0;   /* CNTLID */
    d[72] = 6;                                  /* RAB */
    d[73] = 0x00; d[74] = 0x02; d[75] = 0xb3;   /* IEEE OUI */
    d[80] = 0x00; d[81] = 0x03; d[82] = 0x01; d[83] = 0x00; /* VER 1.3.0 */
    d[258] = 3;                                 /* ACL */
    d[259] = 3;                                 /* AERL: 4 outstanding AERs */
    d[260] = 0x02;                              /* FRMW: one slot */
    d[261] = 0x03;                              /* LPA */
    d[266] = 0x57; d[267] = 0x01;               /* WCTEMP 343 K */
    d[268] = 0x75; d[269] = 0x01;               /* CCTEMP 373 K */
    d[512] = 0x66; d[513] = 0x00; /* SQES */
    d[514] = 0x44; d[515] = 0x00; /* CQES */
    d[516] = 1; d[517] = 0;       /* NN */
}

static void identify_ns(void *buf)
{
    uint8_t *d = buf;
    uint64_t blocks = disk_bytes >> lba_shift;
    memset(d, 0, 4096);
    memcpy(d + 0, &blocks, 8);  /* NSZE */
    memcpy(d + 8, &blocks, 8);  /* NCAP */
    memcpy(d + 16, &blocks, 8); /* NUSE */
    d[12] = 0;                  /* NLBAF: one format */
    d[26] = 0;                  /* FLBAS */
    uint32_t lbaf = (uint32_t)(lba_shift) << 16;
    memcpy(d + 128, &lbaf, 4);  /* LBAF[0] */
}

static void nvme_admin(uint8_t *sqe)
{
    uint32_t *w = (uint32_t *)sqe;
    uint8_t opc = sqe[0];
    { static unsigned al; if (getenv("GMBOX_NVME_LOG") && al++ < 200000) fprintf(stderr, "NVMEADM t=%llu opc=0x%02x nsid=%u cdw10=0x%x cdw11=0x%x\n", (unsigned long long)(mach_absolute_time() / 24000000), opc, ((uint32_t *)sqe)[1], ((uint32_t *)sqe)[10], ((uint32_t *)sqe)[11]); }
    uint16_t cid = (uint16_t)(w[0] >> 16);
    uint32_t nsid = w[1];
    uint64_t prp1 = ((uint64_t)w[7] << 32) | w[6];
    uint32_t cdw10 = w[10];

    switch (opc) {
    case 0x06: { /* Identify */
        void *buf = gpa(prp1, 4096);
        uint8_t tmp[4096];
        if (!buf)
            buf = tmp;
        uint32_t cns = cdw10 & 0xff;
        if (cns == 1)
            identify_ctrl(buf);
        else if (cns == 0 && (nsid == 1 || nsid == 0xffffffffu))
            identify_ns(buf);
        else if (cns == 2) {
            memset(buf, 0, 4096);
            *(uint32_t *)buf = 1;
        } else if (cns == 3 && nsid == 1) {
            memset(buf, 0, 4096);               /* no namespace descriptors */
        } else {
            nvme_complete(&nq[0], cid, NVME_SC_INVALID_FIELD, 0);
            break;
        }
        nvme_complete(&nq[0], cid, 0, 0);
        break;
    }
    case 0x09: { /* Set Features */
        uint32_t fid = cdw10 & 0xff;
        uint32_t result = 0;
        if (fid < 1 || fid > 0x0b) {
            nvme_complete(&nq[0], cid, NVME_SC_INVALID_FIELD, 0);
            break;
        }
        if (fid == 0x07) { /* Number of Queues */
            uint32_t nsqr = w[11] & 0xffff;
            uint32_t ncqr = w[11] >> 16;
            if (nsqr > NVME_QMAX - 1) nsqr = NVME_QMAX - 1;
            if (ncqr > NVME_QMAX - 1) ncqr = NVME_QMAX - 1;
            result = (ncqr << 16) | nsqr;
        }
        nvme_complete(&nq[0], cid, 0, result);
        break;
    }
    case 0x0a: { /* Get Features */
        uint32_t fid = cdw10 & 0xff;
        uint32_t result = 0;
        if (fid < 1 || fid > 0x0b) {
            nvme_complete(&nq[0], cid, NVME_SC_INVALID_FIELD, 0);
            break;
        }
        if (fid == 0x07)
            result = ((NVME_QMAX - 1) << 16) | (NVME_QMAX - 1);
        else if (fid == 0x04)
            result = 300;
        nvme_complete(&nq[0], cid, 0, result);
        break;
    }
    case 0x05: { /* Create I/O Completion Queue */
        uint32_t qid = cdw10 & 0xffff;
        uint32_t qsize = ((cdw10 >> 16) & 0xffff) + 1;
        if (qid == 0 || qid >= NVME_QMAX) {
            nvme_complete(&nq[0], cid, 1, 0);
            break;
        }
        nq[qid].cq_base = prp1;
        nq[qid].cqsize = qsize;
        nq[qid].cq_head = 0;
        nq[qid].cq_tail = 0;
        nq[qid].phase = 1;
        nq[qid].ien = (w[11] >> 1) & 1;
        nq[qid].live = 1;
        nvme_complete(&nq[0], cid, 0, 0);
        break;
    }
    case 0x01: { /* Create I/O Submission Queue */
        uint32_t qid = cdw10 & 0xffff;
        uint32_t qsize = ((cdw10 >> 16) & 0xffff) + 1;
        if (qid == 0 || qid >= NVME_QMAX) {
            nvme_complete(&nq[0], cid, 1, 0);
            break;
        }
        {
            uint32_t cqid = w[11] >> 16;
            if (cqid >= NVME_QMAX || !nq[cqid].cqsize) {
                nvme_complete(&nq[0], cid, NVME_SC_INVALID_FIELD, 0);
                break;
            }
            nq[qid].cqid = (uint16_t)cqid;
        }
        nq[qid].sq_base = prp1;
        nq[qid].sq_head = 0;
        nq[qid].sq_tail = 0;
        nq[qid].sqsize = qsize;
        nvme_complete(&nq[0], cid, 0, 0);
        break;
    }
    case 0x0c: /* Async Event Request: stays pending, never completed */
        break;
    case 0x08: /* Abort: command not aborted */
        nvme_complete(&nq[0], cid, 0, 1);
        break;
    case 0x02: { /* Get Log Page */
        uint32_t lid = cdw10 & 0xff;
        uint32_t numd = (((cdw10 >> 16) & 0xffff) + 1) * 4;
        if (lid < 1 || lid > 3) {
            nvme_complete(&nq[0], cid, NVME_SC_INVALID_LOG_PAGE, 0);
            break;
        }
        if (numd > 4096)
            numd = 4096;
        void *lbuf = gpa(prp1, numd);
        if (lbuf)
            memset(lbuf, 0, numd);
        nvme_complete(&nq[0], cid, 0, 0);
        break;
    }
    default:
        nvme_complete(&nq[0], cid, NVME_SC_INVALID_OPCODE, 0);
        break;
    }
}

static void nvme_io(struct nvme_q *q, uint8_t *sqe)
{
    uint32_t *w = (uint32_t *)sqe;
    uint8_t opc = sqe[0];
    uint16_t cid = (uint16_t)(w[0] >> 16);
    uint64_t slba = ((uint64_t)w[11] << 32) | w[10];
    uint32_t nlb = w[12] & 0xffff;
    uint64_t prp1 = ((uint64_t)w[7] << 32) | w[6];
    uint64_t prp2 = ((uint64_t)w[9] << 32) | w[8];
    uint64_t len = ((uint64_t)nlb + 1) << lba_shift;
    uint64_t off = slba << lba_shift;

    switch (opc) {
    case 0x01: /* Write */
        { static unsigned wl; if (getenv("GMBOX_NVME_LOG") && wl++ < 200000) fprintf(stderr, "NVMEIO t=%llu WRITE lba=%llu n=%u\n", (unsigned long long)(mach_absolute_time() / 24000000), (unsigned long long)slba, nlb + 1); }
        if (!gpa(prp1, 1)) {
            nvme_complete(q, cid, 2, 0);
            break;
        }
        nvme_prp_io(prp1, prp2, len, off, 1);
        nvme_complete(q, cid, 0, 0);
        break;
    case 0x02: /* Read */
        { static unsigned rl; if (getenv("GMBOX_NVME_LOG") && rl++ < 200000) fprintf(stderr, "NVMEIO t=%llu read lba=%llu n=%u\n", (unsigned long long)(mach_absolute_time() / 24000000), (unsigned long long)slba, nlb + 1); }
        if (!gpa(prp1, 1)) {
            nvme_complete(q, cid, 2, 0);
            break;
        }
        nvme_prp_io(prp1, prp2, len, off, 0);
        nvme_complete(q, cid, 0, 0);
        break;
    case 0x00: /* Flush */
    case 0x04: /* Write Uncorrectable */
    case 0x05: /* Compare */
    case 0x08: /* Write Zeroes */
    case 0x09: /* Dataset Management */
    default:
        nvme_complete(q, cid, 0, 0);
        break;
    }
}

static void nvme_process(uint32_t qid)
{
    struct nvme_q *q = &nq[qid];
    if (qid == 0) {
        q->sq_base = nvme_asq;
        q->cq_base = nvme_acq;
        q->sqsize = ((nvme_aqa & 0xfff) + 1);
        q->cqsize = (((nvme_aqa >> 16) & 0xfff) + 1);
        q->cqid = 0;
    }
    if (!q->sqsize || qid >= NVME_QMAX)
        return;

    static int logged;
    if (logged < 8) {
        fprintf(stderr, "NVMe kick qid=%u tail=%u size=%u\n",
                qid, q->sq_tail, q->sqsize);
        logged++;
    }

    unsigned guard = 0;
    while (q->sq_head != q->sq_tail && guard++ < q->sqsize * 2) {
        uint8_t *sqe = gpa(q->sq_base, (uint64_t)q->sqsize * 64);
        if (!sqe)
            return;
        sqe += (uint64_t)q->sq_head * 64;
        q->sq_head++;
        if (q->sq_head >= q->sqsize)
            q->sq_head = 0;
        if (qid == 0)
            nvme_admin(sqe);
        else
            nvme_io(q, sqe);
    }
}

static struct { uint64_t off, val; uint8_t wnr; unsigned n; } nv_ring[256];
static unsigned nv_ring_n;
static void nv_ring_add(uint64_t off, uint64_t val, int wnr)
{
    if (getenv("GMBOX_NVMMIO")) fprintf(stderr, "NVMMIO %s off=0x%llx val=0x%llx\n", wnr ? "W" : "R", (unsigned long long)off, (unsigned long long)val);
    unsigned i = nv_ring_n++ % 256;
    nv_ring[i].off = off; nv_ring[i].val = val; nv_ring[i].wnr = (uint8_t)wnr; nv_ring[i].n = nv_ring_n;
}
static void nv_ring_dump(void)
{
    for (unsigned k = 0; k < 256; k++) {
        unsigned i = (nv_ring_n + k) % 256;
        if (nv_ring[i].n)
            fprintf(stderr, "NVRING #%u %s off=0x%llx val=0x%llx\n", nv_ring[i].n, nv_ring[i].wnr ? "W" : "R", (unsigned long long)nv_ring[i].off, (unsigned long long)nv_ring[i].val);
    }
}

static uint64_t nvme_read_i(uint64_t off, int size);
static pthread_cond_t nvme_cv = PTHREAD_COND_INITIALIZER;
static int nvme_sync;
static unsigned g_nvme_delay_us = 30;

static uint64_t nvme_read(uint64_t off, int size)
{
    pthread_mutex_lock(&nvme_mu);
    uint64_t r = nvme_read_i(off, size);
    nv_ring_add(off, r, 0);
    pthread_mutex_unlock(&nvme_mu);
    return r;
}

static uint64_t nvme_read_i(uint64_t off, int size)
{
    uint64_t v = 0;
    switch (off) {
    case 0x0000: v = 0x00000020000f003fULL; break; /* CAP lo: CSS=NVM */
    case 0x0004: v = 0x00000020ULL; break;         /* CAP hi: CSS bit 0 */
    case 0x0008: v = 0x00010300ULL; break;         /* VS */
    case 0x000c:
    case 0x0010: v = nvme_intms; break;
    case 0x0014: v = nvme_cc; break;
    case 0x001c: v = nvme_csts; break;
    case 0x0024: v = nvme_aqa; break;
    case 0x0028: v = nvme_asq; break;
    case 0x0030: v = nvme_acq; break;
    default:
        if (off >= 0x1000 && off < 0x1000 + NVME_QMAX * 8) {
            uint32_t qid = (uint32_t)(off - 0x1000) / 8;
            uint32_t reg = (uint32_t)(off - 0x1000) % 8;
            if (qid < NVME_QMAX)
                v = reg == 0 ? nq[qid].sq_tail : nq[qid].cq_head;
        }
        break;
    }
    if (size == 4)
        v &= 0xffffffffULL;
    return v;
}

static void nvme_write_i(uint64_t off, uint64_t value, int size)
{
    nv_ring_add(off, value, 1);
    if (size == 4)
        value &= 0xffffffffULL;
    switch (off) {
    case 0x0014: /* CC */
        if (getenv("GMBOX_NVME_LOG")) fprintf(stderr, "NVMECC write 0x%llx\n", (unsigned long long)value);
        nvme_cc = value;
        if (value & 1) {
            if (!nvme_enabled) {
                if (g_admin_db_at && ++g_cc_en_n == g_admin_db_at)
                    g_st_wait_db = 1;
                nvme_reset();
                nvme_enabled = 1;
                nvme_csts = 1;
            }
        } else {
            nvme_enabled = 0;
            nvme_csts = 0;
        }
        break;
    case 0x000c: nvme_intms |= (uint32_t)value; nvme_update_irq(); break;
    case 0x0010: nvme_intms &= ~(uint32_t)value; nvme_update_irq(); break;
    case 0x0024: nvme_aqa = value; break;
    case 0x0028: nvme_asq = value; break;
    case 0x0030: nvme_acq = value; break;
    default:
        if (off >= 0x1000 && off < 0x1000 + NVME_QMAX * 8) {
            uint32_t qid = (uint32_t)(off - 0x1000) / 8;
            uint32_t reg = (uint32_t)(off - 0x1000) % 8;
            if (qid < NVME_QMAX) {
                if (reg == 0) {
                    nq[qid].sq_tail = (uint32_t)value;
                    if (nvme_sync)
                        nvme_process(qid);
                    else
                        pthread_cond_signal(&nvme_cv);
                } else {
                    nq[qid].cq_head = (uint32_t)value;
                    nvme_update_irq();
                }
            }
        }
        break;
    }
}

static void nvme_write(uint64_t off, uint64_t value, int size)
{
    pthread_mutex_lock(&nvme_mu);
    nvme_write_i(off, value, size);
    pthread_mutex_unlock(&nvme_mu);
}

/* Completes queued commands shortly after the doorbell write, like a real
 * controller: the guest driver finishes its submission bookkeeping before
 * the completion/interrupt arrives. */
static void *nvme_worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&nvme_mu);
    for (;;) {
        int work = 0;
        for (uint32_t i = 0; i < NVME_QMAX; i++)
            if (nq[i].sq_head != nq[i].sq_tail)
                work = 1;
        if (!work) {
            pthread_cond_wait(&nvme_cv, &nvme_mu);
            continue;
        }
        pthread_mutex_unlock(&nvme_mu);
        usleep(g_nvme_delay_us);
        pthread_mutex_lock(&nvme_mu);
        for (uint32_t i = 0; i < NVME_QMAX; i++)
            if (nq[i].sq_head != nq[i].sq_tail)
                nvme_process(i);
    }
    return NULL;
}

#include "hvf_usb.inc"

/* ------------------------------------------------------------------ */
/* MMIO / exceptions                                                   */

static uint32_t bswap32(uint32_t v)
{
    return __builtin_bswap32(v);
}

static uint64_t bswap64(uint64_t v)
{
    return __builtin_bswap64(v);
}

static size_t load_blob(const char *path, void *dst, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(dst, 1, max, f);
    fclose(f);
    return n;
}

static void fwcfg_build(void)
{
    memset(fw_data, 0, sizeof(fw_data));
    memset(fw_sizes, 0, sizeof(fw_sizes));
    memset(fw_dir, 0, sizeof(fw_dir));

    /* etc/hardware-info: QEMU PCIHostBridgeInfo (little-endian) */
    {
        pci_host_bridge_info_t hbi;
        memset(&hbi, 0, sizeof(hbi));
        hbi.version = 1;
        hbi.pci_express_base_address = ECAM_IPA;
        hbi.pci_express_segment = 0;
        hbi.pci_io_base = 0;
        hbi.pci_io_limit = 0;
        hbi.pci_mem32_base = MMIO32_IPA;
        hbi.pci_mem32_limit = MMIO32_IPA + MMIO32_SIZE;
        hbi.pci_mem64_base = 0;
        hbi.pci_mem64_limit = 0;
        fw_sizes[0] = sizeof(hbi);
        memcpy(fw_data[0], &hbi, sizeof(hbi));
        fprintf(stderr, "hw-info: %u bytes at sel 0x20\n", fw_sizes[0]);
    }

    fw_sizes[1] = (uint32_t)load_blob("/Users/gaetano/Documents/Projects/GMBox/tools/acpi_tables.bin", fw_data[1], sizeof(fw_data[1]));
    fw_sizes[2] = (uint32_t)load_blob("/Users/gaetano/Documents/Projects/GMBox/tools/acpi_rsdp.bin", fw_data[2], sizeof(fw_data[2]));
    fw_sizes[3] = (uint32_t)load_blob("/Users/gaetano/Documents/Projects/GMBox/tools/acpi_loader.bin", fw_data[3], sizeof(fw_data[3]));
    fw_sizes[4] = 28; /* etc/ramfb: RAMFBCfg, written by the guest */

    fw_dir[3] = FW_FILES;
    for (uint32_t i = 0; i < FW_FILES; i++) {
        uint32_t o = 4 + i * 64;
        uint32_t sz = fw_sizes[i];
        fw_dir[o + 0] = (uint8_t)(sz >> 24);
        fw_dir[o + 1] = (uint8_t)(sz >> 16);
        fw_dir[o + 2] = (uint8_t)(sz >> 8);
        fw_dir[o + 3] = (uint8_t)sz;
        fw_dir[o + 4] = 0;
        fw_dir[o + 5] = (uint8_t)(0x20 + i);
        strncpy((char *)fw_dir + o + 8, fw_names[i], 55);
    }
    fw_dir_len = 4 + FW_FILES * 64;
    fprintf(stderr, "fw files: %u %u %u %u\n",
            fw_sizes[0], fw_sizes[1], fw_sizes[2], fw_sizes[3]);

    cfi_init_query();
}

static uint8_t fwcfg_byte(uint16_t sel, uint32_t pos)
{
    static const uint8_t sig[4] = { 0x51, 0x45, 0x4d, 0x55 };
    static const uint8_t id[4] = { 0x03, 0x00, 0x00, 0x00 }; /* v2 + DMA */
    if (sel == 0x00 && pos < 4)
        return sig[pos];
    if (sel == 0x01 && pos < 4)
        return id[pos];
    if (sel == 0x19 && pos < fw_dir_len)
        return fw_dir[pos];
    for (uint32_t i = 0; i < FW_FILES; i++)
        if (sel == 0x20 + i && pos < fw_sizes[i])
            return fw_data[i][pos];
    return 0;
}

static void *gpa_any(uint64_t a, uint64_t len)
{
    if (a >= RAM_IPA && a + len >= a && a + len <= RAM_IPA + RAM_SIZE)
        return (uint8_t *)g_ram + (a - RAM_IPA);
    if (a >= FW_IPA && a + len <= FW_IPA + FW_SIZE)
        return (uint8_t *)g_fw + (a - FW_IPA);
    if (a >= VARS_IPA && a + len <= VARS_IPA + VARS_SIZE)
        return (uint8_t *)g_vars + (a - VARS_IPA);
    return NULL;
}

#include "hvf_fb.inc"

static void fwcfg_dma(uint64_t regval)
{
    /*
     * EDK2 QemuFwCfgLibMMIO (OvmfPkg): the guest does
     *   MmioWrite64 (fwcfg + 0x10, SwapBytes64 ((UINT64)&Access));
     * so the descriptor GPA is bswap64(regval).  The in-RAM descriptor is
     *   Control(BE32) Length(BE32) Address(BE64)
     * stored byte-swapped; native reads must be byteswapped back.
     * Completion contract: Control must read back as 0 (error bit clear).
     */
    uint64_t desc = bswap64(regval);
    volatile uint8_t *p = gpa_any(desc, 16);
    if (!p) {
        fprintf(stderr, "fwcfg_dma: descriptor @0x%llx unmapped\n",
                (unsigned long long)desc);
        return;
    }
    uint32_t ctl, len;
    uint64_t addr;
    memcpy(&ctl, (const void *)p, 4);
    memcpy(&len, (const void *)(p + 4), 4);
    memcpy(&addr, (const void *)(p + 8), 8);
    ctl = bswap32(ctl);
    len = bswap32(len);
    addr = bswap64(addr);
    fprintf(stderr, "fwcfg_dma ctl=0x%x len=%u addr=0x%llx sel=0x%x\n",
            ctl, len, (unsigned long long)addr, (ctl >> 16) & 0xffff);
    if (ctl & 0x8) { /* SELECT: selector in the high 16 bits */
        fw_sel = (uint16_t)(ctl >> 16);
        fw_pos = 0;
    }
    if (ctl & 2) { /* READ: host -> guest */
        uint8_t *dst = gpa_any(addr, len);
        if (!dst) {
            fprintf(stderr, "fwcfg_dma: dest @0x%llx len=%u unmapped\n",
                    (unsigned long long)addr, len);
        } else {
            uint32_t i;
            for (i = 0; i < len; i++)
                dst[i] = fwcfg_byte(fw_sel, fw_pos + i);
            fw_pos += len;
        }
    }
    if (ctl & 0x10) { /* WRITE: guest -> host */
        const uint8_t *src = gpa_any(addr, len);
        if (src && fw_sel == 0x20 + 4 && len <= sizeof(fw_data[4])) {
            memcpy(fw_data[4] + fw_pos, src, len < 28 ? len : 28);
            fb_config(fw_data[4]);
        }
        fw_pos += len;
    }
    if (ctl & 4) /* SKIP */
        fw_pos += len;
    uint32_t done = 0;
    memcpy((void *)p, &done, 4);
}

static uint64_t fwcfg_read(uint64_t off, int size)
{
    static int rlog = 999;
    if (rlog < 30) { fprintf(stderr, "FWCFG read off=0x%llx size=%d sel=0x%x\n", (unsigned long long)off, size, fw_sel); rlog++; }
    (void)size;
    if (off == 0x00) {
        uint64_t v = 0;
        for (int i = 0; i < size; i++)
            v |= (uint64_t)fwcfg_byte(fw_sel, fw_pos++) << (8 * i);
        return v;
    }
    if (off == 0x08)
        return fw_sel;
    return 0;
}

static void fwcfg_write(uint64_t off, uint64_t value, int size)
{
    static int wlog = 999;
    if (wlog < 30) { fprintf(stderr, "FWCFG write off=0x%llx value=0x%llx size=%d\n", (unsigned long long)off, (unsigned long long)value, size); wlog++; }
    (void)size;
    if (off == 0x08) {
        /* Selector register is written big-endian by EDK2 (FwCfgWrite16). */
        fw_sel = bswap16((uint16_t)value);
        fw_pos = 0;
    }
    else if (off == 0x10)
        fwcfg_dma(value);
}

#include "virtio_mmio.inc"
#include "pflash_cfi.inc"

static uint64_t read_reg(hv_vcpu_t vcpu, uint32_t rt)
{
    uint64_t v = 0;
    if (rt < 31)
        hv_vcpu_get_reg(vcpu, (hv_reg_t)(HV_REG_X0 + rt), &v);
    return v;
}

static void write_reg(hv_vcpu_t vcpu, uint32_t rt, uint64_t v)
{
    if (rt < 31)
        hv_vcpu_set_reg(vcpu, (hv_reg_t)(HV_REG_X0 + rt), v);
}

#include "hvf_trace.inc"
static int uart_rx_have;
static unsigned char uart_rx_byte;
volatile int g_uart_inject;

static void uart_poll(void)
{
    if (g_uart_inject && !uart_rx_have) {
        uart_rx_byte = (unsigned char)g_uart_inject;
        g_uart_inject = 0;
        uart_rx_have = 1;
        return;
    }
    if (!uart_rx_have) {
        int n = read(STDIN_FILENO, &uart_rx_byte, 1);
        if (n == 1) uart_rx_have = 1;
    }
}

static uint32_t uart_read(uint64_t off)
{
    uart_poll();
    switch (off) {
    case 0x00: {
        uint32_t c = uart_rx_have ? uart_rx_byte : 0;
        uart_rx_have = 0;
        return c;
    }
    case 0x18: return uart_rx_have ? 0x80 : 0x90;
    case 0x2c: return 0x3;
    case 0x30: return 0x301;
    default: return 0;
    }
}

static void uart_write(uint64_t off, uint64_t value)
{
    if (off == 0x00) {
        tr_uart_char((char)(value & 0xff));
        putchar((int)(value & 0xff));
    }
}


/* ---- PMU emulation: HVF traps PMU sysregs (EC=0x18) when PMUVer != 0 ---- */
#include <mach/mach_time.h>
static uint64_t pmu_pmcr = (0x41ULL << 24) | 0x0, pmu_cntenset, pmu_userenr,
                pmu_intenset, pmu_ovs, pmu_ccfilt, pmu_sel, pmu_start;
static unsigned pmu_traps;

static uint64_t pmu_cycles(void)
{
    if (!pmu_start)
        pmu_start = mach_absolute_time();
    return (mach_absolute_time() - pmu_start) * 100; /* ~2.4 GHz from 24 MHz */
}

/* returns 1 if handled */
static int sysreg_trap(hv_vcpu_t vcpu, uint64_t esr)
{
    uint32_t iss = (uint32_t)(esr & 0x1ffffffu);
    uint32_t op0 = (iss >> 20) & 3, op2 = (iss >> 17) & 7, op1 = (iss >> 14) & 7;
    uint32_t crn = (iss >> 10) & 15, rt = (iss >> 5) & 31, crm = (iss >> 1) & 15;
    int rd = iss & 1; /* 1 = MRS (read) */
    uint64_t pc = 0, v = 0;
    hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
    int pmu = (op0 == 3 && crn == 9 && (crm >= 12 && crm <= 14)) ||
              (op0 == 3 && crn == 14 && crm >= 8);
    if (op0 == 2) { /* external debug: OS lock etc., RAZ/WI except OSLSR_EL1 */
        static unsigned dlog;
        if (dlog++ < 20)
            fprintf(stderr, "DBG trap pc=0x%llx %s op1=%u CRn=%u CRm=%u op2=%u rt=%u\n",
                    (unsigned long long)pc, rd ? "mrs" : "msr", op1, crn, crm, op2, rt);
        if (rd)
            write_reg(vcpu, rt, (op1 == 0 && crn == 1 && crm == 1 && op2 == 4) ? 0x8 : 0);
        hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
        return 1;
    }
    if (!pmu)
        return 0;
    if (!rd)
        v = read_reg(vcpu, rt);
    uint64_t r = 0;
    uint32_t key = (op1 << 12) | (crm << 8) | (op2 << 4);
    if (pmu_traps < 40)
        fprintf(stderr, "PMU trap pc=0x%llx %s op1=%u CRm=%u op2=%u rt=%u v=0x%llx\n",
                (unsigned long long)pc, rd ? "mrs" : "msr", op1, crm, op2, rt,
                (unsigned long long)v);
    pmu_traps++;
    if (op1 == 3 && crn == 9 && crm == 12) {
        switch (op2) {
        case 0: if (rd) r = pmu_pmcr; else pmu_pmcr = (pmu_pmcr & ~0x7fULL) | (v & 0x39); break;
        case 1: if (rd) r = pmu_cntenset; else pmu_cntenset |= v; break;
        case 2: if (rd) r = pmu_cntenset; else pmu_cntenset &= ~v; break;
        case 3: if (rd) r = pmu_ovs; else pmu_ovs &= ~v; break;
        case 5: if (rd) r = pmu_sel; else pmu_sel = v; break;
        default: break; /* SWINC, CEID: RAZ/WI */
        }
    } else if (op1 == 3 && crn == 9 && crm == 13 && op2 == 0) {
        if (rd) r = pmu_cycles();
    } else if (op1 == 3 && crn == 9 && crm == 14 && op2 == 0) {
        if (rd) r = pmu_userenr; else pmu_userenr = v & 0xf;
    } else if (op1 == 0 && crn == 9 && crm == 14) {
        if (op2 == 1) { if (rd) r = pmu_intenset; else pmu_intenset |= v; }
        else if (op2 == 2) { if (rd) r = pmu_intenset; else pmu_intenset &= ~v; }
    } else if (op1 == 3 && crn == 9 && crm == 14 && op2 == 3) {
        if (rd) r = pmu_ovs;
    } else if (op1 == 3 && crn == 14 && crm == 15 && op2 == 7) {
        if (rd) r = pmu_ccfilt; else pmu_ccfilt = v;
    }
    (void)key;
    if (rd)
        write_reg(vcpu, rt, r);
    hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
    return 1;
}

static void dump_abort(hv_vcpu_t vcpu, hv_vcpu_exit_t *e)
{
    uint64_t pc = 0, far = 0, sp = 0;
    hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_FAR_EL1, &far);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL1, &sp);
    fprintf(stderr, "FATAL ESR=0x%llx FAR=0x%llx PC=0x%llx SP=0x%llx\n",
            (unsigned long long)e->exception.syndrome,
            (unsigned long long)far, (unsigned long long)pc,
            (unsigned long long)sp);
    for (int i = 0; i < 31; i += 2) {
        uint64_t a = 0, b = 0;
        hv_vcpu_get_reg(vcpu, (hv_reg_t)(HV_REG_X0 + i), &a);
        hv_vcpu_get_reg(vcpu, (hv_reg_t)(HV_REG_X0 + i + 1), &b);
        fprintf(stderr, "  X%d=0x%016llx X%d=0x%016llx\n", i,
                (unsigned long long)a, i + 1, (unsigned long long)b);
    }
    void *p = gpa(pc, 32);
    if (p) {
        fprintf(stderr, "  bytes@PC:");
        for (int i = 0; i < 32; i++)
            fprintf(stderr, " %02x", ((uint8_t *)p)[i]);
        fprintf(stderr, "\n");
    }
}

static void mmio(hv_vcpu_t vcpu, hv_vcpu_exit_t *e)
{
    uint64_t esr = e->exception.syndrome;
    uint64_t iss = esr & 0x1ffffffULL;
    uint64_t pa = e->exception.physical_address;
    int wnr = (int)((iss >> 6) & 1);
    int isv = (int)((iss >> 24) & 1);
    int srt = (int)((iss >> 16) & 0x1f);
    int size = 1 << ((iss >> 22) & 3);
    uint64_t pc = 0;
    hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);

    if (pa >= UART_IPA && pa < UART_IPA + 0x1000) {
        if (wnr) {
            if (isv)
                uart_write(pa - UART_IPA, read_reg(vcpu, (uint32_t)srt));
        } else if (isv) {
            write_reg(vcpu, (uint32_t)srt, uart_read(pa - UART_IPA));
        }
        hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
        return;
    }

    if (pa >= RTC_IPA && pa < RTC_IPA + 0x1000) {
        if (!wnr && isv)
            write_reg(vcpu, (uint32_t)srt,
                      (pa - RTC_IPA) == 0 ? (uint64_t)time(NULL) : 0);
        hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
        return;
    }

    if (pa >= VARS_IPA && pa < VARS_IPA + VARS_SIZE) {
        static int vselog; if (vselog < 20) { fprintf(stderr, "VARS access off=0x%llx wnr=%d size=%d\n", (unsigned long long)(pa - VARS_IPA), wnr, size); vselog++; }
        uint64_t voff = pa - VARS_IPA;
        if (wnr && isv)
            pflash_write(voff, read_reg(vcpu, (uint32_t)srt), size);
        else if (!wnr && isv)
            write_reg(vcpu, (uint32_t)srt, pflash_read(voff, size));
        hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
        return;
    }

    if (pa >= FWCFG_IPA && pa < FWCFG_IPA + FWCFG_SIZE) {
        uint64_t foff = pa - FWCFG_IPA;
        if (wnr && isv)
            fwcfg_write(foff, read_reg(vcpu, (uint32_t)srt), size);
        else if (!wnr && isv)
            write_reg(vcpu, (uint32_t)srt, fwcfg_read(foff, size));
        hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
        return;
    }
    if (pa >= GICR_IPA && pa < GICR_IPA + 0x20000) {
        uint64_t goff = pa - GICR_IPA;
        if (goff == 0x14) {
            if (!wnr && isv)
                write_reg(vcpu, (uint32_t)srt, 0);
        } else if (wnr) {
            hv_gic_set_redistributor_reg(vcpu,
                (hv_gic_redistributor_reg_t)goff, read_reg(vcpu, (uint32_t)srt));
        } else if (isv) {
            uint64_t v = 0;
            hv_gic_get_redistributor_reg(vcpu,
                (hv_gic_redistributor_reg_t)goff, &v);
            write_reg(vcpu, (uint32_t)srt, v);
        }
        hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
        return;
    }

    if (pa >= ECAM_IPA && pa < ECAM_IPA + ECAM_SIZE) {
        unsigned o = (unsigned)(pa - ECAM_IPA);
        ecam_hits++;
        if (ecam_hits <= 40)
            fprintf(stderr, "ECAM off=0x%x wnr=%d isv=%d rt=%d size=%d\n",
                    o, wnr, isv, srt, size);
        if ((o >> 20) != 0) {
            if (!wnr && isv)
                write_reg(vcpu, (uint32_t)srt, 0xffffffffULL);
        } else {
            /* Only bus 0 dev 0 fn 0 (host bridge) and dev 1 fn 0 (NVMe) exist.
             * Masking the address with 0xffff used to alias devices 2..31 onto
             * these two, so Windows enumerated sixteen phantom NVMe controllers. */
            unsigned dev = (o >> 15) & 0x1f, fn = (o >> 12) & 7;
            unsigned idx = o & 0xfff;
            int present = (dev == 0 || dev == 1 || (dev == 2 && g_usb_enabled)) && fn == 0;
            idx |= dev << 15;
            if (wnr && isv) {
                if (present)
                    cfg_write(idx, size, read_reg(vcpu, (uint32_t)srt));
            } else if (!wnr && isv) {
                write_reg(vcpu, (uint32_t)srt,
                          present ? cfg_read(idx, size)
                                  : (size == 8 ? ~0ULL : (size == 4 ? 0xffffffffULL : (size == 2 ? 0xffffULL : 0xffULL))));
            }
        }
        hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
        return;
    }

    if (g_usb_enabled && usb_bar && pa >= usb_bar && pa < usb_bar + USB_SIZE) {
        uint64_t uoff = pa - usb_bar;
        if (wnr) {
            if (isv) usb_write(uoff, read_reg(vcpu, (uint32_t)srt), size);
        } else if (isv) {
            write_reg(vcpu, (uint32_t)srt, usb_read(uoff, size));
        }
        hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
        return;
    }
    if (nvme_bar && pa >= nvme_bar && pa < nvme_bar + NVME_SIZE) {
        uint64_t off = pa - nvme_bar;
        nvme_hits++;
        if (nvme_hits <= 20)
            fprintf(stderr, "NVMe mmio off=0x%llx wnr=%d size=%d\n",
                    (unsigned long long)off, wnr, size);
        if (wnr && off == 0x1000 && g_st_wait_db) {
            g_st_wait_db = 0;
            g_st_arm_now = 1;
        }
        if (wnr) {
            if (isv)
                nvme_write(off, read_reg(vcpu, (uint32_t)srt), size);
        } else if (isv) {
            write_reg(vcpu, (uint32_t)srt, nvme_read(off, size));
        }
        hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
        return;
    }

    if (pa >= VIRTIO_MMIO_IPA && pa < VIRTIO_MMIO_IPA + VIRTIO_MMIO_SIZE) {
        uint64_t off = pa - VIRTIO_MMIO_IPA;
        static int vlog;
        if (vlog < 40) {
            fprintf(stderr, "virtio-mmio off=0x%llx wnr=%d size=%d\n",
                    (unsigned long long)off, wnr, size);
            vlog++;
        }
        if (wnr) {
            if (isv)
                virtio_write(off, read_reg(vcpu, (uint32_t)srt), size);
        } else if (isv) {
            write_reg(vcpu, (uint32_t)srt, virtio_read(off, size));
        }
        hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
        return;
    }

    /* ------------------------------------------------------------------ */
    /* CFI NOR flash: FW region writes (read-only mapping -> abort here).  */
    /* EDK2 probes FW as first flash device; commands are ignored so it   */
    /* falls through to the VARS device.                                  */
    if (pa >= FW_IPA && pa < FW_IPA + FW_SIZE && wnr && isv) {
        static int fwlog; if (fwlog < 20) { fprintf(stderr, "FW write off=0x%llx size=%d\n", (unsigned long long)pa, size); fwlog++; }
        hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
        return;
    }

    static int unknown;
    if (unknown < 30) {
        fprintf(stderr, "MMIO unhandled pa=0x%llx wnr=%d isv=%d rt=%d size=%d ec=0x%llx\n",
                (unsigned long long)pa, wnr, isv, srt, size,
                (unsigned long long)((esr >> 26) & 0x3f));
        unknown++;
    }
    if (!wnr && isv)
        write_reg(vcpu, (uint32_t)srt, 0);
    hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
}

static char **g_argv;
static void vm_reboot(void)
{
    const char *vp = getenv("GMBOX_VARS") ? getenv("GMBOX_VARS") : VARS_PATH;
    if (!getenv("GMBOX_VARS_RO")) {
        FILE *vf = fopen(vp, "r+b");
        if (vf) {
            fwrite(g_vars, 1, VARS_SIZE, vf);
            fclose(vf);
        }
    }
    if (disk_fd >= 0)
        fsync(disk_fd);
    if (g_argv)
        execv(g_argv[0], g_argv);
    perror("execv");
}

static int psci(hv_vcpu_t vcpu)
{
    uint64_t x0 = 0, pc = 0;
    hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0);
    hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);

    uint64_t ret = 0xffffffffffffffffULL;
    uint64_t r1 = 0, r2 = 0, r3 = 0;

    if (x0 == 0x84000000ULL || x0 == 0x80000000ULL)
        ret = 0x00010001ULL;                 /* PSCI/SMCCC version 1.1 */
    else if (x0 == 0x84000050ULL)
        ret = 0x00010000ULL;                 /* TRNG version 1.0 */
    else if (x0 == 0x84000051ULL)
        ret = 0;                             /* TRNG feature supported */
    else if (x0 == 0x84000052ULL) {          /* TRNG UUID */
        ret = 0x12345678ULL;
        r1 = 0x9abcdef0ULL; r2 = 0x0fedcba9ULL; r3 = 0x87654321ULL;
    } else if (x0 == 0x84000053ULL || x0 == 0xc4000053ULL) {
        uint64_t t = (uint64_t)time(NULL);
        ret = 0;
        r1 = t * 6364136223846793005ULL + 1;
        r2 = r1 * 6364136223846793005ULL + 2;
        r3 = r2 * 6364136223846793005ULL + 3;
    } else if (x0 == 0x84000009ULL && !getenv("GMBOX_NO_REBOOT")) {
        /* SYSTEM_RESET: persist the NVRAM image and restart the whole VM in a
         * fresh process (the disk keeps its contents). */
        fprintf(stderr, "PSCI system reset: rebooting VM\n");
        vm_reboot();
        return 1;
    } else if (x0 == 0x84000008ULL || x0 == 0x84000009ULL) {
        fprintf(stderr, "PSCI system %s (x0=0x%llx)\n", x0 == 0x84000008ULL ? "off" : "reset", (unsigned long long)x0);
        if (x0 == 0x84000008ULL && getenv("GMBOX_REBOOT_ON_OFF")) { vm_reboot(); }
        return 1;
    }

    hv_vcpu_set_reg(vcpu, HV_REG_X0, ret);
    if (x0 == 0x84000052ULL || x0 == 0x84000053ULL || x0 == 0xc4000053ULL) {
        hv_vcpu_set_reg(vcpu, HV_REG_X1, r1);
        hv_vcpu_set_reg(vcpu, HV_REG_X2, r2);
        hv_vcpu_set_reg(vcpu, HV_REG_X3, r3);
    }

    /* HVF reports PC after HVC/SMC on current macOS; older versions report
     * the faulting instruction.  Detect which by looking at the code. */
    int advance = 1;
    if (insn_is_hvc_smc(guest_read32(pc)))
        advance = 1;
    else if (pc >= 4 && insn_is_hvc_smc(guest_read32(pc - 4)))
        advance = 0;
    hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + (advance ? 4 : 0));
    return 0;
}

static void scan_loaded(const char *needle)
{
    size_t n = strlen(needle);
    void *hit = memmem(g_ram, RAM_SIZE, needle, n);
    if (hit)
        fprintf(stderr, "loaded: %s at RAM+0x%llx\n", needle,
                (unsigned long long)((uint8_t *)hit - (uint8_t *)g_ram));
    else
        fprintf(stderr, "not loaded: %s\n", needle);
}

static void dump_ram_snapshot(void)
{
    static const uint8_t magic[4] = { 0xd0, 0x0d, 0xfe, 0xed };
    uint32_t at_dtb = 0;
    memcpy(&at_dtb, g_ram, 4);
    fprintf(stderr, "dtb@0x40000000 word=0x%08x (be-magic=0xd00dfeed)\n",
            (unsigned)bswap32(at_dtb));
    const uint8_t *base = g_ram;
    uint64_t ranges[2][2] = { { 0, 512UL * 1024 * 1024 },
                              { RAM_SIZE - 128UL * 1024 * 1024, RAM_SIZE } };
    for (int r = 0; r < 2; r++)
        for (uint64_t off = ranges[r][0]; off + 4 <= ranges[r][1]; off += 4)
            if (memcmp(base + off, magic, 4) == 0)
                fprintf(stderr, "fdt magic at RAM+0x%llx (GPA 0x%llx)\n",
                        (unsigned long long)off,
                        (unsigned long long)(RAM_IPA + off));
    uint64_t pc = 0;
    hv_vcpu_get_reg(g_vcpu, HV_REG_PC, &pc);
    fprintf(stderr, "watchdog pc=0x%llx\n", (unsigned long long)pc);
    if (pc >= FW_IPA && pc < FW_IPA + FW_SIZE) {
        const uint8_t *p = (const uint8_t *)g_fw + (pc - FW_IPA);
        fprintf(stderr, "code@pc:");
        for (int i = 0; i < 16; i++)
            fprintf(stderr, " %02x", p[i]);
        fprintf(stderr, "\n");
    }
}

#include "hvf_steptrace.inc"
#include "hvf_kdbrk.inc"

static void *watchdog(void *arg)
{
    long sec = (long)arg;
    uint32_t magic = 0;
    if (sec <= 0)
        return NULL;
    sleep((unsigned)sec);
    g_watchdog_fired = 1;
    memcpy(&magic, g_ram, 4);
    fprintf(stderr, "watchdog: ecam=%u nvme=%u dtb_magic=0x%x bar=0x%llx\n",
            ecam_hits, nvme_hits, magic, (unsigned long long)nvme_bar);
    { uint64_t e1=0,l1=0,f1=0,d0=0; hv_vcpu_get_sys_reg(g_vcpu,HV_SYS_REG_ESR_EL1,&e1); hv_vcpu_get_sys_reg(g_vcpu,HV_SYS_REG_ELR_EL1,&l1); hv_vcpu_get_sys_reg(g_vcpu,HV_SYS_REG_FAR_EL1,&f1); hv_vcpu_get_sys_reg(g_vcpu,HV_SYS_REG_ID_AA64DFR0_EL1,&d0); fprintf(stderr,"guest ESR_EL1=0x%llx ELR_EL1=0x%llx FAR=0x%llx DFR0=0x%llx\n",(unsigned long long)e1,(unsigned long long)l1,(unsigned long long)f1,(unsigned long long)d0);}
    { const uint32_t *w = (const uint32_t *)((uint8_t *)g_ram + 0x7c46ddd4 - 16); for (int i=0;i<10;i++) fprintf(stderr,"insn[%+d]=%08x\n",(i-4),w[i]); }
    scan_loaded("pci-host-ecam-generic");
    dump_ram_snapshot();
    scan_loaded("PciBusDxe");
    scan_loaded("PciHostBridgeDxe");
    scan_loaded("NvmExpressDxe");
    scan_loaded("bootmgfw");
    hv_vcpus_exit(&g_vcpu, 1);
    return NULL;
}

static int detect_lba_shift(void)
{
    uint8_t s[4096];
    if (disk_fd < 0)
        return 9;
    ssize_t n = pread(disk_fd, s, sizeof(s), 0);
    if (n >= 512 && s[510] == 0x55 && s[511] == 0xaa)
        return 9;
    if (n >= 4096 && s[4094] == 0x55 && s[4095] == 0xaa)
        return 12;
    return 9;
}

int main(int argc, char **argv)
{
    g_argv = argv;
    const char *dtb_path = argc > 1 ? argv[1] : DTB_PATH;
    long watchdog_sec = 0;
    const char *wd = getenv("GMBOX_WATCHDOG_SEC");
    if (wd && *wd)
        watchdog_sec = strtol(wd, NULL, 0);

    g_kdbrk = getenv("GMBOX_KDBRK") != NULL;
    g_trace_on = getenv("GMBOX_TRACE_BM") != NULL;
    g_patch_pmu = getenv("GMBOX_PATCH_PMU") != NULL;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL, 0) | O_NONBLOCK);

    hv_vm_config_t cfg = hv_vm_config_create();
    if (!cfg) {
        fprintf(stderr, "hv_vm_config_create failed\n");
        return 1;
    }
    fail(hv_vm_create(cfg), "hv_vm_create");

    hv_gic_config_t gic_cfg = hv_gic_config_create();
    if (!gic_cfg) {
        fprintf(stderr, "hv_gic_config_create failed\n");
        return 1;
    }
    fail(hv_gic_config_set_distributor_base(gic_cfg, GICD_IPA), "set gicd");
    fail(hv_gic_config_set_redistributor_base(gic_cfg, GICR_IPA), "set gicr");
    fail(hv_gic_create(gic_cfg), "hv_gic_create");

    g_fw = map_anon(FW_SIZE);
    g_vars = map_anon(VARS_SIZE);
    g_ram = map_anon(RAM_SIZE);
    /* FW: read+execute only. EDK2 runs from here; writes (CFI commands)
     * are intercepted via data abort. VARS: read+write for NVRAM store. */
    fail(hv_vm_map(g_fw, FW_IPA, FW_SIZE,
                   HV_MEMORY_READ | HV_MEMORY_EXEC),
         "map firmware");
    fail(hv_vm_map(g_vars, VARS_IPA, VARS_SIZE,
                   HV_MEMORY_READ),
         "map vars");
    fail(hv_vm_map(g_ram, RAM_IPA, RAM_SIZE,
                   HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
         "map ram");

    load_file(getenv("GMBOX_FW") ? getenv("GMBOX_FW") : FW_PATH, g_fw, FW_SIZE);
    load_file(getenv("GMBOX_VARS") ? getenv("GMBOX_VARS") : VARS_PATH, g_vars, VARS_SIZE);
    load_file(dtb_path, g_ram, 1024 * 1024);
    load_file(dtb_path, (uint8_t *)g_ram + 0x10000, 1024 * 1024);

    /* Completing on the vCPU thread is the default: hv_gic_set_spi from a helper thread
     * can lose the interrupt while the vCPU is entering the guest (Windows then stalls). */
    nvme_sync = getenv("GMBOX_NVME_ASYNC") == NULL;
    if (getenv("GMBOX_NVME_DELAY_US"))
        g_nvme_delay_us = (unsigned)atoi(getenv("GMBOX_NVME_DELAY_US"));
    if (!nvme_sync) {
        pthread_t nt;
        pthread_create(&nt, NULL, nvme_worker, NULL);
    }
    pci_init();
    usb_init();
    fwcfg_build();

    const char *disk_path = getenv("GMBOX_DISK") ? getenv("GMBOX_DISK") : DISK_PATH;
    disk_fd = open(disk_path, O_RDWR);
    if (disk_fd < 0)
        disk_fd = open(disk_path, O_RDONLY);
    if (disk_fd < 0) {
        fprintf(stderr, "disk open failed: %s\n", strerror(errno));
    } else {
        struct stat st;
        if (fstat(disk_fd, &st) == 0)
            disk_bytes = (uint64_t)st.st_size;
        lba_shift = detect_lba_shift();
        printf("disk: %llu bytes, %u-byte LBAs\n",
               (unsigned long long)disk_bytes, 1u << lba_shift);
    }

    hv_vcpu_t vcpu = 0;
    hv_vcpu_exit_t *exit_info = NULL;
    fail(hv_vcpu_create(&vcpu, &exit_info, NULL), "hv_vcpu_create");

    /* GICv3 affinity routing: the redistributor registers (hv_gic_*_redistributor_reg)
     * are HV_DENIED until MPIDR_EL1 has been set explicitly; without it no PPI
     * (timers) is ever deliverable. Bit 31 is RES1; Aff0 = cpu 0. */
    fail(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MPIDR_EL1, 0x80000000ULL), "set MPIDR_EL1");

    /*
     * Keep Windows bootmgfw on its non-PMU path: with PMUVer != 0 it reads
     * PMCR/PMCCNTR (which HVF does not expose here) and fails. QEMU's working
     * Windows path uses pmu=off.
     */
    uint64_t dfr0 = 0;
    if (hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ID_AA64DFR0_EL1, &dfr0) == HV_SUCCESS) {
        /* PMUVer 1 = PMUv3: with the in-kernel GIC the framework emulates the PMU
         * (PMUVer=3 is a reserved encoding and makes PMU accesses UNDEF). */
        const char *pv = getenv("GMBOX_PMUVER");
        dfr0 = (dfr0 & ~(0xfULL << 8)) | ((uint64_t)(pv ? atoi(pv) : 1) << 8);
        hv_return_t sr = hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ID_AA64DFR0_EL1, dfr0);
        { uint64_t chk=0; hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ID_AA64DFR0_EL1, &chk); fprintf(stderr, "DFR0 set=0x%llx ret=0x%x readback=0x%llx\n", (unsigned long long)dfr0, (unsigned)sr, (unsigned long long)chk); }
    }
    uint64_t pfr0 = 0;
    if (hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ID_AA64PFR0_EL1, &pfr0) == HV_SUCCESS) {
        if (!getenv("GMBOX_KEEP_PFR0"))
            pfr0 &= ~(0xfULL << 8);
        hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ID_AA64PFR0_EL1, pfr0);
    }

    if (g_kdbrk)
        hv_vcpu_set_trap_debug_exceptions(vcpu, true);
    fail(hv_vcpu_set_reg(vcpu, HV_REG_PC, FW_IPA), "set PC");
    fail(hv_vcpu_set_reg(vcpu, HV_REG_X0, DTB_IPA), "set X0");
    fail(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c5), "set CPSR");

    printf("GMBox HVF EDK2+NVMe boot (dtb=%s)\n", dtb_path);
    g_vcpu = vcpu;

    if (getenv("GMBOX_STEPTRACE_ADMIN")) {
        g_admin_db_at = (unsigned)atoi(getenv("GMBOX_STEPTRACE_ADMIN"));
        g_st_n = atoi(getenv("GMBOX_STEPTRACE"));
    } else
    if (getenv("GMBOX_STEPTRACE")) {
        g_st_n = atoi(getenv("GMBOX_STEPTRACE"));
        pthread_t stt;
        pthread_create(&stt, NULL, st_thread, NULL);
    }
    if (getenv("GMBOX_PCSAMPLE")) {
        g_pcsample = 1;
        g_pcs_vcpu = vcpu;
        pthread_t pt;
        pthread_create(&pt, NULL, pcs_thread, NULL);
    }
    if (getenv("GMBOX_AUTOENTER")) {
        pthread_t aet;
        pthread_create(&aet, NULL, autoenter_thread, NULL);
    }
    if (getenv("GMBOX_FB_DUMP")) {
        pthread_t fbt;
        pthread_create(&fbt, NULL, fb_thread, NULL);
    }
    pthread_t wd_thread;
    if (watchdog_sec > 0)
        pthread_create(&wd_thread, NULL, watchdog, (void *)watchdog_sec);

    for (;;) {
        hv_return_t r = hv_vcpu_run(vcpu);
        if (r != HV_SUCCESS) {
            fprintf(stderr, "hv_vcpu_run=0x%x\n", (unsigned)r);
            break;
        }
        if (!exit_info) {
            fprintf(stderr, "no exit info\n");
            break;
        }

        if (g_usb_tick && exit_info->reason == HV_EXIT_REASON_CANCELED && !g_watchdog_fired) {
            g_usb_tick = 0;
            usb_tick();
            continue;
        }
        if (g_watchdog_fired)
            exit_info->reason = HV_EXIT_REASON_CANCELED;
        if (exit_info->reason == HV_EXIT_REASON_CANCELED && g_st_arm && !g_watchdog_fired) {
            g_st_arm = 0;
            st_begin(vcpu);
            continue;
        }
        if (exit_info->reason == HV_EXIT_REASON_CANCELED && g_pcsample && !g_watchdog_fired) {
            uint64_t spc = 0, scp = 0;
            hv_vcpu_get_reg(vcpu, HV_REG_PC, &spc);
            hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &scp);
            pcs_add(spc, scp);
            { uint64_t e1=0,l1=0,f1=0; hv_vcpu_get_sys_reg(vcpu,HV_SYS_REG_ESR_EL1,&e1); hv_vcpu_get_sys_reg(vcpu,HV_SYS_REG_ELR_EL1,&l1); hv_vcpu_get_sys_reg(vcpu,HV_SYS_REG_FAR_EL1,&f1); exs_add(e1,l1,f1); }
            continue;
        }
        if (exit_info->reason == HV_EXIT_REASON_CANCELED) {
            uint64_t pc = 0;
            hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
            if (g_pcsample) pcs_dump();
            if (getenv("GMBOX_BT")) guest_backtrace(vcpu, "watchdog");
            { uint64_t cc = 0, cv = 0, mask = 0; hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_CNTV_CTL_EL0, &cc); hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_CNTV_CVAL_EL0, &cv); hv_vcpu_get_vtimer_mask(vcpu, (bool *)&mask); uint64_t now = 0; hv_vcpu_get_vtimer_offset(vcpu, &now); fprintf(stderr, "VTIMER now=0x%llx ", (unsigned long long)mach_absolute_time()); fprintf(stderr, "VTIMER ctl=0x%llx cval=0x%llx mask=%llu voff=0x%llx exits=%u\n", (unsigned long long)cc, (unsigned long long)cv, (unsigned long long)(mask & 1), (unsigned long long)now, g_vt_exits); }
            if (getenv("GMBOX_NVRING")) nv_ring_dump();
            if (getenv("GMBOX_NVRING")) {
                for (int qi = 0; qi < 3; qi++) fprintf(stderr, "NVQ%d sq_head=%u sq_tail=%u cq_head=%u cq_tail=%u phase=%u sqsize=%u cqsize=%u cqid=%u sq_base=0x%llx cq_base=0x%llx\n", qi, nq[qi].sq_head, nq[qi].sq_tail, nq[qi].cq_head, nq[qi].cq_tail, nq[qi].phase, nq[qi].sqsize, nq[qi].cqsize, nq[qi].cqid, (unsigned long long)nq[qi].sq_base, (unsigned long long)nq[qi].cq_base);
                fprintf(stderr, "PCICFG dev1:"); for (int k = 0; k < 0x40; k++) fprintf(stderr, "%s%02x", (k % 16) ? " " : "\n  ", pci_cfg[0x8000 + k]); fprintf(stderr, "\n");
                fprintf(stderr, "nvme_cc=0x%llx csts=0x%llx intms=0x%x aqa=0x%llx\n", (unsigned long long)nvme_cc, (unsigned long long)nvme_csts, nvme_intms, (unsigned long long)nvme_aqa);
                const uint32_t *cq = gpa(nvme_acq, 64);
                if (cq) for (int e = 0; e < 4; e++) fprintf(stderr, "ACQ[%d] %08x %08x %08x %08x\n", e, cq[e*4], cq[e*4+1], cq[e*4+2], cq[e*4+3]);
            }
            if (getenv("GMBOX_GIC_DUMP")) {
                static const uint16_t regs[] = { 0x0000, 0x0084, 0x0104, 0x0204, 0x0304, 0x0424, 0x0c08 };
                for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++) { uint64_t v = 0; hv_return_t r = hv_gic_get_distributor_reg((hv_gic_distributor_reg_t)regs[i], &v); fprintf(stderr, "GICD[0x%04x]=0x%llx r=%x\n", regs[i], (unsigned long long)v, (unsigned)r); }
                { uint64_t v = 0; hv_return_t r = hv_gic_get_distributor_reg((hv_gic_distributor_reg_t)0x6120, &v); fprintf(stderr, "GICD_IROUTER36=0x%llx r=%x\n", (unsigned long long)v, (unsigned)r); }
                static const uint32_t rr[] = { 0x10080, 0x10100, 0x10200, 0x10400, 0x10418, 0x10c04 };
                for (unsigned i = 0; i < sizeof rr / sizeof rr[0]; i++) { uint64_t v = 0; hv_return_t r = hv_gic_get_redistributor_reg(vcpu, (hv_gic_redistributor_reg_t)rr[i], &v); fprintf(stderr, "GICR[0x%05x]=0x%llx r=%x\n", rr[i], (unsigned long long)v, (unsigned)r); }
                static const hv_gic_icc_reg_t ic[] = { HV_GIC_ICC_REG_PMR_EL1, HV_GIC_ICC_REG_RPR_EL1 };
                for (unsigned i = 0; i < 2; i++) { uint64_t v = 0; hv_return_t r = hv_gic_get_icc_reg(vcpu, ic[i], &v); fprintf(stderr, "ICC[%u]=0x%llx r=%x\n", i, (unsigned long long)v, (unsigned)r); }
                fprintf(stderr, "nvme_irq_level=%d\n", nvme_irq_level);
            }
            if (getenv("GMBOX_DUMP_GUEST")) {
                uint64_t gpc = 0, glr = 0, gsp = 0, gfp = 0;
                hv_vcpu_get_reg(vcpu, HV_REG_PC, &gpc); hv_vcpu_get_reg(vcpu, HV_REG_LR, &glr);
                hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL1, &gsp); hv_vcpu_get_reg(vcpu, HV_REG_FP, &gfp);
                uint64_t elr = 0; hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, &elr);
                fprintf(stderr, "GUEST pc=0x%llx lr=0x%llx sp=0x%llx fp=0x%llx elr=0x%llx\n", (unsigned long long)gpc, (unsigned long long)glr, (unsigned long long)gsp, (unsigned long long)gfp, (unsigned long long)elr);
                dump_guest_va(vcpu, "PCPAGE", gpc & ~0xfffULL, 0x1000, "/tmp/claude-501/pcpage.bin");
                dump_guest_va(vcpu, "ELRPAGE", elr & ~0xfffULL, 0x1000, "/tmp/claude-501/elrpage.bin");
                dump_guest_va(vcpu, "LRPAGE", glr & ~0xfffULL, 0x1000, "/tmp/claude-501/lrpage.bin");
                for (int sp_i = 0; sp_i < 6; sp_i++) { char nm[64]; snprintf(nm, sizeof nm, "/tmp/claude-501/stack%d.bin", sp_i); dump_guest_va(vcpu, "STACK", (gsp & ~0xfffULL) + (uint64_t)sp_i * 0x1000, 0x1000, nm); }
            }
            { uint64_t e1=0,l1=0,d0=0,x9=0; hv_vcpu_get_sys_reg(vcpu,HV_SYS_REG_ESR_EL1,&e1); hv_vcpu_get_sys_reg(vcpu,HV_SYS_REG_ELR_EL1,&l1); hv_vcpu_get_sys_reg(vcpu,HV_SYS_REG_ID_AA64DFR0_EL1,&d0); hv_vcpu_get_reg(vcpu,HV_REG_X9,&x9); { uint64_t cf=0,mp=0; hv_vcpu_get_sys_reg(vcpu,HV_SYS_REG_CNTKCTL_EL1,&cf); hv_vcpu_get_sys_reg(vcpu,HV_SYS_REG_MPIDR_EL1,&mp); const uint64_t *fv = gpa(0xbc42f000ULL + 0x286b88, 8); fprintf(stderr,"cntkctl=0x%llx mpidr=0x%llx freqvar=0x%llx\n",(unsigned long long)cf,(unsigned long long)mp,(unsigned long long)(fv?*fv:0)); } fprintf(stderr,"guest ESR_EL1=0x%llx ELR_EL1=0x%llx DFR0=0x%llx\n",(unsigned long long)e1,(unsigned long long)l1,(unsigned long long)d0);}
            fprintf(stderr, "%s PC=0x%llx\n",
                    g_watchdog_fired ? "watchdog" : "canceled",
                    (unsigned long long)pc);
            break;
        }
        if (exit_info->reason == HV_EXIT_REASON_VTIMER_ACTIVATED) {
            g_vt_exits++;
            hv_vcpu_set_vtimer_mask(vcpu, false);
            continue;
        }
        if (exit_info->reason != HV_EXIT_REASON_EXCEPTION) {
            fprintf(stderr, "reason=%u\n", (unsigned)exit_info->reason);
            break;
        }

        uint64_t esr = exit_info->exception.syndrome;
        uint64_t ec = (esr >> 26) & 0x3f;
        if (g_trace_run) { static int dbg; if (dbg++ < 8) fprintf(stderr, "TRACE exit ec=0x%llx esr=0x%llx\n", (unsigned long long)ec, (unsigned long long)esr); }
        if (ec == 0x16 || ec == 0x17) {
            if (psci(vcpu))
                break;
            continue;
        }
        if (g_trace_run && (ec == 0x30 || ec == 0x31)) {
            tr_bp_hit(vcpu);
            continue;
        }
        if ((ec == 0x32 || ec == 0x33) && g_st_active) {
            st_step(vcpu);
            continue;
        }
        if (ec == 0x32 || ec == 0x33) {
            tr_step(vcpu);
            continue;
        }
        if (ec == 0x3c && g_kdbrk && kd_brk(vcpu, esr))
            continue;
        if (ec == 0x18 && sysreg_trap(vcpu, esr))
            continue;
        if (ec == 0x24 || ec == 0x25) {
            mmio(vcpu, exit_info);
            if (g_trace_arm) { g_trace_arm = 0; tr_begin(vcpu); }
            if (g_st_arm_now) { g_st_arm_now = 0; st_begin(vcpu); }
            continue;
        }
        fprintf(stderr, "exception EC=0x%llx ESR=0x%llx pa=0x%llx\n",
                (unsigned long long)ec, (unsigned long long)esr,
                (unsigned long long)exit_info->exception.physical_address);
        dump_abort(vcpu, exit_info);
        break;
    }

    hv_vcpu_destroy(vcpu);
    hv_vm_destroy();
    if (disk_fd >= 0)
        close(disk_fd);
    return 0;
}
