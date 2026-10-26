#include <Hypervisor/Hypervisor.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096UL
#define CODE_IPA 0x1000UL
#define DATA_IPA 0x2000UL
#define STACK_IPA 0x3000UL
#define MAP_SIZE (4 * PAGE)

static void fail(hv_return_t r, const char *what)
{
    if (r == HV_SUCCESS)
        return;
    fprintf(stderr, "%s: hv_return_t=0x%x\n", what, (unsigned)r);
    exit(1);
}

static void check(hv_return_t r, const char *what)
{
    if (r != HV_SUCCESS)
        fprintf(stderr, "%s: hv_return_t=0x%x\n", what, (unsigned)r);
}

int main(int argc, char **argv)
{
    bool el2 = false;
    if (argc > 1 && strcmp(argv[1], "--el2") == 0)
        el2 = true;

    bool el2_supported = false;
    check(hv_vm_config_get_el2_supported(&el2_supported), "el2 supported");
    printf("el2_supported=%d\n", el2_supported);
    if (el2 && !el2_supported) {
        fprintf(stderr, "EL2 non supportato; eseguo senza EL2\n");
        el2 = false;
    }

    hv_vm_config_t cfg = hv_vm_config_create();
    if (!cfg) {
        fprintf(stderr, "hv_vm_config_create failed\n");
        return 1;
    }
    if (el2)
        fail(hv_vm_config_set_el2_enabled(cfg, true), "set el2");

    fail(hv_vm_create(cfg), "hv_vm_create");

    void *mem = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    fail(hv_vm_map(mem, 0, MAP_SIZE,
                   HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
         "hv_vm_map");

    static const uint32_t code[] = {
        0xd53b9d00,
        0xf9000020,
        0xd4000002,
        0x14000000,
    };
    memcpy((char *)mem + CODE_IPA, code, sizeof(code));

    hv_vcpu_t vcpu = 0;
    hv_vcpu_exit_t *exit_info = NULL;
    fail(hv_vcpu_create(&vcpu, &exit_info, NULL), "hv_vcpu_create");

    uint64_t dfr0 = 0;
    check(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ID_AA64DFR0_EL1, &dfr0),
          "get ID_AA64DFR0_EL1");
    printf("ID_AA64DFR0_EL1 before=0x%llx PMUVer=%llu\n",
           (unsigned long long)dfr0,
           (unsigned long long)((dfr0 >> 8) & 0xf));
    uint64_t dfr0_pmu3 = (dfr0 & ~(0xfULL << 8)) | (3ULL << 8);
    check(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ID_AA64DFR0_EL1, dfr0_pmu3),
          "set ID_AA64DFR0_EL1.PMUVer=3");
    check(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ID_AA64DFR0_EL1, &dfr0),
          "get ID_AA64DFR0_EL1");

    fail(hv_vcpu_set_reg(vcpu, HV_REG_PC, CODE_IPA), "set PC");
    fail(hv_vcpu_set_reg(vcpu, HV_REG_X1, DATA_IPA), "set X1");
    fail(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c5), "set CPSR");

    printf("run: el2=%d dfr0=0x%llx PMUVer=%llu\n", el2,
           (unsigned long long)dfr0,
           (unsigned long long)((dfr0 >> 8) & 0xf));
    fflush(stdout);
    hv_return_t rr = HV_SUCCESS;
    int emulated = 0;
    for (int round = 0; round < 3; round++) {
        rr = hv_vcpu_run(vcpu);
        printf("hv_vcpu_run=0x%x reason=%u\n", (unsigned)rr,
               exit_info ? (unsigned)exit_info->reason : 0xffffffffU);
        if (rr != HV_SUCCESS || !exit_info ||
            exit_info->reason != HV_EXIT_REASON_EXCEPTION)
            break;
        uint64_t esr = exit_info->exception.syndrome;
        uint64_t ec = (esr >> 26) & 0x3f;
        printf("exception ESR=0x%llx EC=0x%llx ISS=0x%llx va=0x%llx pa=0x%llx\n",
               (unsigned long long)esr, (unsigned long long)ec,
               (unsigned long long)(esr & 0x1ffffff),
               (unsigned long long)exit_info->exception.virtual_address,
               (unsigned long long)exit_info->exception.physical_address);
        if (ec != 0x18 || emulated)
            break;
        uint64_t pc = 0;
        fail(hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc), "get PC");
        fail(hv_vcpu_set_reg(vcpu, HV_REG_X0, 0x1122334455667788ULL), "set X0");
        fail(hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4), "advance PC");
        emulated = 1;
        printf("PMU trap emulated at PC=0x%llx\n", (unsigned long long)pc);
    }

    uint64_t value = *(volatile uint64_t *)((char *)mem + DATA_IPA);
    printf("DATA[0x2000]=0x%llx (%llu)\n",
           (unsigned long long)value, (unsigned long long)value);

    hv_vcpu_destroy(vcpu);
    hv_vm_destroy();
    munmap(mem, MAP_SIZE);
    return 0;
}
