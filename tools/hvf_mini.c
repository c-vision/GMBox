#include <Hypervisor/Hypervisor.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096UL
#define RAM_IPA 0x40000000UL
#define RAM_SIZE (4 * PAGE)
#define UART_IPA 0x09000000UL

static const uint32_t guest_code[] = {
    0x100000e0, 0xd2a12003, 0x38401402, 0x34000062,
    0x39000062, 0x17fffffd, 0xd4000002, 0x6f424d47,
    0x56482078, 0x00000a46,
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
    fail(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c5), "set CPSR");

    printf("GMBox HVF mini-VMM\n");
    fflush(stdout);

    for (;;) {
        hv_return_t r = hv_vcpu_run(vcpu);
        if (r != HV_SUCCESS) {
            fprintf(stderr, "hv_vcpu_run=0x%x\n", (unsigned)r);
            break;
        }
        if (!exit_info || exit_info->reason != HV_EXIT_REASON_EXCEPTION) {
            fprintf(stderr, "exit reason=%u\n",
                    exit_info ? (unsigned)exit_info->reason : 0xffffffffU);
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
