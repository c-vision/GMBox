#define _DARWIN_C_SOURCE
#include "vmfc.h"
#include "util.h"
#include "vmdk.h"
#include "vz.h"
#include "qemu.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static int cmd_start(int argc, char **argv, const char *root);

static void usage(FILE *out)
{
    fprintf(out,
            "GMBox %s - macchine virtuali Windows/Linux su Apple silicon\n"
            "\n"
            "Uso:\n"
            "  vmfc list   [--dir CARTELLA] [--filter TESTO] [--tag TAG]\n"
            "  vmfc create [NOME] [--wizard] [opzioni]\n"
            "  vmfc start  NOME [opzioni]\n"
            "  vmfc gui    NOME [opzioni]\n"
            "  vmfc stop   NOME [--force] [--dir CARTELLA]\n"
            "  vmfc pause  NOME [--dir CARTELLA]\n"
            "  vmfc resume NOME [--dir CARTELLA]\n"
            "  vmfc reboot NOME [--dir CARTELLA]\n"
            "  vmfc status NOME [--dir CARTELLA]\n"
            "  vmfc info   NOME [--dir CARTELLA]\n"
            "  vmfc tag    NOME [TAG...] [--dir CARTELLA]\n"
            "  vmfc delete NOME [--dir CARTELLA] [--force]\n"
            "  vmfc import SORGENTE [opzioni]\n"
            "  vmfc autostart NOME [--disable|--status] [--dry-run] [--dir CARTELLA]\n"
            "  vmfc tools\n"
            "  vmfc version\n"
            "\n"
            "Opzioni di create:\n"
            "  --cpus N          numero di CPU virtuali (default %u)\n"
            "  --memory SIZE     memoria (default 4096M; es. 8G)\n"
            "  --disk SIZE       dimensione disco sparso (default 64G)\n"
            "  --iso PERCORSO    immagine ISO di installazione\n"
            "  --drivers PERCORSO seconda ISO con i driver virtio (Windows)\n"
            "  --shared PERCORSO cartella condivisa (virtiofs)\n"
            "  --readonly        cartella condivisa in sola lettura\n"
            "  --guest STRINGA   guestOS VMware (default %s)\n"
            "  --dir CARTELLA    cartella delle VM (default ~/Documents/Virtual Machines)\n"
            "  --start           avvia subito la VM\n"
            "\n"
            "Opzioni di start/gui:\n"
            "  --headless        nessuna finestra (arresta con SIGTERM/SIGINT)\n"
            "  --seconds N       arresta automaticamente dopo N secondi\n"
            "  --iso PERCORSO    imposta la ISO prima dell'avvio\n"
            "  --drivers PERCORSO imposta la ISO driver prima dell'avvio\n"
            "  --no-drivers      scollega la seconda ISO\n"
            "  --engine vz|qemu  forza il motore (default: QEMU/HVF per Windows, VZ per Linux)\n"
            "  --dir CARTELLA    cartella delle VM\n"
            "\n"
            "Opzioni di import (VMware .vmwarevm, .vmx o .vmdk):\n"
            "  --name NOME       nome della nuova VM\n"
            "  --memory SIZE     sovrascrive la RAM\n"
            "  --cpus N          sovrascrive il numero di CPU\n"
            "  --guest STRINGA   sovrascrive guestOS\n"
            "  --dir CARTELLA    cartella delle VM\n"
            "  --start           avvia la VM dopo la conversione\n"
            "  --dry-run         analizza la sorgente senza convertire\n"
            "\n"
            "Le VM Windows richiedono Windows 11 ARM64. Il disco e' un file raw sparso\n"
            "collegato come NVMe. Windows usa QEMU/HVF (qemu-system-aarch64 + EDK2),\n"
            "Linux usa VZ/Virtualization.framework; --engine forza la scelta.\n",
            VMFC_VERSION, VMFC_DEFAULT_CPUS, VMFC_DEFAULT_GUEST);
}

static const char *arg_value(int argc, char **argv, const char *name, int *i, int *err)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "vmfc: opzione %s senza valore\n", name);
        *err = 1;
        return NULL;
    }
    (*i)++;
    return argv[*i];
}

static void default_drivers_iso(char *out, size_t n)
{
    const char *home = getenv("HOME");
    out[0] = 0;
    if (!home || !*home)
        return;
    snprintf(out, n, "%s/Library/Application Support/GMBox/utm-guest-tools.iso", home);
}

static int make_absolute_path(const char *in, char *out, size_t n)
{
    char resolved[PATH_MAX];
    if (!realpath(in, resolved)) {
        fprintf(stderr, "vmfc: percorso non valido %s: %s\n", in, strerror(errno));
        return -1;
    }
    snprintf(out, n, "%s", resolved);
    return 0;
}

struct list_filter {
    int *any;
    const char *filter;
    const char *tag;
};

static int contains_ci(const char *hay, const char *needle)
{
    if (!needle || !*needle)
        return 1;
    if (!hay)
        return 0;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (!strncasecmp(p, needle, n))
            return 1;
    return 0;
}

static int has_tag(const char *tags, const char *want)
{
    if (!want || !*want)
        return 1;
    const char *p = tags;
    while (p && *p) {
        while (*p == ' ' || *p == ',') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        size_t n = (size_t)(e - p);
        while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
        if (n == strlen(want) && !strncasecmp(p, want, n))
            return 1;
        p = *e ? e + 1 : e;
    }
    return 0;
}

static int list_row(const vm_config *c, void *user)
{
    struct list_filter *f = user;
    if (!contains_ci(c->name, f->filter) && !contains_ci(c->bundle, f->filter) &&
        !contains_ci(c->guest_os, f->filter) && !contains_ci(c->tags, f->filter))
        return 0;
    if (!has_tag(c->tags, f->tag))
        return 0;
    char mem[64], disk[64], last[32] = "-";
    pid_t pid;
    struct stat sb;
    format_gib(c->memory_mib, mem, sizeof(mem));
    format_gib(c->disk_mib, disk, sizeof(disk));
    if (stat(c->logfile, &sb) == 0) {
        struct tm tmv;
        if (localtime_r(&sb.st_mtime, &tmv))
            strftime(last, sizeof(last), "%Y-%m-%d %H:%M", &tmv);
    }
    *f->any = 1;
    printf("%-28s %-14s %2u CPU  %-8s %-8s %-16s %s%s%s\n", c->name,
           vm_running(c, &pid) ? "in esecuzione" : "spenta",
           c->cpus, mem, disk, last, c->bundle,
           c->tags[0] ? "  tag=" : "", c->tags);
    return 0;
}

static int cmd_list(const char *root, const char *filter, const char *tag)
{
    int any = 0;
    struct list_filter f = { &any, filter, tag };
    if (vm_list(root, list_row, &f) != 0) {
        fprintf(stderr, "vmfc: impossibile leggere %s\n", root);
        return 1;
    }
    if (!any) {
        printf("Nessuna macchina virtuale in %s\n", root);
        if (filter || tag)
            printf("Nessun risultato per i filtri richiesti.\n");
        else
            printf("Crea la prima con: vmfc create \"Windows 11\" --iso /percorso/Windows11.iso --start\n");
    }
    return 0;
}

static void config_paths(vm_config *cfg, const char *root)
{
    snprintf(cfg->root, sizeof(cfg->root), "%s", root);
    vm_bundle_path(root, cfg->name, cfg->bundle, sizeof(cfg->bundle));
}

static int prompt_line(const char *label, const char *def, char *out, size_t n)
{
    char line[PATH_MAX];
    printf("%s", label);
    if (def && *def)
        printf(" [%s]", def);
    printf(": ");
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin))
        return -1;
    char *v = str_trim(line);
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", *v ? v : (def ? def : ""));
    snprintf(out, n, "%s", tmp);
    return 0;
}

static int wizard_fill(vm_config *cfg, char *iso, size_t iso_n,
                       char *shared, size_t shared_n, int *start_now)
{
    char line[PATH_MAX], choice[16];
    if (!cfg->name[0] &&
        prompt_line("Nome VM", "Windows 11", cfg->name, sizeof(cfg->name)) != 0)
        return -1;
    if (!vm_name_valid(cfg->name)) {
        fprintf(stderr, "vmfc: nome VM non valido\n");
        return -1;
    }
    if (prompt_line("Guest OS", cfg->guest_os, cfg->guest_os, sizeof(cfg->guest_os)) != 0)
        return -1;
    snprintf(line, sizeof(line), "%u", cfg->cpus);
    if (prompt_line("CPU", line, line, sizeof(line)) != 0 ||
        parse_u32(line, &cfg->cpus) != 0 || cfg->cpus < 1 || cfg->cpus > 64) {
        fprintf(stderr, "vmfc: numero CPU non valido\n");
        return -1;
    }
    format_gib(cfg->memory_mib, line, sizeof(line));
    if (prompt_line("Memoria", line, line, sizeof(line)) != 0 ||
        parse_size_mib(line, &cfg->memory_mib) != 0 || cfg->memory_mib < 512) {
        fprintf(stderr, "vmfc: memoria non valida (minimo 512 MiB)\n");
        return -1;
    }
    format_gib(cfg->disk_mib, line, sizeof(line));
    if (prompt_line("Disco", line, line, sizeof(line)) != 0 ||
        parse_size_mib(line, &cfg->disk_mib) != 0 || cfg->disk_mib < 1024) {
        fprintf(stderr, "vmfc: disco non valido (minimo 1 GB)\n");
        return -1;
    }
    if (prompt_line("ISO (vuoto per nessuna)", iso, iso, iso_n) != 0)
        return -1;
    if (prompt_line("Cartella condivisa (vuoto per nessuna)", shared, shared, shared_n) != 0)
        return -1;
    if (prompt_line("Avviare ora? (s/N)", *start_now ? "s" : "N", choice, sizeof(choice)) != 0)
        return -1;
    *start_now = str_ieq(choice, "s") || str_ieq(choice, "si") || str_ieq(choice, "y") ||
                 str_ieq(choice, "yes");
    return 0;
}

static int cmd_create(int argc, char **argv, const char *root)
{
    const char *name = NULL;
    const char *iso = NULL;
    const char *drivers = NULL;
    const char *shared = NULL;
    const char *guest = VMFC_DEFAULT_GUEST;
    int readonly = 0;
    int start_now = 0;
    int wizard = 0;
    int err = 0;
    vm_config cfg;

    vm_config_init(&cfg);
    for (int i = 0; i < argc && !err; i++) {
        if (!strcmp(argv[i], "--cpus")) {
            const char *v = arg_value(argc, argv, "--cpus", &i, &err);
            uint32_t n;
            if (v && (parse_u32(v, &n) != 0 || n < 1 || n > 64)) {
                fprintf(stderr, "vmfc: --cpus non valido\n");
                err = 1;
            } else if (v) {
                cfg.cpus = n;
            }
        } else if (!strcmp(argv[i], "--memory")) {
            const char *v = arg_value(argc, argv, "--memory", &i, &err);
            uint64_t mib;
            if (v && parse_size_mib(v, &mib) != 0) {
                fprintf(stderr, "vmfc: --memory non valido\n");
                err = 1;
            } else if (v) {
                cfg.memory_mib = mib;
            }
        } else if (!strcmp(argv[i], "--disk")) {
            const char *v = arg_value(argc, argv, "--disk", &i, &err);
            uint64_t mib;
            if (v && parse_size_mib(v, &mib) != 0) {
                fprintf(stderr, "vmfc: --disk non valido\n");
                err = 1;
            } else if (v) {
                cfg.disk_mib = mib;
            }
        } else if (!strcmp(argv[i], "--iso")) {
            iso = arg_value(argc, argv, "--iso", &i, &err);
        } else if (!strcmp(argv[i], "--drivers")) {
            drivers = arg_value(argc, argv, "--drivers", &i, &err);
        } else if (!strcmp(argv[i], "--shared")) {
            shared = arg_value(argc, argv, "--shared", &i, &err);
        } else if (!strcmp(argv[i], "--guest")) {
            guest = arg_value(argc, argv, "--guest", &i, &err);
        } else if (!strcmp(argv[i], "--readonly")) {
            readonly = 1;
        } else if (!strcmp(argv[i], "--start")) {
            start_now = 1;
        } else if (!strcmp(argv[i], "--wizard")) {
            wizard = 1;
        } else if (!strcmp(argv[i], "--dir")) {
            root = arg_value(argc, argv, "--dir", &i, &err);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "vmfc: opzione sconosciuta %s\n", argv[i]);
            err = 1;
        } else if (!name) {
            name = argv[i];
        } else {
            fprintf(stderr, "vmfc: argomento inatteso %s\n", argv[i]);
            err = 1;
        }
    }
    if (err)
        return 2;

    char iso_buf[PATH_MAX], shared_buf[PATH_MAX];
    if (iso)
        snprintf(iso_buf, sizeof(iso_buf), "%s", iso);
    if (shared)
        snprintf(shared_buf, sizeof(shared_buf), "%s", shared);

    if (name)
        snprintf(cfg.name, sizeof(cfg.name), "%s", name);
    if (guest)
        snprintf(cfg.guest_os, sizeof(cfg.guest_os), "%s", guest);

    if (wizard) {
        if (wizard_fill(&cfg, iso_buf, sizeof(iso_buf),
                        shared_buf, sizeof(shared_buf), &start_now) != 0)
            return 2;
        iso = iso_buf[0] ? iso_buf : NULL;
        shared = shared_buf[0] ? shared_buf : NULL;
        name = cfg.name;
        guest = cfg.guest_os;
    }

    if (!name || !vm_name_valid(name)) {
        fprintf(stderr, "vmfc: nome VM mancante o non valido\n");
        return 2;
    }
    if (cfg.disk_mib < 1024) {
        fprintf(stderr, "vmfc: il disco deve essere di almeno 1 GB\n");
        return 2;
    }

    snprintf(cfg.name, sizeof(cfg.name), "%s", name);
    snprintf(cfg.guest_os, sizeof(cfg.guest_os), "%s", guest);
    config_paths(&cfg, root);

    if (iso && make_absolute_path(iso, cfg.iso, sizeof(cfg.iso)) != 0)
        return 2;
    if (drivers && make_absolute_path(drivers, cfg.drivers, sizeof(cfg.drivers)) != 0)
        return 2;
    if (!drivers && str_starts_with(cfg.guest_os, "arm-windows")) {
        char p[PATH_MAX];
        default_drivers_iso(p, sizeof(p));
        if (path_exists(p))
            snprintf(cfg.drivers, sizeof(cfg.drivers), "%s", p);
    }
    if (shared) {
        if (make_absolute_path(shared, cfg.shared_dir, sizeof(cfg.shared_dir)) != 0)
            return 2;
        if (!is_directory(cfg.shared_dir)) {
            fprintf(stderr, "vmfc: la cartella condivisa non e' una directory\n");
            return 2;
        }
    }
    cfg.shared_readonly = readonly;

    if (vm_create(&cfg) != 0)
        return 1;

    char mem[64];
    format_gib(cfg.memory_mib, mem, sizeof(mem));
    printf("Creata la macchina virtuale \"%s\"\n", cfg.name);
    printf("  bundle: %s\n", cfg.bundle);
    printf("  disco:  %s (%llu MiB, sparso)\n", cfg.disk, (unsigned long long)cfg.disk_mib);
    printf("  CPU:    %u\n", cfg.cpus);
    printf("  RAM:    %s\n", mem);
    if (cfg.iso[0])
        printf("  ISO:    %s\n", cfg.iso);
    if (cfg.drivers[0])
        printf("  driver: %s\n", cfg.drivers);
    printf("\nAvvio:  vmfc start \"%s\" --dir \"%s\"\n", cfg.name, root);
    if (vm_guest_is_windows(&cfg))
        printf("Nota:   serve Windows 11 ARM64 (ISO ufficiale Microsoft); "
               "QEMU richiede EDK2 e, per la ISO, VMFC_QEMU_ISO=1.\n");
    else
        printf("Nota:   guest Linux: usa VZ; nessuna dipendenza QEMU.\n");

    if (start_now) {
        char *start_argv[4] = {(char *)"start", cfg.name, (char *)"--dir", (char *)root};
        return cmd_start(4, start_argv, root);
    }
    return 0;
}

static int load_vm(const char *root, const char *name, vm_config *cfg)
{
    char bundle[PATH_MAX];
    if (vm_find(root, name, bundle, sizeof(bundle)) != 0) {
        fprintf(stderr, "vmfc: VM \"%s\" non trovata in %s\n", name, root);
        return -1;
    }
    if (vm_load(bundle, cfg) != 0) {
        fprintf(stderr, "vmfc: impossibile leggere la configurazione di \"%s\"\n", name);
        return -1;
    }
    return 0;
}

static int start_vm(vm_config *cfg, int headless, int seconds, const char *engine)
{
    pid_t pid;
    if (vm_running(cfg, &pid)) {
        fprintf(stderr, "vmfc: la VM e' gia' in esecuzione (pid %ld)\n", (long)pid);
        return 1;
    }
    if (!path_exists(cfg->disk)) {
        fprintf(stderr, "vmfc: disco mancante: %s\n", cfg->disk);
        return 1;
    }

    int use_qemu;
    if (engine) {
        if (strcmp(engine, "qemu") == 0) {
            use_qemu = 1;
        } else if (strcmp(engine, "vz") == 0) {
            use_qemu = 0;
        } else {
            fprintf(stderr, "vmfc: motore sconosciuto \"%s\" (usa vz o qemu)\n", engine);
            return 2;
        }
    } else {
        use_qemu = vm_guest_is_windows(cfg);
    }

    if (use_qemu)
        return qemu_run(cfg, headless, seconds);
    return vz_run(cfg, headless, seconds);
}

static int cmd_start(int argc, char **argv, const char *root)
{
    const char *name = NULL;
    const char *iso = NULL;
    const char *drivers = NULL;
    const char *engine = NULL;
    int no_drivers = 0;
    int headless = 0;
    int seconds = 0;
    int err = 0;
    vm_config cfg;

    for (int i = 0; i < argc && !err; i++) {
        if (!strcmp(argv[i], "--headless")) {
            headless = 1;
        } else if (!strcmp(argv[i], "--seconds")) {
            const char *v = arg_value(argc, argv, "--seconds", &i, &err);
            uint32_t n;
            if (v && (parse_u32(v, &n) != 0 || n == 0)) {
                fprintf(stderr, "vmfc: --seconds non valido\n");
                err = 1;
            } else if (v) {
                seconds = (int)n;
            }
        } else if (!strcmp(argv[i], "--iso")) {
            iso = arg_value(argc, argv, "--iso", &i, &err);
        } else if (!strcmp(argv[i], "--drivers")) {
            drivers = arg_value(argc, argv, "--drivers", &i, &err);
        } else if (!strcmp(argv[i], "--no-drivers")) {
            no_drivers = 1;
        } else if (!strcmp(argv[i], "--engine")) {
            engine = arg_value(argc, argv, "--engine", &i, &err);
        } else if (!strcmp(argv[i], "--dir")) {
            root = arg_value(argc, argv, "--dir", &i, &err);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "vmfc: opzione sconosciuta %s\n", argv[i]);
            err = 1;
        } else if (!name) {
            name = argv[i];
        } else {
            fprintf(stderr, "vmfc: argomento inatteso %s\n", argv[i]);
            err = 1;
        }
    }
    if (err)
        return 2;
    if (!name) {
        fprintf(stderr, "vmfc: specifica il nome della VM\n");
        return 2;
    }
    if (load_vm(root, name, &cfg) != 0)
        return 1;
    if (iso) {
        if (make_absolute_path(iso, cfg.iso, sizeof(cfg.iso)) != 0)
            return 2;
        vm_save(&cfg);
    }
    if (no_drivers) {
        cfg.drivers[0] = 0;
        vm_save(&cfg);
    } else if (drivers) {
        if (make_absolute_path(drivers, cfg.drivers, sizeof(cfg.drivers)) != 0)
            return 2;
        vm_save(&cfg);
    } else if (!cfg.drivers[0] && str_starts_with(cfg.guest_os, "arm-windows")) {
        char p[PATH_MAX];
        default_drivers_iso(p, sizeof(p));
        if (path_exists(p)) {
            snprintf(cfg.drivers, sizeof(cfg.drivers), "%s", p);
            vm_save(&cfg);
        }
    }
    return start_vm(&cfg, headless, seconds, engine);
}

static int find_vmx_in_dir(const char *dir, char *out, size_t n)
{
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || !str_ends_with(e->d_name, ".vmx"))
            continue;
        path_join(out, n, dir, e->d_name);
        found = 0;
        break;
    }
    closedir(d);
    return found;
}

static void sanitize_vm_name(const char *in, char *out, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 1 < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.' || c == '(' || c == ')')
            out[j++] = (char)c;
        else
            out[j++] = '-';
    }
    out[j] = 0;
    char *t = str_trim(out);
    if (t != out)
        memmove(out, t, strlen(t) + 1);
    if (!*out)
        snprintf(out, n, "Windows VM");
}

static int resolve_import_source(const char *src, char *vmx, size_t vn, char *disk,
                                 size_t dn, char *hint, size_t hn)
{
    vmx[0] = disk[0] = hint[0] = 0;
    if (is_directory(src)) {
        if (find_vmx_in_dir(src, vmx, vn) != 0) {
            fprintf(stderr, "vmfc: nessun file .vmx in %s\n", src);
            return -1;
        }
        snprintf(hint, hn, "%s", path_basename(src));
        char *dot = strrchr(hint, '.');
        if (dot)
            *dot = 0;
    } else if (str_ends_with(src, ".vmx")) {
        char resolved[PATH_MAX];
        if (!realpath(src, resolved)) {
            fprintf(stderr, "vmfc: percorso non valido %s\n", src);
            return -1;
        }
        snprintf(vmx, vn, "%s", resolved);
    } else if (str_ends_with(src, ".vmdk")) {
        char resolved[PATH_MAX];
        if (!realpath(src, resolved)) {
            fprintf(stderr, "vmfc: percorso non valido %s\n", src);
            return -1;
        }
        snprintf(disk, dn, "%s", resolved);
        snprintf(hint, hn, "%s", path_basename(resolved));
        char *dot = strrchr(hint, '.');
        if (dot)
            *dot = 0;
        return 0;
    } else {
        fprintf(stderr, "vmfc: sorgente non supportata %s (usa .vmwarevm, .vmx o .vmdk)\n", src);
        return -1;
    }

    char *h = vmx_get_value(vmx, "displayName");
    if (h) {
        sanitize_vm_name(h, hint, hn);
        free(h);
    }
    char *d = vmx_find_disk(vmx);
    if (!d) {
        fprintf(stderr, "vmfc: nessun disco principale nel file .vmx\n");
        return -1;
    }
    if (d[0] == '/') {
        snprintf(disk, dn, "%s", d);
    } else {
        char dir[PATH_MAX];
        path_dirname(vmx, dir, sizeof(dir));
        char joined[PATH_MAX];
        path_join(joined, sizeof(joined), dir, d);
        char resolved[PATH_MAX];
        if (!realpath(joined, resolved)) {
            fprintf(stderr, "vmfc: disco non trovato %s\n", joined);
            free(d);
            return -1;
        }
        snprintf(disk, dn, "%s", resolved);
    }
    free(d);
    return 0;
}

static int cmd_import(int argc, char **argv, const char *root)
{
    const char *src = NULL;
    const char *name = NULL;
    const char *iso = NULL;
    int start_now = 0;
    int dry_run = 0;
    int have_mem = 0, have_cpus = 0;
    uint64_t memory_mib = 0;
    uint32_t cpus = 0;
    const char *guest = NULL;
    int err = 0;

    for (int i = 0; i < argc && !err; i++) {
        if (!strcmp(argv[i], "--name")) {
            name = arg_value(argc, argv, "--name", &i, &err);
        } else if (!strcmp(argv[i], "--memory")) {
            const char *v = arg_value(argc, argv, "--memory", &i, &err);
            if (v && parse_size_mib(v, &memory_mib) != 0) {
                fprintf(stderr, "vmfc: --memory non valido\n");
                err = 1;
            } else {
                have_mem = 1;
            }
        } else if (!strcmp(argv[i], "--cpus")) {
            const char *v = arg_value(argc, argv, "--cpus", &i, &err);
            if (v && (parse_u32(v, &cpus) != 0 || cpus < 1 || cpus > 64)) {
                fprintf(stderr, "vmfc: --cpus non valido\n");
                err = 1;
            } else {
                have_cpus = 1;
            }
        } else if (!strcmp(argv[i], "--guest")) {
            guest = arg_value(argc, argv, "--guest", &i, &err);
        } else if (!strcmp(argv[i], "--iso")) {
            iso = arg_value(argc, argv, "--iso", &i, &err);
        } else if (!strcmp(argv[i], "--start")) {
            start_now = 1;
        } else if (!strcmp(argv[i], "--dry-run")) {
            dry_run = 1;
        } else if (!strcmp(argv[i], "--dir")) {
            root = arg_value(argc, argv, "--dir", &i, &err);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "vmfc: opzione sconosciuta %s\n", argv[i]);
            err = 1;
        } else if (!src) {
            src = argv[i];
        } else {
            fprintf(stderr, "vmfc: argomento inatteso %s\n", argv[i]);
            err = 1;
        }
    }
    if (err)
        return 2;
    if (!src) {
        fprintf(stderr, "vmfc: specifica la VM VMware da importare\n");
        return 2;
    }

    char vmx[PATH_MAX], disk[PATH_MAX], hint[256];
    if (resolve_import_source(src, vmx, sizeof(vmx), disk, sizeof(disk), hint, sizeof(hint)) != 0)
        return 1;

    char errbuf[1024] = {0};
    uint64_t sectors = 0;
    if (vmdk_probe(disk, &sectors, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "vmfc: %s\n", errbuf);
        return 1;
    }
    int bl_part = 0;
    uint64_t bl_off = 0;
    int bl = vmdk_detect_bitlocker(disk, &bl_part, &bl_off, errbuf, sizeof(errbuf));
    if (bl < 0) {
        fprintf(stderr, "vmfc: impossibile analizzare la tabella delle partizioni: %s\n", errbuf);
        return 1;
    }
    if (bl == 1) {
        fprintf(stderr,
                "vmfc: BitLocker rilevato sulla partizione %d (offset %llu).\n"
                "      La chiave e' sigillata nel vTPM di VMware Fusion, che Virtualization.framework\n"
                "      non puo' usare: il disco non e' avviabile in questo clone.\n"
                "      Avvia la VM in VMware Fusion, disattiva BitLocker (Gestione BitLocker ->\n"
                "      Disattiva BitLocker), attendi la decifratura, spegni Windows e ripeti l'import.\n",
                bl_part, (unsigned long long)bl_off);
        return 1;
    }

    uint64_t bytes = sectors * 512ULL;
    uint64_t disk_mib = (bytes + (1024ULL * 1024ULL) - 1) / (1024ULL * 1024ULL);
    if (dry_run) {
        printf("Sorgente: %s\n", disk);
        printf("Formato:  VMDK\n");
        printf("Disco:    %llu settori, %llu MiB (%llu GB)\n",
               (unsigned long long)sectors,
               (unsigned long long)disk_mib,
               (unsigned long long)(bytes / (1000ULL * 1000ULL * 1000ULL)));
        return 0;
    }

    vm_config cfg;
    vm_config_init(&cfg);
    if (name)
        sanitize_vm_name(name, cfg.name, sizeof(cfg.name));
    else
        sanitize_vm_name(hint, cfg.name, sizeof(cfg.name));

    if (have_mem)
        cfg.memory_mib = memory_mib;
    if (have_cpus)
        cfg.cpus = cpus;
    if (guest)
        snprintf(cfg.guest_os, sizeof(cfg.guest_os), "%s", guest);

    if (vmx[0]) {
        char *v = vmx_get_value(vmx, "memsize");
        if (v) {
            uint64_t mib = 0;
            if (!have_mem && parse_size_mib(v, &mib) == 0)
                cfg.memory_mib = mib;
            free(v);
        }
        v = vmx_get_value(vmx, "numvcpus");
        if (v) {
            uint32_t c = 0;
            if (!have_cpus && parse_u32(v, &c) == 0 && c > 0)
                cfg.cpus = c;
            free(v);
        }
        v = vmx_get_value(vmx, "guestOS");
        if (v) {
            if (!guest)
                snprintf(cfg.guest_os, sizeof(cfg.guest_os), "%s", v);
            free(v);
        }
    }

    snprintf(cfg.root, sizeof(cfg.root), "%s", root);
    for (int n = 2; n < 10; n++) {
        char trial[160];
        snprintf(trial, sizeof(trial), "%s", cfg.name);
        vm_bundle_path(root, trial, cfg.bundle, sizeof(cfg.bundle));
        if (!path_exists(cfg.bundle))
            break;
        char base[128];
        snprintf(base, sizeof(base), "%s", cfg.name);
        snprintf(cfg.name, sizeof(cfg.name), "%s (%d)", base, n);
    }
    cfg.disk_mib = disk_mib;

    if (iso) {
        if (make_absolute_path(iso, cfg.iso, sizeof(cfg.iso)) != 0)
            return 2;
    }

    printf("Importazione di \"%s\"\n", cfg.name);
    printf("  sorgente: %s\n", disk);
    printf("  disco:    %llu MiB\n", (unsigned long long)disk_mib);
    if (vm_create(&cfg) != 0)
        return 1;

    if (vmdk_to_raw(disk, cfg.disk, &sectors, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "vmfc: importazione non riuscita: %s\n", errbuf);
        rm_rf(cfg.bundle);
        return 1;
    }
    if (vm_save(&cfg) != 0) {
        fprintf(stderr, "vmfc: impossibile salvare la configurazione\n");
        return 1;
    }

    struct stat st;
    unsigned long long allocated = 0;
    if (stat(cfg.disk, &st) == 0)
        allocated = (unsigned long long)st.st_blocks * 512ULL;
    printf("Importata \"%s\"\n", cfg.name);
    printf("  bundle: %s\n", cfg.bundle);
    printf("  disco:  %llu MiB virtuali, %llu MiB allocati\n",
           (unsigned long long)disk_mib, allocated / (1024ULL * 1024ULL));
    printf("\nAvvio:  vmfc start \"%s\" --dir \"%s\"\n", cfg.name, root);

    if (start_now) {
        char *start_argv[4] = {(char *)"start", cfg.name, (char *)"--dir", (char *)root};
        return cmd_start(4, start_argv, root);
    }
    return 0;
}

static int cmd_tools(void)
{
    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr, "vmfc: HOME non impostata\n");
        return 1;
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/Library/Application Support/GMBox", home);
    if (mkdir_p(dir, 0755) != 0) {
        fprintf(stderr, "vmfc: impossibile creare %s\n", dir);
        return 1;
    }
    char dest[PATH_MAX];
    path_join(dest, sizeof(dest), dir, "utm-guest-tools.iso");
    if (path_exists(dest)) {
        printf("%s\n", dest);
        return 0;
    }

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.part", dest);
    unlink(tmp);
    printf("Scarico i driver virtio per Windows (utm-guest-tools, ~144 MB)...\n");
    fflush(stdout);
    const char *url = "https://getutm.app/downloads/utm-guest-tools-3d-latest.iso";
    char *argv[] = {"/usr/bin/curl", "-L", "--fail", "--progress-bar", "-o", tmp,
                    (char *)url, NULL};
    pid_t pid = 0;
    int rc = posix_spawn(&pid, "/usr/bin/curl", NULL, NULL, argv, environ);
    if (rc != 0) {
        fprintf(stderr, "vmfc: impossibile avviare curl\n");
        return 1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        unlink(tmp);
        fprintf(stderr, "vmfc: download non riuscito\n");
        return 1;
    }
    if (rename(tmp, dest) != 0) {
        unlink(tmp);
        fprintf(stderr, "vmfc: impossibile salvare %s\n", dest);
        return 1;
    }
    printf("%s\n", dest);
    return 0;
}

static int cmd_stop(const char *root, const char *name, int force)
{
    vm_config cfg;
    if (load_vm(root, name, &cfg) != 0)
        return 1;
    pid_t pid;
    if (!vm_running(&cfg, &pid)) {
        printf("La VM \"%s\" e' gia' spenta.\n", name);
        return 0;
    }
    if (force) {
        if (kill(pid, SIGKILL) != 0 && errno != ESRCH) {
            fprintf(stderr, "vmfc: impossibile forzare l'arresto: %s\n", strerror(errno));
            return 1;
        }
        for (int i = 0; i < 25; i++) {
            if (kill(pid, 0) != 0 && errno == ESRCH)
                break;
            usleep(100000);
        }
        unlink(cfg.pidfile);
        printf("VM arrestata con forza.\n");
        return 0;
    }
    int acpi = 0;
    if (vm_guest_is_windows(&cfg) && qemu_request_powerdown(&cfg) == 0) {
        acpi = 1;
        printf("Richiesta ACPI di spegnimento a \"%s\" (pid %ld)...\n", name, (long)pid);
    } else {
        printf("Richiesta di spegnimento a \"%s\" (pid %ld)...\n", name, (long)pid);
        if (kill(pid, SIGTERM) != 0) {
            fprintf(stderr, "vmfc: impossibile inviare SIGTERM: %s\n", strerror(errno));
            return 1;
        }
    }
    for (int i = 0; i < 150; i++) {
        if (kill(pid, 0) != 0 && errno == ESRCH) {
            printf("VM arrestata.\n");
            return 0;
        }
        usleep(200000);
    }
    if (acpi) {
        fprintf(stderr, "vmfc: il guest non risponde allo spegnimento ACPI, invio SIGTERM\n");
        kill(pid, SIGTERM);
        for (int i = 0; i < 25; i++) {
            if (kill(pid, 0) != 0 && errno == ESRCH) {
                printf("VM arrestata.\n");
                return 0;
            }
            usleep(200000);
        }
    }
    fprintf(stderr, "vmfc: il guest non risponde, forzo l'arresto\n");
    kill(pid, SIGKILL);
    usleep(300000);
    unlink(cfg.pidfile);
    return 0;
}

static int cmd_pause(const char *root, const char *name)
{
    vm_config cfg;
    if (load_vm(root, name, &cfg) != 0)
        return 1;
    pid_t pid;
    if (!vm_running(&cfg, &pid)) {
        fprintf(stderr, "vmfc: la VM \"%s\" e' spenta\n", name);
        return 1;
    }
    if (qemu_request_pause(&cfg) != 0) {
        if (vm_guest_is_windows(&cfg) || kill(pid, SIGUSR1) != 0) {
            fprintf(stderr, "vmfc: pausa non disponibile per questo backend\n");
            return 1;
        }
    }
    printf("VM \"%s\" in pausa.\n", name);
    return 0;
}

static int cmd_resume(const char *root, const char *name)
{
    vm_config cfg;
    if (load_vm(root, name, &cfg) != 0)
        return 1;
    pid_t pid;
    if (!vm_running(&cfg, &pid)) {
        fprintf(stderr, "vmfc: la VM \"%s\" e' spenta\n", name);
        return 1;
    }
    if (qemu_request_resume(&cfg) != 0) {
        if (vm_guest_is_windows(&cfg) || kill(pid, SIGUSR2) != 0) {
            fprintf(stderr, "vmfc: ripresa non disponibile per questo backend\n");
            return 1;
        }
    }
    printf("VM \"%s\" ripresa.\n", name);
    return 0;
}

static int cmd_reboot(const char *root, const char *name)
{
    vm_config cfg;
    if (load_vm(root, name, &cfg) != 0)
        return 1;
    pid_t pid;
    if (!vm_running(&cfg, &pid)) {
        fprintf(stderr, "vmfc: la VM \"%s\" e' spenta\n", name);
        return 1;
    }
    if (qemu_request_reset(&cfg) != 0) {
        if (vm_guest_is_windows(&cfg) || kill(pid, SIGHUP) != 0) {
            fprintf(stderr, "vmfc: reboot non disponibile per questo backend\n");
            return 1;
        }
    }
    printf("Riavvio richiesto a \"%s\" (pid %ld).\n", name, (long)pid);
    return 0;
}

static int cmd_status(const char *root, const char *name)
{
    vm_config cfg;
    if (load_vm(root, name, &cfg) != 0)
        return 1;
    pid_t pid;
    if (!vm_running(&cfg, &pid)) {
        printf("La VM \"%s\" e' spenta.\n", name);
        return 0;
    }
    char state[64] = "";
    if (qemu_query_status(&cfg, state, sizeof(state)) != 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/vmfc.state", cfg.bundle);
        char *text = read_text_file(path);
        if (text) {
            snprintf(state, sizeof(state), "%s", str_trim(text));
            free(text);
        }
    }
    if (state[0])
        printf("La VM \"%s\" e' in esecuzione (pid %ld, stato %s).\n", name, (long)pid, state);
    else
        printf("La VM \"%s\" e' in esecuzione (pid %ld, stato non disponibile).\n", name, (long)pid);
    return 0;
}

static void xml_escape(const char *in, char *out, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 8 < n; i++) {
        const char *rep = NULL;
        switch (in[i]) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&apos;"; break;
        default: out[j++] = in[i]; continue;
        }
        size_t len = strlen(rep);
        memcpy(out + j, rep, len);
        j += len;
    }
    out[j] = 0;
}

static int run_wait(char *const argv[])
{
    pid_t pid = 0;
    int rc = posix_spawn(&pid, argv[0], NULL, NULL, argv, environ);
    if (rc != 0)
        return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int cmd_autostart(const char *root, const char *name, int disable,
                          int status_only, int dry_run, const char *self)
{
    vm_config cfg;
    if (load_vm(root, name, &cfg) != 0)
        return 1;

    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr, "vmfc: HOME non impostata\n");
        return 1;
    }
    char agents[PATH_MAX], plist[PATH_MAX];
    snprintf(agents, sizeof(agents), "%s/Library/LaunchAgents", home);
    snprintf(plist, sizeof(plist), "%s/com.gmbox.%s.plist", agents, cfg.uuid);
    char label[128];
    snprintf(label, sizeof(label), "com.gmbox.%s", cfg.uuid);

    if (status_only) {
        int loaded = 0;
        char target[128];
        snprintf(target, sizeof(target), "gui/%u/%s", (unsigned)getuid(), label);
        char *args[] = { (char *)"/bin/launchctl", (char *)"print", target, NULL };
        loaded = path_exists(plist) && run_wait(args) == 0;
        printf("Autostart di \"%s\": %s\n", cfg.name, loaded ? "attivo" : "disattivato");
        return loaded ? 0 : 1;
    }

    if (disable) {
        char target[128];
        snprintf(target, sizeof(target), "gui/%u/%s", (unsigned)getuid(), label);
        char *args[] = { (char *)"/bin/launchctl", (char *)"bootout", target, NULL };
        run_wait(args);
        if (unlink(plist) != 0 && errno != ENOENT) {
            fprintf(stderr, "vmfc: impossibile rimuovere %s: %s\n", plist, strerror(errno));
            return 1;
        }
        printf("Autostart di \"%s\" disattivato.\n", cfg.name);
        return 0;
    }

    char exe[PATH_MAX];
    if (!realpath(self, exe))
        snprintf(exe, sizeof(exe), "%s", self);
    char logs[PATH_MAX], outlog[PATH_MAX], errlog[PATH_MAX];
    snprintf(logs, sizeof(logs), "%s/Library/Logs/GMBox", home);
    snprintf(outlog, sizeof(outlog), "%s/%s.out.log", logs, cfg.uuid);
    snprintf(errlog, sizeof(errlog), "%s/%s.err.log", logs, cfg.uuid);
    char root_abs[PATH_MAX];
    if (!realpath(root, root_abs))
        snprintf(root_abs, sizeof(root_abs), "%s", root);
    char xlabel[256], xexe[PATH_MAX * 2], xname[512], xroot[PATH_MAX * 2];
    char xout[PATH_MAX * 2], xerr[PATH_MAX * 2];
    xml_escape(label, xlabel, sizeof(xlabel));
    xml_escape(exe, xexe, sizeof(xexe));
    xml_escape(cfg.name, xname, sizeof(xname));
    xml_escape(root_abs, xroot, sizeof(xroot));
    xml_escape(outlog, xout, sizeof(xout));
    xml_escape(errlog, xerr, sizeof(xerr));

    char xml[8192];
    snprintf(xml, sizeof(xml),
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
             "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
             "<plist version=\"1.0\"><dict>\n"
             "  <key>Label</key><string>%s</string>\n"
             "  <key>ProgramArguments</key><array>"
             "<string>%s</string><string>start</string><string>%s</string>"
             "<string>--dir</string><string>%s</string></array>\n"
             "  <key>RunAtLoad</key><true/>\n"
             "  <key>LimitLoadToSessionType</key><string>Aqua</string>\n"
             "  <key>ExitTimeOut</key><integer>30</integer>\n"
             "  <key>StandardOutPath</key><string>%s</string>\n"
             "  <key>StandardErrorPath</key><string>%s</string>\n"
             "</dict></plist>\n",
             xlabel, xexe, xname, xroot, xout, xerr);

    if (dry_run) {
        printf("%s", xml);
        return 0;
    }
    if (mkdir_p(agents, 0755) != 0 || mkdir_p(logs, 0755) != 0) {
        fprintf(stderr, "vmfc: impossibile creare le cartelle LaunchAgent/Logs\n");
        return 1;
    }
    if (write_text_file(plist, xml) != 0) {
        fprintf(stderr, "vmfc: impossibile scrivere %s\n", plist);
        return 1;
    }

    char target[128];
    snprintf(target, sizeof(target), "gui/%u/%s", (unsigned)getuid(), label);
    char *bootout[] = { (char *)"/bin/launchctl", (char *)"bootout", target, NULL };
    run_wait(bootout);
    char *bootstrap[] = { (char *)"/bin/launchctl", (char *)"bootstrap",
                          (char *)target, plist, NULL };
    if (run_wait(bootstrap) != 0) {
        fprintf(stderr, "vmfc: launchctl non ha caricato %s\n", plist);
        return 1;
    }
    printf("Autostart di \"%s\" attivato: %s\n", cfg.name, plist);
    return 0;
}

static int cmd_delete(const char *root, const char *name, int force)
{
    vm_config cfg;
    if (load_vm(root, name, &cfg) != 0)
        return 1;
    pid_t pid;
    if (vm_running(&cfg, &pid)) {
        fprintf(stderr, "vmfc: la VM e' in esecuzione (pid %ld); arrestala prima\n", (long)pid);
        return 1;
    }
    if (!force) {
        printf("Eliminare definitivamente \"%s\" e il suo disco?\n", cfg.name);
        printf("Digita YES per confermare: ");
        fflush(stdout);
        char line[32];
        if (!fgets(line, sizeof(line), stdin) || strcmp(str_trim(line), "YES") != 0) {
            printf("Annullato.\n");
            return 0;
        }
    }
    if (rm_rf(cfg.bundle) != 0) {
        fprintf(stderr, "vmfc: eliminazione non riuscita: %s\n", strerror(errno));
        return 1;
    }
    printf("Eliminata \"%s\".\n", cfg.name);
    return 0;
}

int main(int argc, char **argv)
{
    char root[PATH_MAX];
    vm_default_root(root, sizeof(root));

    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
        usage(stdout);
        return 0;
    }
    if (!strcmp(argv[1], "version") || !strcmp(argv[1], "--version")) {
        printf("GMBox %s\n", VMFC_VERSION);
        printf("Virtualization.framework, macOS 14+\n");
        return 0;
    }

    const char *cmd = argv[1];
    int rest_argc = argc - 2;
    char **rest = argv + 2;
    const char *dir = NULL;
    const char *filter = NULL;
    const char *tag = NULL;
    for (int i = 0; i < rest_argc; i++) {
        if (!strcmp(rest[i], "--dir")) {
            if (i + 1 >= rest_argc) {
                fprintf(stderr, "vmfc: --dir senza valore\n");
                return 2;
            }
            dir = rest[i + 1];
        } else if (!strcmp(rest[i], "--filter")) {
            if (i + 1 >= rest_argc) {
                fprintf(stderr, "vmfc: --filter senza valore\n");
                return 2;
            }
            filter = rest[i + 1];
        } else if (!strcmp(rest[i], "--tag")) {
            if (i + 1 >= rest_argc) {
                fprintf(stderr, "vmfc: --tag senza valore\n");
                return 2;
            }
            tag = rest[i + 1];
        }
    }
    if (dir)
        snprintf(root, sizeof(root), "%s", dir);

    if (!strcmp(cmd, "list"))
        return cmd_list(root, filter, tag);
    if (!strcmp(cmd, "import"))
        return cmd_import(rest_argc, rest, root);
    if (!strcmp(cmd, "tools"))
        return cmd_tools();
    if (!strcmp(cmd, "create"))
        return cmd_create(rest_argc, rest, root);
    if (!strcmp(cmd, "start") || !strcmp(cmd, "gui"))
        return cmd_start(rest_argc, rest, root);
    if (!strcmp(cmd, "stop")) {
        const char *name = NULL;
        int force = 0;
        for (int i = 0; i < rest_argc; i++) {
            if (!strcmp(rest[i], "--dir")) {
                i++;
                continue;
            }
            if (!strcmp(rest[i], "--force")) {
                force = 1;
                continue;
            }
            if (rest[i][0] != '-' && !name)
                name = rest[i];
        }
        if (!name) {
            fprintf(stderr, "vmfc: specifica il nome della VM\n");
            return 2;
        }
        return cmd_stop(root, name, force);
    }
    if (!strcmp(cmd, "pause") || !strcmp(cmd, "resume") ||
        !strcmp(cmd, "reboot") || !strcmp(cmd, "status")) {
        const char *name = NULL;
        for (int i = 0; i < rest_argc; i++) {
            if (!strcmp(rest[i], "--dir")) {
                i++;
                continue;
            }
            if (rest[i][0] != '-' && !name)
                name = rest[i];
        }
        if (!name) {
            fprintf(stderr, "vmfc: specifica il nome della VM\n");
            return 2;
        }
        if (!strcmp(cmd, "pause"))
            return cmd_pause(root, name);
        if (!strcmp(cmd, "resume"))
            return cmd_resume(root, name);
        if (!strcmp(cmd, "reboot"))
            return cmd_reboot(root, name);
        return cmd_status(root, name);
    }
    if (!strcmp(cmd, "delete")) {
        const char *name = NULL;
        int force = 0;
        for (int i = 0; i < rest_argc; i++) {
            if (!strcmp(rest[i], "--dir")) {
                i++;
                continue;
            }
            if (!strcmp(rest[i], "--force")) {
                force = 1;
                continue;
            }
            if (rest[i][0] != '-' && !name)
                name = rest[i];
        }
        if (!name) {
            fprintf(stderr, "vmfc: specifica il nome della VM\n");
            return 2;
        }
        return cmd_delete(root, name, force);
    }
    if (!strcmp(cmd, "autostart")) {
        const char *name = NULL;
        int disable = 0, status_only = 0, dry_run = 0;
        for (int i = 0; i < rest_argc; i++) {
            if (!strcmp(rest[i], "--dir")) { i++; continue; }
            if (!strcmp(rest[i], "--disable") || !strcmp(rest[i], "--off")) { disable = 1; continue; }
            if (!strcmp(rest[i], "--status")) { status_only = 1; continue; }
            if (!strcmp(rest[i], "--dry-run")) { dry_run = 1; continue; }
            if (rest[i][0] != '-' && !name) { name = rest[i]; continue; }
        }
        if (!name) {
            fprintf(stderr, "vmfc: specifica il nome della VM\n");
            return 2;
        }
        return cmd_autostart(root, name, disable, status_only, dry_run, argv[0]);
    }
    if (!strcmp(cmd, "tag")) {
        const char *name = NULL;
        char tags[256] = "";
        for (int i = 0; i < rest_argc; i++) {
            if (!strcmp(rest[i], "--dir")) { i++; continue; }
            if (rest[i][0] != '-' && !name) { name = rest[i]; continue; }
            if (rest[i][0] == '-') continue;
            if (tags[0]) strncat(tags, ",", sizeof(tags) - strlen(tags) - 1);
            strncat(tags, rest[i], sizeof(tags) - strlen(tags) - 1);
        }
        if (!name) {
            fprintf(stderr, "vmfc: specifica il nome della VM\n");
            return 2;
        }
        vm_config cfg;
        if (load_vm(root, name, &cfg) != 0)
            return 1;
        snprintf(cfg.tags, sizeof(cfg.tags), "%s", tags);
        if (vm_save(&cfg) != 0) {
            fprintf(stderr, "vmfc: salvataggio tag fallito\n");
            return 1;
        }
        printf("Tag di \"%s\": %s\n", cfg.name, cfg.tags[0] ? cfg.tags : "(nessuno)");
        return 0;
    }
    if (!strcmp(cmd, "info")) {
        const char *name = NULL;
        for (int i = 0; i < rest_argc; i++) {
            if (!strcmp(rest[i], "--dir")) {
                i++;
                continue;
            }
            if (rest[i][0] != '-' && !name)
                name = rest[i];
        }
        if (!name) {
            fprintf(stderr, "vmfc: specifica il nome della VM\n");
            return 2;
        }
        vm_config cfg;
        if (load_vm(root, name, &cfg) != 0)
            return 1;
        vm_print_info(&cfg);
        return 0;
    }

    fprintf(stderr, "vmfc: comando sconosciuto \"%s\"\n", cmd);
    usage(stderr);
    return 2;
}
