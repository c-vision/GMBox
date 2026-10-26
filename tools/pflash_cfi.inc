/*
 * P30/CFI NOR flash model for the EDK2 variable bank.
 *
 * Included by hvf_win.c after g_vars and the Hypervisor mappings exist.
 * Read-array is served directly from the read-only guest mapping; command,
 * status and CFI modes unmap the bank and are served here.
 */

#define PFLASH_BLOCK_SIZE (256UL * 1024UL)

static int pflash_unmapped;
static int pflash_mode;          /* 0=read array, 1=status, 2=CFI, 3=ID */
static int pflash_wcycle;
static uint8_t pflash_cmd;
static uint32_t pflash_status = 0x80;
static uint32_t pflash_counter;
static uint64_t pflash_prog_off;
static int pflash_dbg;

static void pflash_map_readonly(void)
{
    hv_return_t r = hv_vm_map(g_vars, VARS_IPA, VARS_SIZE, HV_MEMORY_READ);
    if (r != HV_SUCCESS)
        fprintf(stderr, "pflash: hv_vm_map failed 0x%x\n", (unsigned)r);
    pflash_unmapped = 0;
}

static void pflash_unmap(void)
{
    if (pflash_unmapped)
        return;
    hv_return_t r = hv_vm_unmap(VARS_IPA, VARS_SIZE);
    if (r != HV_SUCCESS)
        fprintf(stderr, "pflash: hv_vm_unmap failed 0x%x\n", (unsigned)r);
    pflash_unmapped = 1;
}

static uint64_t pflash_read(uint64_t off, int size)
{
    if (pflash_dbg < 30) { fprintf(stderr, "pflash read off=0x%llx size=%d mode=%d unmapped=%d\n", (unsigned long long)off, size, pflash_mode, pflash_unmapped); pflash_dbg++; }
    uint64_t v = 0;

    if (pflash_mode == 2) {
        uint8_t b = cfi_data[(off & 0x7f)];
        v = (uint64_t)b * 0x0101010101010101ULL;
    } else if (pflash_mode == 3) {
        v = (off & 4) ? 0x00010001ULL : 0x00010001ULL;
    } else {
        /* Status register (0x70), plus the default ready state. */
        v = 0x00800080ULL;
    }
    if (size == 1)
        v &= 0xff;
    else if (size == 2)
        v &= 0xffff;
    else if (size == 4)
        v &= 0xffffffffULL;
    return v;
}

static void pflash_write(uint64_t off, uint64_t value, int size)
{
    if (pflash_dbg < 30) { fprintf(stderr, "pflash write off=0x%llx value=0x%llx size=%d cyc=%d\n", (unsigned long long)off, (unsigned long long)value, size, pflash_wcycle); pflash_dbg++; }
    uint8_t byte = (uint8_t)value;

    if (size == 1)
        value &= 0xff;
    else if (size == 2)
        value &= 0xffff;
    else if (size == 4)
        value &= 0xffffffffULL;

    if (!pflash_unmapped)
        pflash_unmap();

    switch (pflash_wcycle) {
    case 0:
        switch (byte) {
        case 0x70:
            pflash_status |= 0x80;
            pflash_mode = 1;
            break;
        case 0x98:
            pflash_mode = 2;
            break;
        case 0x90:
            pflash_mode = 3;
            break;
        case 0xFF:
        case 0x00:
            pflash_mode = 0;
            pflash_wcycle = 0;
            pflash_map_readonly();
            break;
        case 0x50:
            pflash_status = 0;
            pflash_mode = 0;
            pflash_map_readonly();
            break;
        case 0x20:
        case 0x28:
            pflash_cmd = (uint8_t)byte;
            pflash_wcycle = 1;
            break;
        case 0xE8:
            pflash_cmd = 0xE8;
            pflash_wcycle = 1;
            break;
        case 0x10:
        case 0x40:
            pflash_cmd = (uint8_t)byte;
            pflash_wcycle = 1;
            break;
        default:
            break;
        }
        break;

    case 1:
        if (pflash_cmd == 0x20 || pflash_cmd == 0x28) {
            if (byte == 0xD0) {
                uint64_t base = off & ~(PFLASH_BLOCK_SIZE - 1);
                if (base + PFLASH_BLOCK_SIZE <= VARS_SIZE)
                    memset((uint8_t *)g_vars + base, 0xff, PFLASH_BLOCK_SIZE);
                pflash_status = 0x80;
                pflash_wcycle = 0;
                pflash_mode = 0;
                pflash_map_readonly();
            } else if (byte == 0xFF) {
                pflash_wcycle = 0;
                pflash_mode = 0;
                pflash_map_readonly();
            }
        } else if (pflash_cmd == 0xE8) {
            pflash_counter = (uint32_t)(value & 0xffffffffu);
            pflash_prog_off = off;
            pflash_wcycle = 2;
        } else if (pflash_cmd == 0x10 || pflash_cmd == 0x40) {
            if (off < VARS_SIZE)
                memcpy((uint8_t *)g_vars + off, &value, (size_t)size);
            pflash_status |= 0x80;
            pflash_wcycle = 0;
            pflash_mode = 0;
            pflash_map_readonly();
        }
        break;

    case 2:
        if (pflash_cmd == 0xE8) {
            if (pflash_counter && off < VARS_SIZE) {
                memcpy((uint8_t *)g_vars + off, &value, (size_t)size);
                pflash_prog_off = off + (uint64_t)size;
                pflash_counter--;
            }
            pflash_status |= 0x80;
            if (pflash_counter == 0)
                pflash_wcycle = 3;
        }
        break;

    case 3:
        if (pflash_cmd == 0xE8 && byte == 0xD0) {
            pflash_wcycle = 0;
            pflash_mode = 0;
            pflash_status = 0x80;
            pflash_map_readonly();
        } else {
            pflash_wcycle = 0;
            pflash_mode = 0;
            pflash_map_readonly();
        }
        break;

    default:
        pflash_wcycle = 0;
        pflash_mode = 0;
        pflash_map_readonly();
        break;
    }
}
