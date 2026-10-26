#define _DARWIN_C_SOURCE
#include "vmfc.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void vm_default_root(char *out, size_t n)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "/tmp";
    snprintf(out, n, "%s/Documents/Virtual Machines", home);
}

int vm_name_valid(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len > 100 || name[0] == '.' || name[0] == ' ')
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.' || c == '(' || c == ')'))
            return 0;
    }
    return 1;
}

void vm_config_init(vm_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->memory_mib = VMFC_DEFAULT_MEMORY_MIB;
    cfg->cpus = VMFC_DEFAULT_CPUS;
    cfg->disk_mib = VMFC_DEFAULT_DISK_MIB;
    snprintf(cfg->guest_os, sizeof(cfg->guest_os), "%s", VMFC_DEFAULT_GUEST);
    snprintf(cfg->mac, sizeof(cfg->mac), "00:50:56:%02x:%02x:%02x",
             arc4random_uniform(0x40), arc4random_uniform(256), arc4random_uniform(256));
    char u[33];
    random_hex(u, sizeof(u), 16);
    snprintf(cfg->uuid, sizeof(cfg->uuid),
             "%c%c%c%c%c%c%c%c-%c%c%c%c-%c%c%c%c-%c%c%c%c-%c%c%c%c%c%c%c%c%c%c%c%c",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
             u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15],
             u[16], u[17], u[18], u[19], u[20], u[21], u[22], u[23],
             u[24], u[25], u[26], u[27], u[28], u[29], u[30], u[31]);
}

void vm_bundle_path(const char *root, const char *name, char *out, size_t n)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s%s", name, VMFC_BUNDLE_EXT);
    path_join(out, n, root, dir);
}

static void vm_fill_paths(vm_config *cfg, const char *bundle)
{
    snprintf(cfg->bundle, sizeof(cfg->bundle), "%s", bundle);
    path_join(cfg->vmx, sizeof(cfg->vmx), bundle, "config.vmx");
    path_join(cfg->disk, sizeof(cfg->disk), bundle, "disk.raw");
    path_join(cfg->nvram, sizeof(cfg->nvram), bundle, "nvram.bin");
    path_join(cfg->machine_id, sizeof(cfg->machine_id), bundle, "machine.id");
    path_join(cfg->pidfile, sizeof(cfg->pidfile), bundle, "vmfc.pid");
    path_join(cfg->logfile, sizeof(cfg->logfile), bundle, "vmware.log");
}

static int vm_find_in(const char *dir, const char *name, unsigned depth,
                      char *bundle_out, size_t n)
{
    char want[PATH_MAX];
    snprintf(want, sizeof(want), "%s%s", name, VMFC_BUNDLE_EXT);
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char path[PATH_MAX];
        path_join(path, sizeof(path), dir, e->d_name);
        struct stat sb;
        if (lstat(path, &sb) != 0 || S_ISLNK(sb.st_mode) || !S_ISDIR(sb.st_mode))
            continue;
        if (!strcmp(e->d_name, want)) {
            snprintf(bundle_out, n, "%s", path);
            closedir(d);
            return 0;
        }
        if (depth < 16 && vm_find_in(path, name, depth + 1, bundle_out, n) == 0) {
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

int vm_find(const char *root, const char *name, char *bundle_out, size_t n)
{
    char bundle[PATH_MAX];
    vm_bundle_path(root, name, bundle, sizeof(bundle));
    if (is_directory(bundle)) {
        snprintf(bundle_out, n, "%s", bundle);
        return 0;
    }
    return vm_find_in(root, name, 0, bundle_out, n);
}

static char *vmx_unquote(char *s)
{
    s = str_trim(s);
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        s[len - 1] = 0;
        s++;
        char *w = s;
        for (char *r = s; *r; r++) {
            if (*r == '\\' && (r[1] == '"' || r[1] == '\\')) {
                r++;
                *w++ = *r;
            } else {
                *w++ = *r;
            }
        }
        *w = 0;
    }
    return s;
}

static char *vmx_lookup(const char *text, const char *key)
{
    if (!text || !key)
        return NULL;
    char *copy = xstrdup(text);
    char *save = NULL;
    char *line = strtok_r(copy, "\n", &save);
    char *result = NULL;
    while (line) {
        char *t = str_trim(line);
        if (*t && *t != '#') {
            char *eq = strchr(t, '=');
            if (eq) {
                *eq = 0;
                char *k = str_trim(t);
                if (strcmp(k, key) == 0) {
                    result = xstrdup(vmx_unquote(eq + 1));
                    break;
                }
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }
    free(copy);
    return result;
}

static void vmx_escape(const char *in, char *out, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < n; i++) {
        if (in[i] == '"' || in[i] == '\\')
            out[j++] = '\\';
        out[j++] = in[i];
    }
    out[j] = 0;
}

int vm_load(const char *bundle, vm_config *cfg)
{
    vm_config_init(cfg);
    vm_fill_paths(cfg, bundle);
    snprintf(cfg->name, sizeof(cfg->name), "%s", path_basename(bundle));
    char *dot = strrchr(cfg->name, '.');
    if (dot)
        *dot = 0;

    char *text = read_text_file(cfg->vmx);
    if (!text)
        return -1;
    char *v = NULL;

    v = vmx_lookup(text, "displayName");
    if (v) {
        snprintf(cfg->name, sizeof(cfg->name), "%s", v);
        free(v);
    }
    v = vmx_lookup(text, "guestOS");
    if (v) {
        snprintf(cfg->guest_os, sizeof(cfg->guest_os), "%s", v);
        free(v);
    }
    v = vmx_lookup(text, "memsize");
    if (v) {
        uint64_t mib = 0;
        if (parse_size_mib(v, &mib) == 0)
            cfg->memory_mib = mib;
        free(v);
    }
    v = vmx_lookup(text, "numvcpus");
    if (v) {
        uint32_t c = 0;
        if (parse_u32(v, &c) == 0 && c > 0)
            cfg->cpus = c;
        free(v);
    }
    v = vmx_lookup(text, "vmfc.diskSizeMiB");
    if (v) {
        uint32_t d = 0;
        if (parse_u32(v, &d) == 0 && d > 0)
            cfg->disk_mib = d;
        free(v);
    }
    v = vmx_lookup(text, "sata0:1.fileName");
    if (v) {
        snprintf(cfg->iso, sizeof(cfg->iso), "%s", v);
        free(v);
    }
    v = vmx_lookup(text, "sata0:2.fileName");
    if (v) {
        snprintf(cfg->drivers, sizeof(cfg->drivers), "%s", v);
        free(v);
    }
    v = vmx_lookup(text, "ethernet0.address");
    if (v) {
        snprintf(cfg->mac, sizeof(cfg->mac), "%s", v);
        free(v);
    }
    v = vmx_lookup(text, "uuid.bios");
    if (v) {
        snprintf(cfg->uuid, sizeof(cfg->uuid), "%s", v);
        free(v);
    }
    v = vmx_lookup(text, "vmfc.sharedFolder");
    if (v) {
        snprintf(cfg->shared_dir, sizeof(cfg->shared_dir), "%s", v);
        free(v);
    }
    v = vmx_lookup(text, "vmfc.sharedReadOnly");
    if (v) {
        cfg->shared_readonly = str_ieq(v, "TRUE");
        free(v);
    }
    v = vmx_lookup(text, "vmfc.tags");
    if (v) {
        snprintf(cfg->tags, sizeof(cfg->tags), "%s", v);
        free(v);
    }
    v = vmx_lookup(text, "vmfc.portForward");
    if (v) {
        snprintf(cfg->port_forward, sizeof(cfg->port_forward), "%s", v);
        free(v);
    }
    free(text);
    return 0;
}

char *vmx_get_value(const char *vmx_path, const char *key)
{
    char *text = read_text_file(vmx_path);
    if (!text)
        return NULL;
    char *v = vmx_lookup(text, key);
    free(text);
    return v;
}

char *vmx_find_disk(const char *vmx_path)
{
    char key[32];
    const char *buses[] = { "nvme0", "scsi0", "sata0", "ide0" };
    for (size_t b = 0; b < sizeof(buses) / sizeof(buses[0]); b++) {
        for (int i = 0; i < 10; i++) {
            snprintf(key, sizeof(key), "%s:%d.fileName", buses[b], i);
            char *v = vmx_get_value(vmx_path, key);
            if (v && *v) {
                size_t n = strlen(v);
                if (n > 4 && !strcasecmp(v + n - 4, ".iso")) {
                    free(v);
                    continue;
                }
                return v;
            }
            free(v);
        }
    }
    return NULL;
}

int vm_save(const vm_config *cfg)
{
    char esc_name[512], esc_iso[PATH_MAX * 2], esc_drivers[PATH_MAX * 2], esc_share[PATH_MAX * 2], esc_tags[1024], esc_ports[2048];
    vmx_escape(cfg->name, esc_name, sizeof(esc_name));
    vmx_escape(cfg->iso, esc_iso, sizeof(esc_iso));
    vmx_escape(cfg->drivers, esc_drivers, sizeof(esc_drivers));
    vmx_escape(cfg->shared_dir, esc_share, sizeof(esc_share));
    vmx_escape(cfg->tags, esc_tags, sizeof(esc_tags));
    vmx_escape(cfg->port_forward, esc_ports, sizeof(esc_ports));

    FILE *f = fopen(cfg->vmx, "wb");
    if (!f)
        return -1;
    fprintf(f, "%s\n", ".encoding = \"UTF-8\"");
    fprintf(f, "%s\n", "config.version = \"8\"");
    fprintf(f, "%s\n", "virtualHW.version = \"22\"");
    fprintf(f, "displayName = \"%s\"\n", esc_name);
    fprintf(f, "guestOS = \"%s\"\n", cfg->guest_os);
    fprintf(f, "%s\n", "firmware = \"efi\"");
    fprintf(f, "memsize = \"%llu\"\n", (unsigned long long)cfg->memory_mib);
    fprintf(f, "numvcpus = \"%u\"\n", cfg->cpus);
    fprintf(f, "%s\n", "cpuid.coresPerSocket = \"1\"");
    fprintf(f, "%s\n", "nvram = \"nvram.bin\"");
    fprintf(f, "%s\n", "powerType.powerOn = \"soft\"");
    fprintf(f, "%s\n", "powerType.suspend = \"soft\"");
    fprintf(f, "%s\n", "powerType.reset = \"soft\"");
    fprintf(f, "%s\n", "tools.syncTime = \"TRUE\"");
    fprintf(f, "%s\n", "nvme0.present = \"TRUE\"");
    fprintf(f, "%s\n", "nvme0:0.present = \"TRUE\"");
    fprintf(f, "%s\n", "nvme0:0.fileName = \"disk.raw\"");
    fprintf(f, "%s\n", "sata0.present = \"TRUE\"");
    fprintf(f, "sata0:1.present = \"%s\"\n", cfg->iso[0] ? "TRUE" : "FALSE");
    fprintf(f, "%s\n", "sata0:1.deviceType = \"cdrom-image\"");
    fprintf(f, "sata0:1.fileName = \"%s\"\n", esc_iso);
    fprintf(f, "sata0:2.present = \"%s\"\n", cfg->drivers[0] ? "TRUE" : "FALSE");
    fprintf(f, "%s\n", "sata0:2.deviceType = \"cdrom-image\"");
    fprintf(f, "sata0:2.fileName = \"%s\"\n", esc_drivers);
    fprintf(f, "%s\n", "ethernet0.present = \"TRUE\"");
    fprintf(f, "%s\n", "ethernet0.connectionType = \"nat\"");
    fprintf(f, "%s\n", "ethernet0.virtualDev = \"virtio\"");
    fprintf(f, "%s\n", "ethernet0.addressType = \"static\"");
    fprintf(f, "ethernet0.address = \"%s\"\n", cfg->mac);
    fprintf(f, "uuid.bios = \"%s\"\n", cfg->uuid);
    fprintf(f, "%s\n", "vmfc.product = \"GMBox\"");
    fprintf(f, "vmfc.version = \"%s\"\n", VMFC_VERSION);
    fprintf(f, "vmfc.diskSizeMiB = \"%llu\"\n", (unsigned long long)cfg->disk_mib);
    fprintf(f, "vmfc.sharedFolder = \"%s\"\n", esc_share);
    fprintf(f, "vmfc.sharedReadOnly = \"%s\"\n", cfg->shared_readonly ? "TRUE" : "FALSE");
    fprintf(f, "vmfc.tags = \"%s\"\n", esc_tags);
    fprintf(f, "vmfc.portForward = \"%s\"\n", esc_ports);
    fprintf(f, "%s\n", "vmfc.graphics = \"virtio-gpu 1920x1200\"");
    int rc = fclose(f);
    return rc == 0 ? 0 : -1;
}

int vm_create(vm_config *cfg)
{
    if (mkdir_p(cfg->root, 0755) != 0) {
        vmfc_log("impossibile creare la cartella %s: %s", cfg->root, strerror(errno));
        return -1;
    }
    vm_fill_paths(cfg, cfg->bundle);
    if (path_exists(cfg->bundle)) {
        vmfc_log("la VM esiste gia': %s", cfg->bundle);
        return -1;
    }
    if (mkdir(cfg->bundle, 0755) != 0) {
        vmfc_log("impossibile creare %s: %s", cfg->bundle, strerror(errno));
        return -1;
    }
    if (vm_save(cfg) != 0) {
        vmfc_log("impossibile scrivere %s", cfg->vmx);
        return -1;
    }
    int fd = open(cfg->disk, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        vmfc_log("impossibile creare il disco: %s", strerror(errno));
        return -1;
    }
    off_t bytes = (off_t)(cfg->disk_mib * 1024ULL * 1024ULL);
    if (ftruncate(fd, bytes) != 0) {
        vmfc_log("impossibile dimensionare il disco: %s", strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    touch_file(cfg->logfile);
    vm_log_start(cfg, 0);
    return 0;
}

struct list_state {
    vm_config *items;
    size_t count;
    size_t cap;
};

static int vm_list_cb_path(const char *root, const char *bundle, void *user)
{
    (void)root;
    struct list_state *st = user;
    vm_config cfg;
    if (vm_load(bundle, &cfg) != 0)
        return 0;
    if (st->count == st->cap) {
        st->cap = st->cap ? st->cap * 2 : 8;
        st->items = realloc(st->items, st->cap * sizeof(*st->items));
        if (!st->items) {
            fprintf(stderr, "vmfc: out of memory\n");
            exit(1);
        }
    }
    st->items[st->count++] = cfg;
    return 0;
}

static int config_cmp(const void *a, const void *b)
{
    const vm_config *ca = a;
    const vm_config *cb = b;
    return strcasecmp(ca->name, cb->name);
}

static int vm_scan_dir(const char *dir, unsigned depth, struct list_state *st)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char path[PATH_MAX];
        path_join(path, sizeof(path), dir, e->d_name);
        struct stat sb;
        if (lstat(path, &sb) != 0 || S_ISLNK(sb.st_mode) || !S_ISDIR(sb.st_mode))
            continue;
        if (str_ends_with(e->d_name, VMFC_BUNDLE_EXT)) {
            vm_list_cb_path(dir, path, st);
            continue;
        }
        if (depth < 16)
            vm_scan_dir(path, depth + 1, st);
    }
    closedir(d);
    return 0;
}

int vm_list(const char *root, vm_list_cb cb, void *user)
{
    struct stat sb;
    if (lstat(root, &sb) != 0) {
        if (errno == ENOENT)
            return 0;
        return -1;
    }
    if (!S_ISDIR(sb.st_mode))
        return -1;
    struct list_state st = {0};
    vm_scan_dir(root, 0, &st);
    qsort(st.items, st.count, sizeof(vm_config), config_cmp);
    for (size_t i = 0; i < st.count; i++) {
        if (cb(&st.items[i], user) != 0)
            break;
    }
    free(st.items);
    return 0;
}

int vm_running(const vm_config *cfg, pid_t *pid_out)
{
    char *text = read_text_file(cfg->pidfile);
    if (!text)
        return 0;
    pid_t pid = (pid_t)strtol(text, NULL, 10);
    free(text);
    if (pid <= 0)
        return 0;
    if (kill(pid, 0) == 0 || errno == EPERM) {
        if (pid_out)
            *pid_out = pid;
        return 1;
    }
    if (errno == ESRCH)
        unlink(cfg->pidfile);
    return 0;
}

int vm_pid_write(const vm_config *cfg)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
    return write_text_file(cfg->pidfile, buf);
}

void vm_pid_remove(const vm_config *cfg)
{
    unlink(cfg->pidfile);
}

void vm_log_start(const vm_config *cfg, pid_t pid)
{
    char line[512];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tmv);
    if (pid > 0)
        snprintf(line, sizeof(line), "%s vmfc %s avvio (pid %ld)\n", ts, VMFC_VERSION, (long)pid);
    else
        snprintf(line, sizeof(line), "%s vmfc %s configurazione creata\n", ts, VMFC_VERSION);
    append_text_file(cfg->logfile, line);
}

int vm_guest_is_windows(const vm_config *cfg)
{
    return str_starts_with(cfg->guest_os, "arm-windows") ||
           strstr(cfg->guest_os, "windows") != NULL ||
           strstr(cfg->guest_os, "Windows") != NULL;
}

void vm_print_info(const vm_config *cfg)
{
    char mem[64];
    char vars[PATH_MAX];
    int qemu = vm_guest_is_windows(cfg);
    int qemu_iso = getenv("VMFC_QEMU_ISO") && *getenv("VMFC_QEMU_ISO");
    int qemu_net = getenv("VMFC_QEMU_NET") && *getenv("VMFC_QEMU_NET");
    format_gib(cfg->memory_mib, mem, sizeof(mem));
    snprintf(vars, sizeof(vars), "%s/vars.fd", cfg->bundle);
    printf("Nome:        %s\n", cfg->name);
    printf("Bundle:      %s\n", cfg->bundle);
    printf("Guest OS:    %s\n", cfg->guest_os);
    printf("Motore:      %s\n", qemu
           ? "QEMU/HVF (Windows)" : "VZ/Virtualization.framework (Linux)");
    printf("Firmware:    %s\n", qemu ? "EFI (EDK2)" : "EFI (VZ)");
    printf("CPU:         %u\n", cfg->cpus);
    printf("Memoria:     %s\n", mem);
    printf("Disco:       %llu MiB (%s)\n", (unsigned long long)cfg->disk_mib, cfg->disk);
    if (cfg->tags[0])
        printf("Tag:         %s\n", cfg->tags);
    if (cfg->port_forward[0])
        printf("Porte:       %s\n", cfg->port_forward);
    printf("NVRAM:       %s\n", qemu ? vars : cfg->nvram);
    if (cfg->iso[0])
        printf("ISO:         %s%s\n", cfg->iso,
               qemu && !qemu_iso ? " (non collegata: VMFC_QEMU_ISO=1)" : "");
    else
        printf("ISO:         (nessuna)\n");
    if (cfg->drivers[0])
        printf("Driver ISO:  %s%s\n", cfg->drivers,
               qemu && !qemu_iso ? " (non collegata: VMFC_QEMU_ISO=1)" : "");
    if (cfg->shared_dir[0]) {
        if (qemu)
            printf("Cartella:    %s (non supportata dal backend QEMU)\n", cfg->shared_dir);
        else
            printf("Cartella:    %s%s\n", cfg->shared_dir, cfg->shared_readonly ? " (sola lettura)" : "");
    }
    if (qemu) {
        if (qemu_net)
            printf("Rete:        NAT QEMU (virtio-net-pci), MAC %s\n", cfg->mac);
        else
            printf("Rete:        disattivata (QEMU default; VMFC_QEMU_NET=1 per NAT)\n");
    } else {
        printf("Rete:        NAT, MAC %s\n", cfg->mac);
    }
    printf("UUID:        %s\n", cfg->uuid);
    pid_t pid;
    if (vm_running(cfg, &pid))
        printf("Stato:       in esecuzione (pid %ld)\n", (long)pid);
    else
        printf("Stato:       spenta\n");
}
