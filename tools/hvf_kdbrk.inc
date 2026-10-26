/*
 * Windows-kernel BRK services (ARM64): with debug exceptions trapped to the
 * VMM, `brk #0xf000` (DbgBreakPoint) and `brk #0xf002` (DEBUG_SERVICE:
 * x16 = service, e.g. 1 = DbgPrint with x0 = string, w1 = length) arrive here.
 * We act as a trivial "attached debugger": print kernel DbgPrint output and
 * continue after the instruction.
 */
static unsigned g_brk_n;

static int kd_brk(hv_vcpu_t vcpu, uint64_t esr)
{
    uint32_t imm = (uint32_t)(esr & 0xffff);
    uint64_t pc = 0, x0 = 0, x1 = 0, x16 = 0;
    hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
    hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0);
    hv_vcpu_get_reg(vcpu, HV_REG_X1, &x1);
    hv_vcpu_get_reg(vcpu, (hv_reg_t)(HV_REG_X0 + 16), &x16);
    if (imm == 0xf002 && x16 == 3) { /* DbgLoadImageSymbols: x0 -> STRING { len, max, buf } */
        uint64_t pa, bpa; const uint8_t *sp;
        if (va_to_pa(vcpu, x0, &pa) == 0 && (sp = gpa(pa, 16))) {
            uint16_t len = *(const uint16_t *)sp; uint64_t buf = *(const uint64_t *)(sp + 8);
            if (len < 260 && va_to_pa(vcpu, buf, &bpa) == 0 && gpa(bpa, len))
                fprintf(stderr, "KDLOAD %.*s\n", (int)len, (const char *)gpa(bpa, len));
        }
    } else
    if (imm == 0xf002 && x16 == 1 && x1 && x1 < 1024) {
        uint64_t pa;
        if (va_to_pa(vcpu, x0, &pa) == 0) {
            const char *s = gpa(pa, x1);
            if (s) {
                fprintf(stderr, "KDPRINT: %.*s", (int)x1, s);
                if (s[x1 - 1] != '\n')
                    fputc('\n', stderr);
            }
        }
    } else if (g_brk_n == 0 && getenv("GMBOX_KDBRK_DUMP")) {
        uint64_t lr0 = 0, sp0 = 0, fp0 = 0;
        hv_vcpu_get_reg(vcpu, HV_REG_LR, &lr0);
        hv_vcpu_get_reg(vcpu, HV_REG_FP, &fp0);
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL1, &sp0);
        fprintf(stderr, "KDDUMP pc=0x%llx lr=0x%llx sp=0x%llx fp=0x%llx\n", (unsigned long long)pc, (unsigned long long)lr0, (unsigned long long)sp0, (unsigned long long)fp0);
        for (int i = 0; i < 31; i += 1) { uint64_t r = 0; hv_vcpu_get_reg(vcpu, (hv_reg_t)(HV_REG_X0 + i), &r); fprintf(stderr, "  x%d=0x%llx%s", i, (unsigned long long)r, (i % 4 == 3) ? "\n" : ""); }
        fprintf(stderr, "\n");
        for (int i = 0; i < 8; i++) { char nm[96]; snprintf(nm, sizeof nm, "/tmp/claude-501/kd_stack%d.bin", i); dump_guest_va(vcpu, "KDSTACK", (sp0 & ~0xfffULL) + (uint64_t)i * 0x1000, 0x1000, nm); }
        dump_guest_va(vcpu, "KDPC", pc & ~0xfffULL, 0x1000, "/tmp/claude-501/kd_pc.bin");
        for (int i = -1; i < 3; i++) { char nm[96]; snprintf(nm, sizeof nm, "/tmp/claude-501/kd_lr%d.bin", i + 1); dump_guest_va(vcpu, "KDLR", (lr0 & ~0xfffULL) + (int64_t)i * 0x1000, 0x1000, nm); }
        { uint64_t f = fp0; for (int fr = 0; fr < 10 && f; fr++) { uint64_t fpa; if (va_to_pa(vcpu, f, &fpa) < 0) break; const uint64_t *fv = gpa(fpa, 16); if (!fv) break; char nm[96]; snprintf(nm, sizeof nm, "/tmp/claude-501/kd_fr%d.bin", fr); fprintf(stderr, "KDFRAME %d fp=0x%llx lr=0x%llx\n", fr, (unsigned long long)f, (unsigned long long)fv[1]); dump_guest_va(vcpu, "KDFR", fv[1] & ~0xfffULL, 0x1000, nm); f = fv[0]; } }
        g_brk_n++;
    } else if (g_brk_n++ < 40) {
        uint64_t lr = 0, sp = 0;
        hv_vcpu_get_reg(vcpu, HV_REG_LR, &lr);
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL1, &sp);
        fprintf(stderr, "KDBRK #%u imm=0x%x pc=0x%llx lr=0x%llx sp=0x%llx x0=0x%llx x16=0x%llx\n",
                g_brk_n, imm, (unsigned long long)pc, (unsigned long long)lr,
                (unsigned long long)sp, (unsigned long long)x0, (unsigned long long)x16);
    }
    hv_vcpu_set_reg(vcpu, HV_REG_X0, 0);
    hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4);
    return 1;
}
