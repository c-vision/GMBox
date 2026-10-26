#define _DARWIN_C_SOURCE
#include "qemu.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static volatile sig_atomic_t g_stop;

static void on_stop_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int find_path(const char *env_name, const char *const *candidates,
                     int executable, char *out, size_t n)
{
    const char *env = env_name ? getenv(env_name) : NULL;
    if (env && *env) {
        if (access(env, executable ? X_OK : R_OK) == 0) {
            snprintf(out, n, "%s", env);
            return 0;
        }
        return -1;
    }
    for (size_t i = 0; candidates[i]; i++) {
        if (access(candidates[i], executable ? X_OK : R_OK) == 0) {
            snprintf(out, n, "%s", candidates[i]);
            return 0;
        }
    }
    return -1;
}

static void add_arg(char **argv, int *argc, const char *fmt, ...)
{
    va_list ap;
    char *s = NULL;
    va_start(ap, fmt);
    if (vasprintf(&s, fmt, ap) < 0) {
        va_end(ap);
        fprintf(stderr, "vmfc: out of memory\n");
        exit(2);
    }
    va_end(ap);
    argv[(*argc)++] = s;
}

static void free_args(char **argv, int argc)
{
    for (int i = 0; i < argc; i++)
        free(argv[i]);
}

static void qmp_id(const char *uuid, char *out, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; uuid[i] && j + 1 < n && j < 8; i++) {
        unsigned char c = (unsigned char)uuid[i];
        if (isalnum(c))
            out[j++] = (char)c;
    }
    if (j == 0)
        snprintf(out, n, "%ld", (long)getpid());
    else
        out[j] = 0;
}

static int qmp_write_all(int fd, const char *s)
{
    size_t left = strlen(s);
    while (left > 0) {
        ssize_t n = send(fd, s, left, 0);
        if (n <= 0)
            return -1;
        s += n;
        left -= (size_t)n;
    }
    return 0;
}

static int qmp_count_returns(const char *s)
{
    int n = 0;
    const char *p = s;
    while ((p = strstr(p, "\"return\"")) != NULL) {
        n++;
        p += 8;
    }
    return n;
}

static int qmp_connect(const vm_config *cfg)
{
    char id[16];
    char path[PATH_MAX];
    qmp_id(cfg->uuid, id, sizeof(id));
    snprintf(path, sizeof(path), "/tmp/gmbox-qmp-%s.sock", id);
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path))
        return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int qmp_request(const vm_config *cfg, const char *command,
                       char *reply, size_t reply_n)
{
    int fd = qmp_connect(cfg);
    if (fd < 0)
        return -1;

    char cmds[1024];
    snprintf(cmds, sizeof(cmds),
             "{\"execute\":\"qmp_capabilities\"}\n"
             "{\"execute\":\"%s\"}\n", command);
    if (qmp_write_all(fd, cmds) != 0) {
        close(fd);
        return -1;
    }

    char buf[8192];
    size_t used = 0;
    int ok = -1;
    while (used + 1 < sizeof(buf)) {
        ssize_t n = read(fd, buf + used, sizeof(buf) - 1 - used);
        if (n <= 0)
            break;
        used += (size_t)n;
        buf[used] = 0;
        if (qmp_count_returns(buf) >= 2) {
            ok = 0;
            if (reply && reply_n)
                snprintf(reply, reply_n, "%s", buf);
            break;
        }
    }
    close(fd);
    return ok;
}

int qemu_request_powerdown(const vm_config *cfg)
{
    return qmp_request(cfg, "system_powerdown", NULL, 0);
}

int qemu_request_reset(const vm_config *cfg)
{
    return qmp_request(cfg, "system_reset", NULL, 0);
}

int qemu_request_pause(const vm_config *cfg)
{
    return qmp_request(cfg, "stop", NULL, 0);
}

int qemu_request_resume(const vm_config *cfg)
{
    return qmp_request(cfg, "cont", NULL, 0);
}

int qemu_query_status(const vm_config *cfg, char *status, size_t n)
{
    char reply[8192];
    if (qmp_request(cfg, "query-status", reply, sizeof(reply)) != 0)
        return -1;
    const char *p = strstr(reply, "\"status\"");
    if (!p)
        return -1;
    p = strchr(p, ':');
    if (!p)
        return -1;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '\"')
        return -1;
    p++;
    const char *e = strchr(p, '\"');
    if (!e)
        return -1;
    size_t len = (size_t)(e - p);
    if (len >= n)
        len = n - 1;
    memcpy(status, p, len);
    status[len] = 0;
    return 0;
}

static void qemu_netdev(const vm_config *cfg, char *out, size_t n)
{
    snprintf(out, n, "user,id=net0");
    const char *p = cfg->port_forward;
    while (p && *p) {
        while (*p == ',' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        char rule[128];
        size_t len = (size_t)(e - p);
        if (len >= sizeof(rule))
            len = sizeof(rule) - 1;
        memcpy(rule, p, len);
        rule[len] = 0;
        char proto[8];
        unsigned host = 0, guest = 0;
        if (sscanf(rule, "%7[^:]:%u:%u", proto, &host, &guest) == 3 &&
            host > 0 && host <= 65535 && guest > 0 && guest <= 65535) {
            size_t used = strlen(out);
            snprintf(out + used, n - used, ",hostfwd=%s::%u-:%u", proto, host, guest);
        }
        p = *e ? e + 1 : e;
    }
}

int qemu_run(vm_config *cfg, int headless, int seconds)
{
    static const char *const qemu_candidates[] = {
        "/opt/homebrew/bin/qemu-system-aarch64",
        "/usr/local/bin/qemu-system-aarch64",
        "/usr/bin/qemu-system-aarch64",
        NULL
    };
    static const char *const fw_candidates[] = {
        "/opt/homebrew/share/qemu/edk2-aarch64-code.fd",
        "/usr/local/share/qemu/edk2-aarch64-code.fd",
        "/usr/share/qemu/edk2-aarch64-code.fd",
        NULL
    };
    static const char *const vars_candidates[] = {
        "/opt/homebrew/share/qemu/edk2-arm-vars.fd",
        "/usr/local/share/qemu/edk2-arm-vars.fd",
        "/usr/share/qemu/edk2-arm-vars.fd",
        NULL
    };

    char qemu[PATH_MAX];
    char firmware[PATH_MAX];
    char vars_template[PATH_MAX];
    char vars[PATH_MAX];
    char qmp[PATH_MAX];
    char qmp_idbuf[16];

    if (find_path("VMFC_QEMU", qemu_candidates, 1, qemu, sizeof(qemu)) != 0) {
        fprintf(stderr,
                "vmfc: QEMU/HVF non disponibile. Serve qemu-system-aarch64 "
                "(es. `brew install qemu`) oppure VMFC_QEMU=/percorso/assoluto.\n");
        return 1;
    }
    if (find_path("VMFC_QEMU_FIRMWARE", fw_candidates, 0, firmware, sizeof(firmware)) != 0) {
        fprintf(stderr,
                "vmfc: firmware EDK2 non trovato (edk2-aarch64-code.fd). "
                "Imposta VMFC_QEMU_FIRMWARE=/percorso/assoluto.\n");
        return 1;
    }
    if (find_path("VMFC_QEMU_VARS", vars_candidates, 0, vars_template, sizeof(vars_template)) != 0) {
        fprintf(stderr,
                "vmfc: template NVRAM EDK2 non trovato (edk2-arm-vars.fd). "
                "Imposta VMFC_QEMU_VARS=/percorso/assoluto.\n");
        return 1;
    }

    snprintf(vars, sizeof(vars), "%s/vars.fd", cfg->bundle);
    if (!path_exists(vars)) {
        if (copy_file(vars_template, vars) != 0) {
            fprintf(stderr, "vmfc: impossibile creare la NVRAM QEMU %s\n", vars);
            return 1;
        }
    }

    qmp_id(cfg->uuid, qmp_idbuf, sizeof(qmp_idbuf));
    snprintf(qmp, sizeof(qmp), "/tmp/gmbox-qmp-%s.sock", qmp_idbuf);
    if (strlen(qmp) >= 100) {
        fprintf(stderr, "vmfc: percorso QMP troppo lungo per un socket Unix\n");
        return 1;
    }
    unlink(qmp);

    char *argv[128];
    int argc = 0;
    add_arg(argv, &argc, "%s", qemu);
    add_arg(argv, &argc, "-name");
    add_arg(argv, &argc, "%s", cfg->name);
    add_arg(argv, &argc, "-machine");
    add_arg(argv, &argc, "virt,gic-version=3");
    add_arg(argv, &argc, "-accel");
    add_arg(argv, &argc, "hvf");
    add_arg(argv, &argc, "-cpu");
    add_arg(argv, &argc, "host");
    add_arg(argv, &argc, "-smp");
    add_arg(argv, &argc, "%u,sockets=1,cores=%u,threads=1", cfg->cpus, cfg->cpus);
    add_arg(argv, &argc, "-m");
    add_arg(argv, &argc, "%llu", (unsigned long long)cfg->memory_mib);
    add_arg(argv, &argc, "-rtc");
    add_arg(argv, &argc, "base=localtime,clock=host");
    add_arg(argv, &argc, "-drive");
    add_arg(argv, &argc, "if=pflash,format=raw,readonly=on,file=%s", firmware);
    add_arg(argv, &argc, "-drive");
    add_arg(argv, &argc, "if=pflash,format=raw,file=%s", vars);
    add_arg(argv, &argc, "-drive");
    add_arg(argv, &argc, "if=none,id=os,format=raw,file=%s", cfg->disk);
    add_arg(argv, &argc, "-device");
    add_arg(argv, &argc, "nvme,drive=os,serial=GMBOX");
    add_arg(argv, &argc, "-device");
    add_arg(argv, &argc, "qemu-xhci");
    add_arg(argv, &argc, "-device");
    add_arg(argv, &argc, "usb-kbd");
    add_arg(argv, &argc, "-device");
    add_arg(argv, &argc, "usb-tablet");
    add_arg(argv, &argc, "-device");
    add_arg(argv, &argc, "ramfb");
    add_arg(argv, &argc, "-display");
    add_arg(argv, &argc, headless ? "none" : "cocoa");
    add_arg(argv, &argc, "-qmp");
    add_arg(argv, &argc, "unix:%s,server=on,wait=off", qmp);
    add_arg(argv, &argc, "-serial");
    add_arg(argv, &argc, "none");
    add_arg(argv, &argc, "-pidfile");
    add_arg(argv, &argc, "%s", cfg->pidfile);

    if ((getenv("VMFC_QEMU_NET") && *getenv("VMFC_QEMU_NET")) || cfg->port_forward[0]) {
        char netdev[1024];
        qemu_netdev(cfg, netdev, sizeof(netdev));
        add_arg(argv, &argc, "-netdev");
        add_arg(argv, &argc, "%s", netdev);
        add_arg(argv, &argc, "-device");
        add_arg(argv, &argc, "virtio-net-pci,netdev=net0,mac=%s", cfg->mac);
    } else {
        add_arg(argv, &argc, "-nic");
        add_arg(argv, &argc, "none");
    }

    if (getenv("VMFC_QEMU_ISO") && *getenv("VMFC_QEMU_ISO")) {
        if (cfg->iso[0] && path_exists(cfg->iso)) {
            add_arg(argv, &argc, "-drive");
            add_arg(argv, &argc, "if=none,id=iso,media=cdrom,readonly=on,file=%s", cfg->iso);
            add_arg(argv, &argc, "-device");
            add_arg(argv, &argc, "usb-storage,drive=iso");
        }
        if (cfg->drivers[0] && path_exists(cfg->drivers)) {
            add_arg(argv, &argc, "-drive");
            add_arg(argv, &argc, "if=none,id=drv,media=cdrom,readonly=on,file=%s", cfg->drivers);
            add_arg(argv, &argc, "-device");
            add_arg(argv, &argc, "usb-storage,drive=drv");
        }
    }
    argv[argc] = NULL;

    g_stop = 0;
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "vmfc: fork fallita: %s\n", strerror(errno));
        free_args(argv, argc);
        return 1;
    }
    if (child == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        execv(qemu, argv);
        fprintf(stderr, "vmfc: impossibile eseguire %s: %s\n", qemu, strerror(errno));
        _exit(127);
    }

    vm_log_start(cfg, child);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    time_t started = time(NULL);
    int status = 0;
    int reaped = 0;
    int stop_requested = 0;
    int powerdown_sent = 0;
    int sigterm_sent = 0;
    time_t powerdown_at = 0;
    while (!reaped) {
        pid_t r = waitpid(child, &status, WNOHANG);
        if (r == child) {
            reaped = 1;
            break;
        }
        if (r < 0 && errno == ECHILD) {
            reaped = 1;
            break;
        }
        if (!stop_requested &&
            (g_stop || (seconds > 0 && time(NULL) - started >= seconds))) {
            stop_requested = 1;
            if (qemu_request_powerdown(cfg) == 0) {
                powerdown_sent = 1;
                powerdown_at = time(NULL);
            } else {
                kill(child, SIGTERM);
                sigterm_sent = 1;
            }
        }
        if (powerdown_sent && !sigterm_sent &&
            time(NULL) - powerdown_at >= 10) {
            kill(child, SIGTERM);
            sigterm_sent = 1;
        }
        usleep(100000);
    }
    if (!reaped) {
        kill(child, SIGTERM);
        waitpid(child, &status, 0);
    }

    unlink(qmp);
    vm_pid_remove(cfg);
    free_args(argv, argc);
    if (WIFEXITED(status))
        return WEXITSTATUS(status) == 0 ? 0 : 1;
    return 0;
}
