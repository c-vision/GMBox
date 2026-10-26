/* Guest stage-1 page-table walk (4 KiB granule, up to 48-bit VA) for diagnostics. */
static int va_to_pa(hv_vcpu_t vcpu, uint64_t va, uint64_t *pa)
{
    uint64_t ttbr = 0;
    hv_vcpu_get_sys_reg(vcpu, (va >> 63) ? HV_SYS_REG_TTBR1_EL1 : HV_SYS_REG_TTBR0_EL1, &ttbr);
    uint64_t table = ttbr & 0x0000fffffffff000ULL;
    for (int level = 0; level < 4; level++) {
        int shift = 39 - 9 * level;
        const uint64_t *t = gpa(table, 4096);
        if (!t)
            return -1;
        uint64_t e = t[(va >> shift) & 0x1ff];
        if (!(e & 1))
            return -1;
        if (level == 3 || !(e & 2)) { /* page or block */
            uint64_t mask = (1ULL << shift) - 1;
            *pa = (e & 0x0000fffffffff000ULL & ~mask) | (va & mask);
            return 0;
        }
        table = e & 0x0000fffffffff000ULL;
    }
    return -1;
}

static void dump_guest_va(hv_vcpu_t vcpu, const char *tag, uint64_t va, size_t len, const char *path)
{
    uint64_t pa = 0;
    if (va_to_pa(vcpu, va, &pa) < 0) {
        fprintf(stderr, "%s: va 0x%llx not mapped\n", tag, (unsigned long long)va);
        return;
    }
    const uint8_t *p = gpa(pa, len);
    fprintf(stderr, "%s: va=0x%llx pa=0x%llx\n", tag, (unsigned long long)va, (unsigned long long)pa);
    if (p && path) {
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(p, 1, len, f); fclose(f); }
    }
}

/* Frame-pointer backtrace of the guest (x29 chain), for diagnostics. */
static void guest_backtrace(hv_vcpu_t vcpu, const char *tag)
{
    uint64_t pc = 0, fp = 0, lr = 0, cpsr = 0, sp0 = 0, sp1 = 0;
    hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
    hv_vcpu_get_reg(vcpu, HV_REG_FP, &fp);
    hv_vcpu_get_reg(vcpu, HV_REG_LR, &lr);
    hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &cpsr);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL0, &sp0);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL1, &sp1);
    fprintf(stderr, "BT[%s] pc=0x%llx lr=0x%llx fp=0x%llx cpsr=0x%llx sp_el0=0x%llx sp_el1=0x%llx\n", tag,
            (unsigned long long)pc, (unsigned long long)lr, (unsigned long long)fp,
            (unsigned long long)cpsr, (unsigned long long)sp0, (unsigned long long)sp1);
    for (int i = 0; i < 24 && fp; i++) {
        uint64_t pa;
        if (va_to_pa(vcpu, fp, &pa) < 0)
            break;
        const uint64_t *f = gpa(pa, 16);
        if (!f)
            break;
        fprintf(stderr, "  #%d lr=0x%llx\n", i, (unsigned long long)f[1]);
        if (f[0] <= fp && f[0] != 0)
            break;
        fp = f[0];
    }
}
