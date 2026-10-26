/*
 * UEFI call tracer for the guest boot application (bootmgfw.efi).
 *
 * Enabled with GMBOX_TRACE_BM=1. When the UART prints "BdsDxe: starting",
 * the vCPU is put in single-step; every transition between the PE image and
 * the firmware (a UEFI service call) is logged with its arguments and the
 * returned EFI_STATUS. C99, no external deps.
 */
static int g_trace_on, g_trace_arm, g_trace_run;
static uint64_t tr_base, tr_end, tr_vbar, tr_last_lr, tr_exc_ret;
static uint64_t tr_systab, tr_bs;
static int tr_prev_in, tr_depth_ret;
static unsigned long tr_steps, tr_events;
static char tr_uart_tail[32];

static const char *bs_name(uint64_t off)
{
    static const char *n[] = {
        "RaiseTPL","RestoreTPL","AllocatePages","FreePages","GetMemoryMap",
        "AllocatePool","FreePool","CreateEvent","SetTimer","WaitForEvent",
        "SignalEvent","CloseEvent","CheckEvent","InstallProtocolInterface",
        "ReinstallProtocolInterface","UninstallProtocolInterface",
        "HandleProtocol","Reserved","RegisterProtocolNotify","LocateHandle",
        "LocateDevicePath","InstallConfigurationTable","LoadImage","StartImage",
        "Exit","UnloadImage","ExitBootServices","GetNextMonotonicCount","Stall",
        "SetWatchdogTimer","ConnectController","DisconnectController",
        "OpenProtocol","CloseProtocol","OpenProtocolInformation",
        "ProtocolsPerHandle","LocateHandleBuffer","LocateProtocol",
        "InstallMultipleProtocolInterfaces","UninstallMultipleProtocolInterfaces",
        "CalculateCrc32","CopyMem","SetMem","CreateEventEx" };
    if (off < 24 || ((off - 24) & 7))
        return NULL;
    size_t i = (off - 24) / 8;
    return i < sizeof(n) / sizeof(n[0]) ? n[i] : NULL;
}

static const char *tr_bs_lookup(uint64_t pc)
{
    if (!tr_bs)
        return NULL;
    const uint64_t *t = gpa(tr_bs, 24 + 8 * 44);
    if (!t)
        return NULL;
    for (size_t i = 3; i < 3 + 44; i++)
        if (t[i] == pc)
            return bs_name(i * 8);
    return NULL;
}

static void tr_str(uint64_t a, char *out, size_t n)
{
    out[0] = 0;
    if (a < RAM_IPA || a >= RAM_IPA + RAM_SIZE - 128)
        return;
    const uint16_t *w = gpa(a, 128);
    size_t i = 0;
    if (!w || w[0] < 0x20 || w[0] > 0x7e)
        return;
    for (; i + 1 < n && i < 120 && ((w[i] >= 0x20 && w[i] < 0x7f) || w[i] == 10 || w[i] == 13); i++)
        out[i] = w[i] < 0x20 ? (char)0x7e : (char)w[i];
    if (i < 2 || (w[i] != 0 && i < 120)) { out[0] = 0; return; }
    out[i] = 0;
}

static void tr_guid(uint64_t a, char *out, size_t n)
{
    out[0] = 0;
    if (a < RAM_IPA || a >= RAM_IPA + RAM_SIZE - 16)
        return;
    const uint8_t *g = gpa(a, 16);
    if (!g)
        return;
    uint32_t d1; uint16_t d2, d3;
    memcpy(&d1, g, 4); memcpy(&d2, g + 4, 2); memcpy(&d3, g + 6, 2);
    snprintf(out, n, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             d1, d2, d3, g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static int tr_find_image(void)
{
    const uint8_t *r = g_ram;
    const char *env = getenv("GMBOX_TRACE_BASE");
    uint64_t base = 0;
    if (env && *env) {
        base = strtoull(env, NULL, 0);
    } else {
        uint8_t *hit = memmem(g_ram, RAM_SIZE, "bootmgfw", 8);
        if (!hit)
            return -1;
        uint64_t off = (uint64_t)(hit - r) & ~0xfffULL;
        for (; off > 0; off -= 0x1000) {
            if (r[off] == 'M' && r[off + 1] == 'Z') {
                uint32_t pe = *(const uint32_t *)(r + off + 0x3c);
                if (pe < 0x400 && memcmp(r + off + pe, "PE\0\0", 4) == 0)
                    break;
            }
        }
        if (!off)
            return -1;
        base = RAM_IPA + off;
    }
    const uint8_t *h = gpa(base, 0x400);
    if (!h || h[0] != 'M')
        return -1;
    uint32_t pe = *(const uint32_t *)(h + 0x3c);
    uint32_t soi = *(const uint32_t *)(h + pe + 24 + 56);
    uint32_t ep = *(const uint32_t *)(h + pe + 24 + 16);
    tr_base = base;
    tr_end = base + soi;
    if (getenv("GMBOX_DUMP_IMAGE")) {
        FILE *f = fopen(getenv("GMBOX_DUMP_IMAGE"), "wb");
        if (f) { fwrite(gpa(base, soi), 1, soi, f); fclose(f); }
    }
    fprintf(stderr, "TRACE image base=0x%llx size=0x%x entry=RVA 0x%x\n",
            (unsigned long long)base, soi, ep);
    return 0;
}

static void tr_begin(hv_vcpu_t vcpu)
{
    if (tr_find_image() < 0) {
        fprintf(stderr, "TRACE: image not found\n");
        return;
    }
    uint64_t md = 0, cpsr = 0, x1 = 0;
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, &tr_vbar);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_MDSCR_EL1, &md);
    hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &cpsr);
    hv_vcpu_get_reg(vcpu, HV_REG_X1, &x1);
    (void)x1;
    hv_vcpu_set_trap_debug_exceptions(vcpu, true);
    hv_vcpu_set_trap_debug_reg_accesses(vcpu, true);
    {
        const uint8_t *h = gpa(tr_base, 0x400);
        uint32_t pe = *(const uint32_t *)(h + 0x3c);
        uint32_t ep = *(const uint32_t *)(h + pe + 24 + 16);
        hv_return_t r1 = hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_DBGBVR0_EL1, tr_base + ep);
        hv_return_t r2 = hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_DBGBCR0_EL1, 0x1e7);
        hv_return_t r3 = hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MDSCR_EL1, md | (1ULL << 15));
        fprintf(stderr, "TRACE bp entry=0x%llx r=%x,%x,%x\n", (unsigned long long)(tr_base + ep), r1, r2, r3);
    }
    (void)cpsr;
    g_trace_run = 1;
    tr_prev_in = 0;
    fprintf(stderr, "TRACE start vbar=0x%llx\n", (unsigned long long)tr_vbar);
}

static void tr_regs(hv_vcpu_t vcpu, uint64_t *x)
{
    for (int i = 0; i < 6; i++)
        hv_vcpu_get_reg(vcpu, (hv_reg_t)(HV_REG_X0 + i), &x[i]);
}

static void tr_bp_hit(hv_vcpu_t vcpu)
{
    uint64_t md = 0, cpsr = 0, pc = 0;
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_MDSCR_EL1, &md);
    hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &cpsr);
    hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_DBGBCR0_EL1, 0);
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MDSCR_EL1, md | 1);
    hv_vcpu_set_reg(vcpu, HV_REG_CPSR, cpsr | (1ULL << 21));
    {
        uint64_t st = 0; hv_vcpu_get_reg(vcpu, HV_REG_X1, &st);
        const uint64_t *s = gpa(st, 120);
        if (s) tr_bs = s[12];
        fprintf(stderr, "TRACE systab=0x%llx bs=0x%llx\n", (unsigned long long)st, (unsigned long long)tr_bs);
        if (s) {
            uint64_t n = s[13], ct = s[14];
            const uint8_t *e = gpa(ct, n * 24);
            for (uint64_t i = 0; e && i < n && i < 32; i++) {
                char gs[48]; tr_guid(ct + i * 24, gs, sizeof gs);
                fprintf(stderr, "TRACE conftable[%llu] %s ptr=0x%llx\n", (unsigned long long)i, gs, (unsigned long long)*(const uint64_t *)(e + i * 24 + 16));
            }
        }
    }
    fprintf(stderr, "TRACE entry reached pc=0x%llx; single-stepping\n", (unsigned long long)pc);
}

/* called on every software-step exit */
static void tr_step(hv_vcpu_t vcpu)
{
    uint64_t pc = 0, cpsr = 0, x[6], lr = 0;
    hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
    hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &cpsr);
    int in = pc >= tr_base && pc < tr_end;
    tr_steps++;
    if (tr_prev_in && !in) {
        if (pc - tr_vbar < 0x800) {
            tr_exc_ret = 1;           /* exception taken; ignore the round trip */
        } else {
            tr_regs(vcpu, x);
            hv_vcpu_get_reg(vcpu, HV_REG_LR, &lr);
            const char *nm = tr_bs_lookup(pc);
            char s1[130], s2[130], s0[130], g1[48], g2[48];
            tr_str(x[0], s0, sizeof s0);
            tr_str(x[1], s1, sizeof s1);
            tr_str(x[2], s2, sizeof s2);
            tr_guid(x[0], g1, sizeof g1);
            tr_guid(x[1], g2, sizeof g2);
            fprintf(stderr, "TRACE #%lu CALL rva=%llx -> %s%s pc=%llx x0=%llx x1=%llx x2=%llx x3=%llx",
                    tr_events++, (unsigned long long)(lr - tr_base),
                    nm ? "BS." : "", nm ? nm : "?",
                    (unsigned long long)pc, (unsigned long long)x[0],
                    (unsigned long long)x[1], (unsigned long long)x[2],
                    (unsigned long long)x[3]);
            if (x[3] >= RAM_IPA && x[3] < RAM_IPA + RAM_SIZE - 16) { const uint64_t *d = gpa(x[3], 16); if (d) fprintf(stderr, " [x3]=%llx", (unsigned long long)d[0]); }
            if (x[2] >= RAM_IPA && x[2] < RAM_IPA + RAM_SIZE - 16) { const uint64_t *d = gpa(x[2], 16); if (d) fprintf(stderr, " [x2]=%llx", (unsigned long long)d[0]); }
            if (g1[0]) fprintf(stderr, " guid0=%s", g1);
            if (g2[0]) fprintf(stderr, " guid1=%s", g2);
            if (s0[0]) fprintf(stderr, " str0=\"%s\"", s0);
            if (s1[0]) fprintf(stderr, " str1=\"%s\"", s1);
            if (s2[0]) fprintf(stderr, " str2=\"%s\"", s2);
            fprintf(stderr, "\n");
            tr_last_lr = lr;
        }
    } else if (!tr_prev_in && in) {
        if (tr_exc_ret) {
            tr_exc_ret = 0;
        } else {
            hv_vcpu_get_reg(vcpu, HV_REG_X0, &x[0]);
            fprintf(stderr, "TRACE      RET rva=%llx x0=%llx%s\n",
                    (unsigned long long)(pc - tr_base),
                    (unsigned long long)x[0],
                    pc == tr_last_lr ? "" : " (callback/enter)");
        }
    }
    tr_prev_in = in;
    hv_vcpu_set_reg(vcpu, HV_REG_CPSR, cpsr | (1ULL << 21));
}

static void tr_uart_char(char c)
{
    if (!g_trace_on || g_trace_run || g_trace_arm)
        return;
    memmove(tr_uart_tail, tr_uart_tail + 1, sizeof(tr_uart_tail) - 2);
    tr_uart_tail[sizeof(tr_uart_tail) - 2] = c;
    if (strstr(tr_uart_tail, "BdsDxe: starting"))
        g_trace_arm = 1;
}
