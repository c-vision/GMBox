#include <Hypervisor/Hypervisor.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE 4096UL
#define RAM_IPA 0x40000000UL
#define RAM_SIZE (4 * PAGE)
#define DATA_IPA (RAM_IPA + 0x2000UL)
#define GICD_IPA 0x08000000UL
#define GICR_IPA 0x080A0000UL

static const uint32_t guest_code[] = {
    0xd2a10003, 0xb9400464, 0xf9000024, 0xd53be002,
    0xf9000422, 0xd53be042, 0xf9000822, 0xd4000002,
};

static void fail(hv_return_t r, const char *what)
{
    if (r == HV_SUCCESS)
        return;
    fprintf(stderr, "%s: hv_return_t=0x%x\n", what, (unsigned)r);
    exit(1);
}

int main(void)
{
    hv_vm_config_t cfg = hv_vm_config_create();
    if (!cfg) {
        fprintf(stderr, "hv_vm_config_create failed\n");
        return 1;
    }
    fail(hv_vm_create(cfg), "hv_vm_create");

    size_t gicd_align = 0;
    size_t gicr_align = 0;
    uint32_t spi_base = 0;
    uint32_t spi_count = 0;
    fail(hv_gic_get_distributor_base_alignment(&gicd_align), "gicd alignment");
    fail(hv_gic_get_redistributor_base_alignment(&gicr_align), "gicr alignment");
    fail(hv_gic_get_spi_interrupt_range(&spi_base, &spi_count), "spi range");
    printf("gicd_align=0x%zx gicr_align=0x%zx spi=%u..%u\n",
           gicd_align, gicr_align, spi_base, spi_base + spi_count - 1);
    if ((GICD_IPA % gicd_align) != 0 || (GICR_IPA % gicr_align) != 0) {
        fprintf(stderr, "GIC base non allineata\n");
        return 1;
    }

    hv_gic_config_t gic_cfg = hv_gic_config_create();
    if (!gic_cfg) {
        fprintf(stderr, "hv_gic_config_create failed\n");
        return 1;
    }
    fail(hv_gic_config_set_distributor_base(gic_cfg, GICD_IPA), "set gicd");
    fail(hv_gic_config_set_redistributor_base(gic_cfg, GICR_IPA), "set gicr");
    fail(hv_gic_create(gic_cfg), "hv_gic_create");

    void *mem = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    fail(hv_vm_map(mem, RAM_IPA, RAM_SIZE,
                   HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
         "hv_vm_map");
    memcpy(mem, guest_code, sizeof(guest_code));

    hv_vcpu_t vcpu = 0;
    hv_vcpu_exit_t *exit_info = NULL;
    fail(hv_vcpu_create(&vcpu, &exit_info, NULL), "hv_vcpu_create");
    fail(hv_vcpu_set_reg(vcpu, HV_REG_PC, RAM_IPA), "set PC");
    fail(hv_vcpu_set_reg(vcpu, HV_REG_X1, DATA_IPA), "set X1");
    fail(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c5), "set CPSR");

    printf("GMBox HVF GIC probe\n");
    fflush(stdout);
    hv_return_t r = hv_vcpu_run(vcpu);
    printf("hv_vcpu_run=0x%x reason=%u\n", (unsigned)r,
           exit_info ? (unsigned)exit_info->reason : 0xffffffffU);
    if (r == HV_SUCCESS && exit_info &&
        exit_info->reason == HV_EXIT_REASON_EXCEPTION) {
        uint64_t esr = exit_info->exception.syndrome;
        printf("EC=0x%llx ESR=0x%llx pa=0x%llx\n",
               (unsigned long long)((esr >> 26) & 0x3f),
               (unsigned long long)esr,
               (unsigned long long)exit_info->exception.physical_address);
    }

    uint64_t *data = (uint64_t *)((char *)mem + (DATA_IPA - RAM_IPA));
    printf("GICD_TYPER=0x%llx CNTFRQ_EL0=%llu CNTVCT_EL0=%llu\n",
           (unsigned long long)(data[0] & 0xffffffffU),
           (unsigned long long)data[1],
           (unsigned long long)data[2]);

    hv_vcpu_destroy(vcpu);
    hv_vm_destroy();
    munmap(mem, RAM_SIZE);
    return 0;
}
