/*
 * ACPI table generator for the GMBox native HVF VMM (C99).
 *
 * Produces the three fw_cfg blobs that EDK2's AcpiPlatformDxe expects:
 *   tools/acpi_tables.bin   (etc/acpi/tables: XSDT FADT MADT GTDT MCFG SPCR DSDT)
 *   tools/acpi_rsdp.bin     (etc/acpi/rsdp)
 *   tools/acpi_loader.bin   (etc/table-loader: ALLOCATE / ADD_POINTER / ADD_CHECKSUM)
 *
 * Platform map must match tools/hvf_win.c and the DTB:
 *   GICD 0x08000000, GICR 0x080A0000, PL011 0x09000000 (SPI 1), ECAM 0x3f000000.
 * Run via `make acpi` (needs tools/dsdt.aml from iasl).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GICD_BASE   0x08000000ULL
#define GICR_BASE   0x080A0000ULL
#define GICR_LEN    0x00020000U
#define UART_BASE   0x09000000ULL
#define UART_GSIV   33U
#define ECAM_BASE   0x3F000000ULL
#define ECAM_LAST_BUS 0x0F

#define TABLES_MAX  (64 * 1024)
#define ENTRY_SIZE  128
#define MAX_TABLES  8

static uint8_t tables[TABLES_MAX];
static uint32_t tables_len;
static uint8_t rsdp[36];
static uint8_t loader[48 * ENTRY_SIZE];
static int ncmd;

struct tbl { const char *sig; uint32_t off, len; };
static struct tbl tbls[MAX_TABLES];
static int ntbl;

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

/* Reserve a table (8-byte aligned) and fill the standard header. */
static uint8_t *new_table(const char *sig, uint32_t len, uint8_t rev)
{
    uint32_t off = (tables_len + 7u) & ~7u;
    if (off + len > TABLES_MAX || ntbl >= MAX_TABLES) {
        fprintf(stderr, "acpi_build: table space exhausted\n");
        exit(1);
    }
    uint8_t *p = tables + off;
    memcpy(p, sig, 4);
    put32(p + 4, len);
    p[8] = rev;
    memcpy(p + 10, "GMBOX ", 6);
    memcpy(p + 16, "GMBOXACP", 8);
    put32(p + 24, 1);
    memcpy(p + 28, "GMBX", 4);
    put32(p + 32, 1);
    tbls[ntbl].sig = sig;
    tbls[ntbl].off = off;
    tbls[ntbl].len = len;
    ntbl++;
    tables_len = off + len;
    return p;
}

static void cmd_allocate(const char *file, uint32_t align)
{
    uint8_t *e = loader + ncmd++ * ENTRY_SIZE;
    memset(e, 0, ENTRY_SIZE);
    put32(e, 1);
    strncpy((char *)e + 4, file, 55);
    put32(e + 60, align);
    e[64] = 1; /* zone: HIGH */
}

static void cmd_pointer(const char *dst, const char *src, uint32_t off, uint8_t size)
{
    uint8_t *e = loader + ncmd++ * ENTRY_SIZE;
    memset(e, 0, ENTRY_SIZE);
    put32(e, 2);
    strncpy((char *)e + 4, dst, 55);
    strncpy((char *)e + 60, src, 55);
    put32(e + 116, off);
    e[120] = size;
}

static void cmd_checksum(const char *file, uint32_t result_off, uint32_t start, uint32_t len)
{
    uint8_t *e = loader + ncmd++ * ENTRY_SIZE;
    memset(e, 0, ENTRY_SIZE);
    put32(e, 3);
    strncpy((char *)e + 4, file, 55);
    put32(e + 60, result_off);
    put32(e + 64, start);
    put32(e + 68, len);
}

static void gas(uint8_t *p, uint8_t space, uint8_t width, uint8_t access, uint64_t addr)
{
    p[0] = space;
    p[1] = width;
    p[2] = 0;
    p[3] = access;
    put64(p + 4, addr);
}

int main(void)
{
    FILE *f = fopen("tools/dsdt.aml", "rb");
    if (!f) {
        perror("tools/dsdt.aml (run: iasl -p tools/dsdt tools/dsdt.asl)");
        return 1;
    }
    static uint8_t dsdt[TABLES_MAX / 2];
    size_t dsdt_len = fread(dsdt, 1, sizeof(dsdt), f);
    fclose(f);
    if (dsdt_len < 36)
        return 1;

    /* XSDT is first; entries are filled once the other tables exist. */
    uint8_t *xsdt = new_table("XSDT", 36 + 5 * 8, 1);

    /* FADT, ACPI 6.3 layout (276 bytes), hardware-reduced, PSCI via HVC. */
    uint8_t *fadt = new_table("FACP", 276, 6);
    put32(fadt + 112, (1u << 20) | (1u << 4)); /* HW_REDUCED_ACPI | PWR_BUTTON */
    put16(fadt + 129, 0x0003);                 /* ARM_BOOT_ARCH: PSCI_COMPLIANT | USE_HVC */
    fadt[131] = 3;                             /* FADT minor version */
    memcpy(fadt + 268, "GMBOX   ", 8);         /* hypervisor vendor id */

    /* MADT rev 4: 1 GICC, GICD (v3), 1 GICR region. */
    uint8_t *madt = new_table("APIC", 44 + 80 + 24 + 16, 4);
    uint8_t *g = madt + 44;
    g[0] = 0x0B; g[1] = 80;                    /* GICC */
    put32(g + 4, 0);                           /* CPU interface number */
    put32(g + 8, 0);                           /* ACPI processor UID */
    put32(g + 12, 1);                          /* flags: enabled */
    put64(g + 68, 0);                          /* MPIDR */
    g += 80;
    g[0] = 0x0C; g[1] = 24;                    /* GICD */
    put64(g + 8, GICD_BASE);
    g[20] = 3;                                 /* GIC version 3 */
    g += 24;
    g[0] = 0x0E; g[1] = 16;                    /* GICR */
    put64(g + 4, GICR_BASE);
    put32(g + 12, GICR_LEN);

    /* GTDT rev 3: architected timer PPIs 13/14/11/10, always-on. */
    uint8_t *gtdt = new_table("GTDT", 104, 3);
    put64(gtdt + 36, ~0ULL);                   /* CntControlBase: none */
    put32(gtdt + 48, 29); put32(gtdt + 52, 4); /* secure EL1 */
    put32(gtdt + 56, 30); put32(gtdt + 60, 4); /* non-secure EL1 */
    put32(gtdt + 64, 27); put32(gtdt + 68, 4); /* virtual */
    put32(gtdt + 72, 26); put32(gtdt + 76, 4); /* non-secure EL2 */
    put64(gtdt + 80, ~0ULL);                   /* CntReadBase: none */
    put32(gtdt + 96, 28); put32(gtdt + 100, 4);/* virtual EL2 */

    /* MCFG: one ECAM segment. */
    uint8_t *mcfg = new_table("MCFG", 60, 1);
    put64(mcfg + 44, ECAM_BASE);
    put16(mcfg + 52, 0);
    mcfg[54] = 0;
    mcfg[55] = ECAM_LAST_BUS;

    /* SPCR rev 2: PL011 console. */
    uint8_t *spcr = new_table("SPCR", 80, 2);
    spcr[36] = 3;                              /* ARM PL011 UART */
    gas(spcr + 40, 0, 32, 3, UART_BASE);
    spcr[52] = 0x08;                           /* interrupt type: GIC */
    put32(spcr + 54, UART_GSIV);
    spcr[58] = 7;                              /* 115200 */
    spcr[60] = 1;                              /* 1 stop bit */
    spcr[62] = 2;                              /* VT-UTF8 terminal */
    put16(spcr + 64, 0xFFFF);                  /* no PCI device */
    put16(spcr + 66, 0xFFFF);

    /* DSDT (already a complete AML table from iasl). */
    uint32_t dsdt_off = (tables_len + 7u) & ~7u;
    if (dsdt_off + dsdt_len > TABLES_MAX)
        return 1;
    memcpy(tables + dsdt_off, dsdt, dsdt_len);
    tbls[ntbl].sig = "DSDT";
    tbls[ntbl].off = dsdt_off;
    tbls[ntbl].len = (uint32_t)dsdt_len;
    ntbl++;
    tables_len = dsdt_off + (uint32_t)dsdt_len;

    /* Pointers stored as offsets inside the blob; the loader adds the base. */
    put64(fadt + 140, dsdt_off);               /* X_DSDT */
    int xi = 0;
    for (int i = 0; i < ntbl; i++) {
        if (!strcmp(tbls[i].sig, "XSDT") || !strcmp(tbls[i].sig, "DSDT"))
            continue;
        put64(xsdt + 36 + 8 * xi, tbls[i].off);
        xi++;
    }

    memcpy(rsdp, "RSD PTR ", 8);
    memcpy(rsdp + 9, "GMBOX ", 6);
    rsdp[15] = 2;
    put32(rsdp + 20, 36);

    /* Loader script. */
    cmd_allocate("etc/acpi/tables", 64);
    cmd_allocate("etc/acpi/rsdp", 16);
    cmd_pointer("etc/acpi/rsdp", "etc/acpi/tables", 24, 8);      /* XSDT address */
    for (int i = 0; i < xi; i++)
        cmd_pointer("etc/acpi/tables", "etc/acpi/tables", 36 + 8 * (uint32_t)i, 8);
    cmd_pointer("etc/acpi/tables", "etc/acpi/tables", (uint32_t)(fadt - tables) + 140, 8);

    for (int i = 0; i < ntbl; i++) {
        uint8_t *t = tables + tbls[i].off;
        t[9] = 0;
        cmd_checksum("etc/acpi/tables", tbls[i].off + 9, tbls[i].off, tbls[i].len);
    }
    cmd_checksum("etc/acpi/rsdp", 8, 0, 20);
    cmd_checksum("etc/acpi/rsdp", 32, 0, 36);

    FILE *o = fopen("tools/acpi_tables.bin", "wb");
    fwrite(tables, 1, tables_len, o);
    fclose(o);
    o = fopen("tools/acpi_rsdp.bin", "wb");
    fwrite(rsdp, 1, sizeof(rsdp), o);
    fclose(o);
    o = fopen("tools/acpi_loader.bin", "wb");
    fwrite(loader, 1, (size_t)ncmd * ENTRY_SIZE, o);
    fclose(o);
    printf("acpi: %d tables, blob=%u bytes, dsdt=%zu, loader_cmds=%d\n",
           ntbl, tables_len, dsdt_len, ncmd);
    return 0;
}
