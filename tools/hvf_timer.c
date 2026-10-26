#include <Hypervisor/Hypervisor.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>

#define PAGE 4096UL
#define RAM_IPA 0x40000000UL
#define RAM_SIZE (4 * PAGE)
#define VECTOR_IPA (RAM_IPA + 0x1000UL)
#define CODE_IPA (RAM_IPA + 0x2000UL)
#define UART_IPA 0x09000000UL
#define GICD_IPA 0x08000000UL
#define GICR_IPA 0x080A0000UL

static const uint32_t guest_code[] = {
    0xd2a10140, 0xb9401401, 0x121e7821, 0xb9001401,
    0xd2a10000, 0x52800a41, 0xb9000001, 0xd2800021,
    0xd518cca1, 0xd5033fdf, 0xd2801fe1, 0xd5184601,
    0xd2800021, 0xd518cce1, 0xd53be000, 0xd342fc00,
    0xd51be300, 0xd2800020, 0xd51be320, 0xd50342ff,
    0xd503207f, 0x17ffffff, 0xd538cc00, 0x52800a82,
    0xd2a12003, 0x39000062, 0xd518cc20, 0xd51be33f,
    0xd4000002,
};

static hv_vcpu_t g_vcpu;

static void *cancel_thread(void *arg)
{
    (void)arg;
    sleep(0);
    usleep(500000);
    hv_vcpus_exit(&g_vcpu, 1);
    return NULL;
}

static void check(hv_return_t r, const char *what)
{
    if (r != HV_SUCCESS)
        fprintf(stderr, "%s: hv_return_t=0x%x\n", what, (unsigned)r);
}

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
    memcpy((char *)mem + (CODE_IPA - RAM_IPA), guest_code, sizeof(guest_code));
    *(uint32_t *)((char *)mem + (VECTOR_IPA - RAM_IPA) + 0x280) = 0x14000376;

    hv_vcpu_t vcpu = 0;
    hv_vcpu_exit_t *exit_info = NULL;
    fail(hv_vcpu_create(&vcpu, &exit_info, NULL), "hv_vcpu_create");
    fail(hv_vcpu_set_reg(vcpu, HV_REG_PC, CODE_IPA), "set PC");
    fail(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x345), "set CPSR");
    fail(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, VECTOR_IPA), "set VBAR");
    check(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MPIDR_EL1, 0x80000000ULL), "set MPIDR_EL1");
    hv_ipa_t gicr_base = 0;
    check(hv_gic_get_redistributor_base(vcpu, &gicr_base), "gicr base");
    printf("gicr_base=0x%llx\n", (unsigned long long)gicr_base);
    check(hv_gic_set_redistributor_reg(vcpu, HV_GIC_REDISTRIBUTOR_REG_GICR_ISENABLER0, 1ULL << 27), "enable PPI 27");
    check(hv_gic_set_redistributor_reg(vcpu, HV_GIC_REDISTRIBUTOR_REG_GICR_IPRIORITYR0, 0xff), "PPI priority");

    check(hv_gic_set_distributor_reg(HV_GIC_DISTRIBUTOR_REG_GICD_CTLR, 0x52), "enable GICD grp1");
    check(hv_gic_set_icc_reg(vcpu, HV_GIC_ICC_REG_SRE_EL1, 1), "set SRE");
    check(hv_gic_set_icc_reg(vcpu, HV_GIC_ICC_REG_PMR_EL1, 0xff), "set PMR");
    check(hv_gic_set_icc_reg(vcpu, HV_GIC_ICC_REG_IGRPEN1_EL1, 1), "set IGRPEN1");
    uint64_t sre = 0, pmr = 0, igrpen1 = 0;
    hv_gic_get_icc_reg(vcpu, HV_GIC_ICC_REG_SRE_EL1, &sre);
    hv_gic_get_icc_reg(vcpu, HV_GIC_ICC_REG_PMR_EL1, &pmr);
    hv_gic_get_icc_reg(vcpu, HV_GIC_ICC_REG_IGRPEN1_EL1, &igrpen1);
    printf("pre sre=0x%llx pmr=0x%llx igrpen1=0x%llx\n", (unsigned long long)sre, (unsigned long long)pmr, (unsigned long long)igrpen1);
    fail(hv_vcpu_set_vtimer_mask(vcpu, false), "unmask vtimer initial");
    printf("GMBox HVF timer IRQ probe\n");
    g_vcpu = vcpu;
    pthread_t th;
    pthread_create(&th, NULL, cancel_thread, NULL);
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
        if (exit_info->reason == HV_EXIT_REASON_CANCELED) {
            uint64_t pc = 0, ctl = 0, cval = 0, cpsr = 0;
            hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &cpsr);
            hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
            hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_CNTV_CTL_EL0, &ctl);
            hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_CNTV_CVAL_EL0, &cval);
            printf("CANCELED pc=0x%llx cpsr=0x%llx cntv_ctl=0x%llx cntv_cval=0x%llx\n",
                   (unsigned long long)pc, (unsigned long long)cpsr,
                   (unsigned long long)ctl, (unsigned long long)cval);
            if ((ctl & 4) != 0) {
                check(hv_vcpu_set_pending_interrupt(vcpu, HV_INTERRUPT_TYPE_IRQ, true), "pending IRQ");
                check(hv_vcpu_set_vtimer_mask(vcpu, false), "unmask after inject");
                continue;
            }
            break;
        }
        if (exit_info->reason == HV_EXIT_REASON_VTIMER_ACTIVATED) {
            printf("VTIMER\n");
            fflush(stdout);
            fail(hv_gic_set_redistributor_reg(
                     vcpu, HV_GIC_REDISTRIBUTOR_REG_GICR_ISPENDR0, 1ULL << 27),
                 "pend PPI 27");
            fail(hv_vcpu_set_vtimer_mask(vcpu, false), "unmask vtimer");
            continue;
        }
        if (exit_info->reason != HV_EXIT_REASON_EXCEPTION) {
            fprintf(stderr, "reason=%u\n", (unsigned)exit_info->reason);
            break;
        }

        uint64_t esr = exit_info->exception.syndrome;
        uint64_t ec = (esr >> 26) & 0x3f;
        uint64_t pa = exit_info->exception.physical_address;
        if (ec == 0x16) {
            printf("\nHVC\n");
            break;
        }
        if ((ec == 0x24 || ec == 0x25) && pa == UART_IPA) {
            uint64_t ch = 0;
            fail(hv_vcpu_get_reg(vcpu, HV_REG_X2, &ch), "get X2");
            putchar((int)(ch & 0xff));
            fflush(stdout);
            uint64_t pc = 0;
            fail(hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc), "get PC");
            fail(hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4), "advance PC");
            continue;
        }

        fprintf(stderr, "unhandled EC=0x%llx ESR=0x%llx pa=0x%llx\n",
                (unsigned long long)ec, (unsigned long long)esr,
                (unsigned long long)pa);
        break;
    }

    hv_vcpu_destroy(vcpu);
    hv_vm_destroy();
    munmap(mem, RAM_SIZE);
    return 0;
}
