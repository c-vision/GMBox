/*
 * Post-mortem instruction trace of the running guest (GMBOX_STEPTRACE=N,
 * GMBOX_STEPTRACE_AT=seconds): after the delay, single-step N instructions and
 * print the executed PCs as compressed runs. Used to see what a spinning
 * guest is actually doing.
 */
static int g_st_n, g_st_arm, g_st_active;
static uint64_t *g_st_pc;
static int g_st_left, g_st_cnt;

static void *st_thread(void *arg)
{
    (void)arg;
    const char *at = getenv("GMBOX_STEPTRACE_AT");
    sleep(at ? (unsigned)atoi(at) : 60);
    g_st_arm = 1;
    hv_vcpus_exit(&g_vcpu, 1);
    return NULL;
}

static void st_begin(hv_vcpu_t vcpu)
{
    uint64_t md = 0, cpsr = 0;
    g_st_pc = calloc((size_t)g_st_n, sizeof(uint64_t));
    g_st_left = g_st_n;
    g_st_cnt = 0;
    g_st_active = 1;
    fprintf(stderr, "STEPTRACE BEGIN\n");
    hv_vcpu_set_trap_debug_exceptions(vcpu, true);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_MDSCR_EL1, &md);
    hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &cpsr);
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MDSCR_EL1, md | 1);
    hv_vcpu_set_reg(vcpu, HV_REG_CPSR, cpsr | (1ULL << 21));
}

static void st_step(hv_vcpu_t vcpu)
{
    uint64_t pc = 0, cpsr = 0;
    hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
    g_st_pc[g_st_cnt++] = pc;
    if (--g_st_left > 0) {
        hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &cpsr);
        hv_vcpu_set_reg(vcpu, HV_REG_CPSR, cpsr | (1ULL << 21));
        return;
    }
    uint64_t md = 0;
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_MDSCR_EL1, &md);
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MDSCR_EL1, md & ~1ULL);
    hv_vcpu_set_trap_debug_exceptions(vcpu, false);
    g_st_active = 0;
    { uint64_t ssp = 0, sp0v = 0; hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL1, &ssp); hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL0, &sp0v); fprintf(stderr, "STEPSP sp_el1=0x%llx sp=0x%llx\n", (unsigned long long)ssp, (unsigned long long)sp0v); uint64_t spp = sp0v ? sp0v : ssp; dump_guest_va(vcpu, "LOOPSTK", spp & ~0xfffULL, 0x1000, "/tmp/claude-501/loopstk.bin"); }
    dump_guest_va(vcpu, "LOOPA", g_st_pc[g_st_cnt - 1] & ~0xfffULL, 0x1000, "/tmp/claude-501/loopa.bin");
    dump_guest_va(vcpu, "LOOPB", (g_st_pc[g_st_cnt - 1] & ~0xfffULL) + 0x1000, 0x1000, "/tmp/claude-501/loopb.bin");
    dump_guest_va(vcpu, "LOOPC", (g_st_pc[g_st_cnt - 1] & ~0xfffULL) - 0x1000, 0x1000, "/tmp/claude-501/loopc.bin");
    fprintf(stderr, "STEPTRACE %d instructions:\n", g_st_cnt);
    uint64_t start = g_st_pc[0], prev = g_st_pc[0];
    for (int i = 1; i <= g_st_cnt; i++) {
        if (i == g_st_cnt || g_st_pc[i] != prev + 4) {
            fprintf(stderr, "  run 0x%llx..0x%llx (%llu insns)\n", (unsigned long long)start,
                    (unsigned long long)prev, (unsigned long long)((prev - start) / 4 + 1));
            if (i < g_st_cnt)
                start = g_st_pc[i];
        }
        if (i < g_st_cnt)
            prev = g_st_pc[i];
    }
}
