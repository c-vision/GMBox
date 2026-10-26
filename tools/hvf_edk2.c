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

#define PAGE 4096UL
#define FW_IPA 0x00000000UL
#define FW_SIZE (64UL * 1024 * 1024)
#define VARS_IPA 0x04000000UL
#define VARS_SIZE (64UL * 1024 * 1024)
#define RAM_IPA 0x40000000UL
#define RAM_SIZE (8UL * 1024 * 1024 * 1024)
#define DTB_IPA RAM_IPA
#define GICD_IPA 0x08000000UL
#define GICR_IPA 0x080A0000UL
#define UART_IPA 0x09000000UL
#define RTC_IPA 0x09010000UL

static void fail(hv_return_t r, const char *what)
{
    if (r == HV_SUCCESS)
        return;
    fprintf(stderr, "%s: hv_return_t=0x%x\n", what, (unsigned)r);
    exit(1);
}

static void *g_fw, *g_vars, *g_ram;

static uint32_t guest_read32(uint64_t a)
{
    uint32_t v = 0xffffffffu;
    if (a >= FW_IPA && a + 4 <= FW_IPA + FW_SIZE)
        memcpy(&v, (uint8_t *)g_fw + (a - FW_IPA), 4);
    else if (a >= VARS_IPA && a + 4 <= VARS_IPA + VARS_SIZE)
        memcpy(&v, (uint8_t *)g_vars + (a - VARS_IPA), 4);
    else if (a >= RAM_IPA && a + 4 <= RAM_IPA + RAM_SIZE)
        memcpy(&v, (uint8_t *)g_ram + (a - RAM_IPA), 4);
    return v;
}

static int insn_is_hvc_smc(uint32_t w)
{
    return (w & 0xffe0001fu) == 0xd4000002u ||
           (w & 0xffe0001fu) == 0xd4000003u;
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

static uint32_t uart_read(uint64_t off)
{
    switch (off) {
    case 0x00: return 0;
    case 0x18: return 0x90;
    case 0x2c: return 0x3;
    case 0x30: return 0x301;
    default: return 0;
    }
}

static void uart_write(uint64_t off, uint64_t value)
{
    if (off == 0x00)
        putchar((int)(value & 0xff));
}

static void mmio(hv_vcpu_t vcpu, hv_vcpu_exit_t *e)
{
    uint64_t esr = e->exception.syndrome;
    uint64_t iss = esr & 0x1ffffffULL;
    uint64_t pa = e->exception.physical_address;
    uint64_t off = pa;
    int wnr = (int)((iss >> 6) & 1);
    int isv = (int)((iss >> 24) & 1);
    int srt = (int)((iss >> 16) & 0x1f);
    uint64_t pc = 0;
    hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);

    if (pa >= UART_IPA && pa < UART_IPA + 0x1000) {
        off = pa - UART_IPA;
        if (wnr) {
            if (isv)
                uart_write(off, read_reg(vcpu, (uint32_t)srt));
        } else {
            if (isv)
                write_reg(vcpu, (uint32_t)srt, uart_read(off));
        }
        hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
        return;
    }
    if (pa >= RTC_IPA && pa < RTC_IPA + 0x1000) {
        off = pa - RTC_IPA;
        if (!wnr && isv)
            write_reg(vcpu, (uint32_t)srt, off == 0 ? (uint64_t)time(NULL) : 0);
        hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
        return;
    }
    if (pa >= GICR_IPA && pa < GICR_IPA + 0x20000) {
        uint64_t goff = pa - GICR_IPA;
        if (goff == 0x14) {
            if (!wnr && isv)
                write_reg(vcpu, (uint32_t)srt, 0);
        } else if (wnr) {
            hv_gic_set_redistributor_reg(vcpu, (hv_gic_redistributor_reg_t)goff, read_reg(vcpu, (uint32_t)srt));
        } else if (isv) {
            uint64_t gv = 0;
            hv_gic_get_redistributor_reg(vcpu, (hv_gic_redistributor_reg_t)goff, &gv);
            write_reg(vcpu, (uint32_t)srt, gv);
        }
        hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
        return;
    }

    static int unknown;
    if (unknown++ < 20)
        fprintf(stderr, "MMIO unhandled pa=0x%llx wnr=%d isv=%d rt=%d ec=0x%llx\n",
                (unsigned long long)pa, wnr, isv, srt,
                (unsigned long long)((esr >> 26) & 0x3f));
    if (!wnr && isv)
        write_reg(vcpu, (uint32_t)srt, 0);
    hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
}

static int psci(hv_vcpu_t vcpu, hv_vcpu_exit_t *e)
{
    (void)e;
    uint64_t x0 = 0, pc = 0;
    hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0);
    hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
    uint64_t ret = 0xffffffffffffffffULL;
    uint64_t r1 = 0, r2 = 0, r3 = 0;
    if (x0 == 0x84000000ULL || x0 == 0x80000000ULL)
        ret = 0x00010001ULL;
    else if (x0 == 0x84000050ULL)
        ret = 0x00010000ULL;
    else if (x0 == 0x84000051ULL)
        ret = 0;
    else if (x0 == 0x84000052ULL) {
        ret = 0x12345678ULL;
        r1 = 0x9abcdef0ULL; r2 = 0x0fedcba9ULL; r3 = 0x87654321ULL;
    } else if (x0 == 0x84000053ULL || x0 == 0xc4000053ULL) {
        uint64_t t = (uint64_t)time(NULL);
        ret = 0;
        r1 = t * 6364136223846793005ULL + 1;
        r2 = r1 * 6364136223846793005ULL + 2;
        r3 = r2 * 6364136223846793005ULL + 3;
    } else if (x0 == 0x84000008ULL || x0 == 0x84000009ULL)
        return 1;
    hv_vcpu_set_reg(vcpu, HV_REG_X0, ret);
    if (x0 == 0x84000052ULL || x0 == 0x84000053ULL || x0 == 0xc4000053ULL) {
        hv_vcpu_set_reg(vcpu, HV_REG_X1, r1);
        hv_vcpu_set_reg(vcpu, HV_REG_X2, r2);
        hv_vcpu_set_reg(vcpu, HV_REG_X3, r3);
    }
    int advance = 1;
    if (insn_is_hvc_smc(guest_read32(pc)))
        advance = 1;
    else if (pc >= 4 && insn_is_hvc_smc(guest_read32(pc - 4)))
        advance = 0;
    hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + (advance ? 4 : 0));
    printf("PSCI 0x%llx -> 0x%llx\n", (unsigned long long)x0,
           (unsigned long long)ret);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    const char *dtb_path = argc > 1 ? argv[1]
        : "/Users/gaetano/Documents/Projects/GMBox/tools/qemu-virt-nvme-gic3.dtb";
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
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

    void *fw = map_anon(FW_SIZE);
    g_fw = fw;
    void *vars = map_anon(VARS_SIZE);
    g_vars = vars;
    void *ram = map_anon(RAM_SIZE);
    g_ram = ram;
    fail(hv_vm_map(fw, FW_IPA, FW_SIZE,
                   HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
         "map firmware");
    fail(hv_vm_map(vars, VARS_IPA, VARS_SIZE,
                   HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
         "map vars");
    fail(hv_vm_map(ram, RAM_IPA, RAM_SIZE,
                   HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
         "map ram");

    load_file("/opt/homebrew/share/qemu/edk2-aarch64-code.fd", fw, FW_SIZE);
    load_file("/opt/homebrew/share/qemu/edk2-arm-vars.fd", vars, VARS_SIZE);
    load_file(dtb_path, ram, 1024 * 1024);

    hv_vcpu_t vcpu = 0;
    hv_vcpu_exit_t *exit_info = NULL;
    fail(hv_vcpu_create(&vcpu, &exit_info, NULL), "hv_vcpu_create");
    fail(hv_vcpu_set_reg(vcpu, HV_REG_PC, FW_IPA), "set PC");
    fail(hv_vcpu_set_reg(vcpu, HV_REG_X0, DTB_IPA), "set X0");
    fail(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c5), "set CPSR");

    printf("GMBox HVF EDK2 boot\n");
    fflush(stdout);
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
        if (exit_info->reason == HV_EXIT_REASON_VTIMER_ACTIVATED) {
            hv_vcpu_set_vtimer_mask(vcpu, false);
            continue;
        }
        if (exit_info->reason != HV_EXIT_REASON_EXCEPTION) {
            fprintf(stderr, "reason=%u\n", (unsigned)exit_info->reason);
            break;
        }
        uint64_t esr = exit_info->exception.syndrome;
        uint64_t ec = (esr >> 26) & 0x3f;
        if (ec == 0x16 || ec == 0x17) {
            if (psci(vcpu, exit_info))
                break;
            continue;
        }
        if (ec == 0x24 || ec == 0x25) {
            mmio(vcpu, exit_info);
            continue;
        }
        fprintf(stderr, "exception EC=0x%llx ESR=0x%llx pa=0x%llx\n",
                (unsigned long long)ec, (unsigned long long)esr,
                (unsigned long long)exit_info->exception.physical_address);
        break;
    }

    hv_vcpu_destroy(vcpu);
    hv_vm_destroy();
    return 0;
}
