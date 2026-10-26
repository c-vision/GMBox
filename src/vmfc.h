#ifndef VMFC_VM_H
#define VMFC_VM_H

#include <stdint.h>
#include <sys/types.h>
#include <limits.h>

#define VMFC_VERSION "0.1.0"
#define VMFC_BUNDLE_EXT ".vmfc"
#define VMFC_DEFAULT_GUEST "arm-windows11-64"
#define VMFC_DEFAULT_MEMORY_MIB 4096ULL
#define VMFC_DEFAULT_CPUS 4U
#define VMFC_DEFAULT_DISK_MIB (64ULL * 1024ULL)

typedef struct vm_config {
    char name[128];
    char root[PATH_MAX];
    char bundle[PATH_MAX];
    char vmx[PATH_MAX];
    char disk[PATH_MAX];
    char nvram[PATH_MAX];
    char machine_id[PATH_MAX];
    char pidfile[PATH_MAX];
    char logfile[PATH_MAX];
    char iso[PATH_MAX];
    char drivers[PATH_MAX];
    char shared_dir[PATH_MAX];
    int shared_readonly;
    uint64_t memory_mib;
    uint32_t cpus;
    uint64_t disk_mib;
    char mac[32];
    char uuid[48];
    char guest_os[64];
    char tags[256];
    char port_forward[512];
} vm_config;

typedef int (*vm_list_cb)(const vm_config *cfg, void *user);

void vm_default_root(char *out, size_t n);
int vm_name_valid(const char *name);
void vm_config_init(vm_config *cfg);
void vm_bundle_path(const char *root, const char *name, char *out, size_t n);
int vm_find(const char *root, const char *name, char *bundle_out, size_t n);
int vm_load(const char *bundle, vm_config *cfg);
int vm_save(const vm_config *cfg);
int vm_create(vm_config *cfg);
char *vmx_get_value(const char *vmx_path, const char *key);
char *vmx_find_disk(const char *vmx_path);
int vm_list(const char *root, vm_list_cb cb, void *user);
int vm_running(const vm_config *cfg, pid_t *pid_out);
int vm_pid_write(const vm_config *cfg);
void vm_pid_remove(const vm_config *cfg);
void vm_log_start(const vm_config *cfg, pid_t pid);
int vm_guest_is_windows(const vm_config *cfg);
void vm_print_info(const vm_config *cfg);

#endif
