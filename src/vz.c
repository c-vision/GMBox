#define _DARWIN_C_SOURCE
#include "vz.h"
#include "objc_util.h"
#include "util.h"

#include <Block.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static vm_config *g_cfg;
static id g_app;
static id g_vm;
static id g_window;
static id g_controller;
static int g_headless;
static int g_running;
static int g_terminating;
static int g_terminate_replied;
static int g_sigpipe[2] = {-1, -1};

static void show_alert(const char *title, const char *info)
{
    if (g_headless) {
        fprintf(stderr, "vmfc: %s: %s\n", title, info);
        return;
    }
    id alert = o_init("NSAlert");
    if (!alert)
        return;
    o_vmsg1(alert, "setMessageText:", o_str(title));
    o_vmsg1(alert, "setInformativeText:", o_str(info));
    o_vmsg1(alert, "addButtonWithTitle:", o_str("OK"));
    ((long(*)(id, SEL))objc_msgSend)(alert, o_sel("runModal"));
}

static void reply_terminate_yes(void)
{
    if (g_terminate_replied)
        return;
    g_terminate_replied = 1;
    ((void(*)(id, SEL, BOOL))objc_msgSend)(g_app, o_sel("replyToApplicationShouldTerminate:"), YES);
}

static void print_error(const char *what, id err)
{
    if (err) {
        const char *desc = o_cstr(o_msg0(err, "localizedDescription"));
        long code = ((long(*)(id, SEL))objc_msgSend)(err, o_sel("code"));
        fprintf(stderr, "vmfc: %s: %s (code %ld)\n", what, desc, code);
    } else {
        fprintf(stderr, "vmfc: %s\n", what);
    }
}

static void stop_completed(id error)
{
    (void)error;
    g_running = 0;
    if (g_terminating)
        reply_terminate_yes();
}

static void force_stop(void)
{
    if (!g_vm)
        return;
    void (^done)(id) = ^(id error) {
      stop_completed(error);
    };
    void *blk = Block_copy(done);
    ((void(*)(id, SEL, void *))objc_msgSend)(g_vm, o_sel("stopWithCompletionHandler:"), blk);
}

static void guest_did_stop(id self, SEL _cmd, id vm)
{
    (void)self;
    (void)_cmd;
    (void)vm;
    g_running = 0;
    printf("vmfc: la VM si e' arrestata.\n");
    fflush(stdout);
    if (g_terminating) {
        reply_terminate_yes();
        return;
    }
    if (g_headless) {
        o_vmsg1(g_app, "terminate:", NULL);
        return;
    }
    if (g_window)
        o_vmsg1(g_window, "setTitle:", o_str("VM arrestata - GMBox"));
    if (g_cfg && !g_cfg->iso[0]) {
        show_alert("Macchina virtuale arrestata",
                   "Nessun sistema operativo avviabile. Scegli un'immagine ISO "
                   "dal menu Macchina virtuale, poi avvia di nuovo la VM.");
    }
}

static void did_stop_with_error(id self, SEL _cmd, id vm, id error)
{
    (void)self;
    (void)_cmd;
    (void)vm;
    g_running = 0;
    print_error("arresto anomalo della VM", error);
    if (g_terminating) {
        reply_terminate_yes();
        return;
    }
    show_alert("Errore della macchina virtuale",
               o_cstr(o_msg0(error, "localizedDescription")));
    if (g_headless)
        o_vmsg1(g_app, "terminate:", NULL);
}

static void request_shutdown(void)
{
    if (!g_vm || !g_running)
        return;
    id err = NULL;
    BOOL ok = o_bool_err(g_vm, "requestStopWithError:", &err);
    if (!ok) {
        print_error("richiesta di spegnimento non riuscita, forzo lo stop", err);
        force_stop();
        return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 25LL * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
                     if (g_running)
                         force_stop();
                   });
}

static void act_start(id self, SEL _cmd, id sender)
{
    (void)self;
    (void)_cmd;
    (void)sender;
    if (!g_vm || g_running)
        return;
    void (^done)(id) = ^(id error) {
      if (error) {
          print_error("avvio non riuscito", error);
          show_alert("Avvio non riuscito",
                     o_cstr(o_msg0(error, "localizedDescription")));
          o_vmsg1(g_app, "terminate:", NULL);
      } else {
          g_running = 1;
          printf("vmfc: VM avviata.\n");
          fflush(stdout);
      }
    };
    void *blk = Block_copy(done);
    ((void(*)(id, SEL, void *))objc_msgSend)(g_vm, o_sel("startWithCompletionHandler:"), blk);
}

static void act_stop(id self, SEL _cmd, id sender)
{
    (void)self;
    (void)_cmd;
    (void)sender;
    force_stop();
}

static void act_request_stop(id self, SEL _cmd, id sender)
{
    (void)self;
    (void)_cmd;
    (void)sender;
    request_shutdown();
}

static void act_pause(id self, SEL _cmd, id sender)
{
    (void)self;
    (void)_cmd;
    (void)sender;
    if (!g_vm)
        return;
    void (^done)(id) = ^(id error) {
      if (error)
          print_error("pausa non riuscita", error);
      else
          printf("vmfc: VM in pausa.\n");
    };
    void *blk = Block_copy(done);
    ((void(*)(id, SEL, void *))objc_msgSend)(g_vm, o_sel("pauseWithCompletionHandler:"), blk);
}

static void act_resume(id self, SEL _cmd, id sender)
{
    (void)self;
    (void)_cmd;
    (void)sender;
    if (!g_vm)
        return;
    void (^done)(id) = ^(id error) {
      if (error)
          print_error("ripresa non riuscita", error);
      else
          printf("vmfc: VM ripresa.\n");
    };
    void *blk = Block_copy(done);
    ((void(*)(id, SEL, void *))objc_msgSend)(g_vm, o_sel("resumeWithCompletionHandler:"), blk);
}

static void act_choose_iso(id self, SEL _cmd, id sender)
{
    (void)self;
    (void)_cmd;
    (void)sender;
    if (g_running) {
        show_alert("VM in esecuzione",
                   "Spegni la macchina virtuale prima di cambiare l'immagine ISO.");
        return;
    }
    id panel = ((id(*)(id, SEL))objc_msgSend)(o_cls("NSOpenPanel"), o_sel("openPanel"));
    o_set_bool(panel, "setCanChooseFiles:", YES);
    o_set_bool(panel, "setAllowsMultipleSelection:", NO);
    o_set_bool(panel, "setCanChooseDirectories:", NO);
    id types[3] = {o_str("iso"), o_str("img"), o_str("dmg")};
    o_vmsg1(panel, "setAllowedFileTypes:", o_array(types, 3));
    long r = ((long(*)(id, SEL))objc_msgSend)(panel, o_sel("runModal"));
    if (r != 1)
        return;
    id url = o_msg0(panel, "URL");
    id path = o_msg0(url, "path");
    const char *p = o_cstr(path);
    snprintf(g_cfg->iso, sizeof(g_cfg->iso), "%s", p);
    if (vm_save(g_cfg) == 0) {
        show_alert("Immagine ISO impostata",
                   "Riavvia la macchina virtuale per avviare l'installer.");
    }
}

static BOOL app_should_terminate_after_close(id self, SEL _cmd, id sender)
{
    (void)self;
    (void)_cmd;
    (void)sender;
    return YES;
}

static unsigned long app_should_terminate(id self, SEL _cmd, id sender)
{
    (void)self;
    (void)_cmd;
    (void)sender;
    if (g_running && !g_terminating) {
        g_terminating = 1;
        force_stop();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10LL * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
                         reply_terminate_yes();
                       });
        return 2;
    }
    return 1;
}

static void app_will_terminate(id self, SEL _cmd, id notification)
{
    (void)self;
    (void)_cmd;
    (void)notification;
    if (g_cfg)
        vm_pid_remove(g_cfg);
}

typedef struct {
    const char *name;
    IMP imp;
    const char *types;
} vmfc_method;

static Class register_class(const char *name, const vmfc_method *methods, size_t count)
{
    Class c = objc_allocateClassPair((Class)o_cls("NSObject"), name, 0);
    if (!c)
        return (Class)o_cls(name);
    for (size_t i = 0; i < count; i++)
        class_addMethod(c, o_sel(methods[i].name), methods[i].imp, methods[i].types);
    objc_registerClassPair(c);
    return c;
}

static id make_controller(void)
{
    static const vmfc_method methods[] = {
        {"startVM:", (IMP)act_start, "v@:@"},
        {"stopVM:", (IMP)act_stop, "v@:@"},
        {"requestStopVM:", (IMP)act_request_stop, "v@:@"},
        {"pauseVM:", (IMP)act_pause, "v@:@"},
        {"resumeVM:", (IMP)act_resume, "v@:@"},
        {"chooseISO:", (IMP)act_choose_iso, "v@:@"},
        {"applicationShouldTerminateAfterLastWindowClosed:", (IMP)app_should_terminate_after_close, "B@:@"},
        {"applicationShouldTerminate:", (IMP)app_should_terminate, "Q@:@"},
        {"applicationWillTerminate:", (IMP)app_will_terminate, "v@:@"},
    };
    Class c = register_class("VMFusionCloneController", methods,
                             sizeof(methods) / sizeof(methods[0]));
    return o_msg0((id)c, "new");
}

static id make_menu_item(const char *title, const char *action, const char *key, id target)
{
    id item = ((id(*)(id, SEL, id, SEL, id))objc_msgSend)(
        o_alloc("NSMenuItem"), o_sel("initWithTitle:action:keyEquivalent:"),
        o_str(title), action ? o_sel(action) : (SEL)0, o_str(key));
    if (target)
        o_vmsg1(item, "setTarget:", target);
    return item;
}

static void build_menus(void)
{
    id menubar = o_init("NSMenu");
    id app_item = o_init("NSMenuItem");
    o_vmsg1(menubar, "addItem:", app_item);
    o_vmsg1(g_app, "setMainMenu:", menubar);

    id app_menu = o_init("NSMenu");
    o_vmsg1(app_item, "setSubmenu:", app_menu);
    o_vmsg1(app_menu, "addItem:",
            make_menu_item("Informazioni su GMBox",
                           "orderFrontStandardAboutPanel:", "", g_app));
    o_vmsg1(app_menu, "addItem:", o_msg0(o_cls("NSMenuItem"), "separatorItem"));
    o_vmsg1(app_menu, "addItem:",
            make_menu_item("Esci da GMBox", "terminate:", "q", g_app));

    id vm_item = o_init("NSMenuItem");
    o_vmsg1(menubar, "addItem:", vm_item);
    id vm_menu = o_init("NSMenu");
    o_vmsg1(vm_item, "setSubmenu:", vm_menu);
    o_vmsg1(vm_item, "setTitle:", o_str("Macchina virtuale"));
    o_vmsg1(vm_menu, "addItem:", make_menu_item("Avvia", "startVM:", "r", g_controller));
    o_vmsg1(vm_menu, "addItem:", make_menu_item("Pausa", "pauseVM:", "p", g_controller));
    o_vmsg1(vm_menu, "addItem:", make_menu_item("Riprendi", "resumeVM:", "o", g_controller));
    o_vmsg1(vm_menu, "addItem:", make_menu_item("Spegni (chiedi al guest)", "requestStopVM:", "u", g_controller));
    o_vmsg1(vm_menu, "addItem:", make_menu_item("Spegni forzatamente", "stopVM:", ".", g_controller));
    o_vmsg1(vm_menu, "addItem:", o_msg0(o_cls("NSMenuItem"), "separatorItem"));
    o_vmsg1(vm_menu, "addItem:", make_menu_item("Scegli immagine ISO...", "chooseISO:", "i", g_controller));

    id win_item = o_init("NSMenuItem");
    o_vmsg1(menubar, "addItem:", win_item);
    id win_menu = o_init("NSMenu");
    o_vmsg1(win_item, "setSubmenu:", win_menu);
    o_vmsg1(win_item, "setTitle:", o_str("Finestra"));
    o_vmsg1(win_menu, "addItem:",
            make_menu_item("Schermo intero", "toggleFullScreen:", "f", NULL));
    o_vmsg1(win_menu, "addItem:", make_menu_item("Riduci a icona", "performMiniaturize:", "m", NULL));
}

static void build_window(void)
{
    vmfc_rect rect = {0, 0, 1280, 800};
    id view = ((id(*)(id, SEL, vmfc_rect))objc_msgSend)(
        o_alloc("VZVirtualMachineView"), o_sel("initWithFrame:"), rect);
    o_vmsg1(view, "setVirtualMachine:", g_vm);
    o_set_bool(view, "setCapturesSystemKeys:", YES);
    o_set_bool(view, "setAutomaticallyReconfiguresDisplay:", YES);

    id win = ((id(*)(id, SEL, vmfc_rect, unsigned long, unsigned long, BOOL))objc_msgSend)(
        o_alloc("NSWindow"),
        o_sel("initWithContentRect:styleMask:backing:defer:"),
        rect, 15UL, 2UL, NO);
    o_set_bool(win, "setReleasedWhenClosed:", NO);
    ((void(*)(id, SEL, vmfc_size))objc_msgSend)(
        win, o_sel("setContentMinSize:"), (vmfc_size){640, 400});
    o_vmsg1(win, "setContentView:", view);

    char title[256];
    snprintf(title, sizeof(title), "%s - GMBox", g_cfg->name);
    o_vmsg1(win, "setTitle:", o_str(title));
    o_vmsg0(win, "center");
    o_vmsg1(win, "makeKeyAndOrderFront:", NULL);
    g_window = win;
}

static int hex_to_bytes(const char *hex, unsigned char *out, size_t outcap, size_t *len_out)
{
    size_t n = strlen(hex);
    if (n == 0 || (n % 2) != 0)
        return -1;
    size_t bytes = n / 2;
    if (bytes > outcap)
        return -1;
    for (size_t i = 0; i < bytes; i++) {
        char hi = hex[i * 2], lo = hex[i * 2 + 1];
        int h = (hi >= '0' && hi <= '9') ? hi - '0' : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : -1;
        int l = (lo >= '0' && lo <= '9') ? lo - '0' : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : -1;
        if (h < 0 || l < 0)
            return -1;
        out[i] = (unsigned char)((h << 4) | l);
    }
    *len_out = bytes;
    return 0;
}

static void bytes_to_hex(const unsigned char *in, size_t len, char *out)
{
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = h[in[i] >> 4];
        out[i * 2 + 1] = h[in[i] & 15];
    }
    out[len * 2] = 0;
}

static id load_machine_identifier(vm_config *cfg)
{
    unsigned char buf[128];
    size_t len = 0;
    char *text = read_text_file(cfg->machine_id);
    id mi = NULL;
    if (text) {
        char *t = str_trim(text);
        if (hex_to_bytes(t, buf, sizeof(buf), &len) == 0) {
            mi = o_msg1(o_alloc("VZGenericMachineIdentifier"),
                        "initWithDataRepresentation:", o_data(buf, len));
        }
        free(text);
    }
    if (mi)
        return mi;

    mi = o_init("VZGenericMachineIdentifier");
    if (!mi)
        return NULL;
    id rep = o_msg0(mi, "dataRepresentation");
    const unsigned char *b = o_bytes(rep);
    unsigned long n = o_len(rep);
    if (b && n > 0 && n <= sizeof(buf)) {
        char hex[257];
        bytes_to_hex(b, n, hex);
        write_text_file(cfg->machine_id, hex);
    }
    return mi;
}

static void set_error(char *buf, size_t n, const char *what, id err)
{
    snprintf(buf, n, "%s: %s", what,
             err ? o_cstr(o_msg0(err, "localizedDescription")) : "errore sconosciuto");
}

static id build_configuration(vm_config *cfg, char *errbuf, size_t errlen)
{
    id err = NULL;

    id store = NULL;
    if (!path_exists(cfg->nvram)) {
        store = ((id(*)(id, SEL, id, unsigned long, id *))objc_msgSend)(
            o_alloc("VZEFIVariableStore"),
            o_sel("initCreatingVariableStoreAtURL:options:error:"),
            o_url(cfg->nvram), 0UL, &err);
        if (!store) {
            set_error(errbuf, errlen, "creazione NVRAM", err);
            return NULL;
        }
    } else {
        store = o_msg1(o_alloc("VZEFIVariableStore"), "initWithURL:", o_url(cfg->nvram));
        if (!store) {
            snprintf(errbuf, errlen, "impossibile aprire la NVRAM %s", cfg->nvram);
            return NULL;
        }
    }

    id boot = o_init("VZEFIBootLoader");
    o_vmsg1(boot, "setVariableStore:", store);

    id platform = o_init("VZGenericPlatformConfiguration");
    BOOL nested = ((BOOL(*)(id, SEL))objc_msgSend)(
        o_cls("VZGenericPlatformConfiguration"), o_sel("isNestedVirtualizationSupported"));
    if (nested)
        o_set_bool(platform, "setNestedVirtualizationEnabled:", YES);
    id machine_id = load_machine_identifier(cfg);
    if (!machine_id) {
        snprintf(errbuf, errlen, "impossibile creare il machine identifier");
        return NULL;
    }
    o_vmsg1(platform, "setMachineIdentifier:", machine_id);

    id conf = o_init("VZVirtualMachineConfiguration");
    o_vmsg1(conf, "setBootLoader:", boot);
    o_vmsg1(conf, "setPlatform:", platform);
    o_set_u64(conf, "setMemorySize:", cfg->memory_mib * 1024ULL * 1024ULL);
    o_set_ulong(conf, "setCPUCount:", (unsigned long)cfg->cpus);
    o_vmsg1(conf, "setLabel:", o_str(cfg->name));

    id entropy = o_init("VZVirtioEntropyDeviceConfiguration");
    id entropy_arr[1] = {entropy};
    o_vmsg1(conf, "setEntropyDevices:", o_array(entropy_arr, 1));

    id net = o_init("VZVirtioNetworkDeviceConfiguration");
    id mac = o_msg1(o_alloc("VZMACAddress"), "initWithString:", o_str(cfg->mac));
    if (!mac)
        mac = ((id(*)(id, SEL))objc_msgSend)(o_cls("VZMACAddress"), o_sel("randomLocallyAdministeredAddress"));
    o_vmsg1(net, "setMACAddress:", mac);
    o_vmsg1(net, "setAttachment:", o_init("VZNATNetworkDeviceAttachment"));
    id net_arr[1] = {net};
    o_vmsg1(conf, "setNetworkDevices:", o_array(net_arr, 1));

    id scanout = ((id(*)(id, SEL, long, long))objc_msgSend)(
        o_alloc("VZVirtioGraphicsScanoutConfiguration"),
        o_sel("initWithWidthInPixels:heightInPixels:"), 1920L, 1200L);
    id gfx = o_init("VZVirtioGraphicsDeviceConfiguration");
    id scanouts[1] = {scanout};
    o_vmsg1(gfx, "setScanouts:", o_array(scanouts, 1));
    id gfx_arr[1] = {gfx};
    o_vmsg1(conf, "setGraphicsDevices:", o_array(gfx_arr, 1));

    id kbd = o_init("VZUSBKeyboardConfiguration");
    id kbd_arr[1] = {kbd};
    o_vmsg1(conf, "setKeyboards:", o_array(kbd_arr, 1));
    id mouse = o_init("VZUSBScreenCoordinatePointingDeviceConfiguration");
    id mouse_arr[1] = {mouse};
    o_vmsg1(conf, "setPointingDevices:", o_array(mouse_arr, 1));

    id out_stream = o_init("VZVirtioSoundDeviceOutputStreamConfiguration");
    o_vmsg1(out_stream, "setSink:", o_init("VZHostAudioOutputStreamSink"));
    id stream_arr[1] = {out_stream};
    id sound = o_init("VZVirtioSoundDeviceConfiguration");
    o_vmsg1(sound, "setStreams:", o_array(stream_arr, 1));
    id audio_arr[1] = {sound};
    o_vmsg1(conf, "setAudioDevices:", o_array(audio_arr, 1));

    if (getenv("VMFC_SERIAL") && *getenv("VMFC_SERIAL")) {
        id in_h = ((id(*)(id, SEL, int, BOOL))objc_msgSend)(
            o_alloc("NSFileHandle"),
            o_sel("initWithFileDescriptor:closeOnDealloc:"),
            STDIN_FILENO, NO);
        id out_h = ((id(*)(id, SEL, int, BOOL))objc_msgSend)(
            o_alloc("NSFileHandle"),
            o_sel("initWithFileDescriptor:closeOnDealloc:"),
            STDOUT_FILENO, NO);
        id serial_att = o_msg2(o_alloc("VZFileHandleSerialPortAttachment"),
                              "initWithFileHandleForReading:fileHandleForWriting:",
                              in_h, out_h);
        id serial = o_init("VZVirtioConsoleDeviceSerialPortConfiguration");
        o_vmsg1(serial, "setAttachment:", serial_att);
        id serial_arr[1] = {serial};
        o_vmsg1(conf, "setSerialPorts:", o_array(serial_arr, 1));
    }

    err = NULL;
    id disk_att = ((id(*)(id, SEL, id, BOOL, id *))objc_msgSend)(
        o_alloc("VZDiskImageStorageDeviceAttachment"),
        o_sel("initWithURL:readOnly:error:"), o_url(cfg->disk), NO, &err);
    if (!disk_att) {
        set_error(errbuf, errlen, "apertura del disco", err);
        return NULL;
    }
    const char *usb_disk = getenv("VMFC_USB_DISK");
    id main_disk;
    if (usb_disk && *usb_disk == '1') {
        main_disk = o_msg1(o_alloc("VZUSBMassStorageDeviceConfiguration"),
                           "initWithAttachment:", disk_att);
    } else {
        main_disk = o_msg1(o_alloc("VZNVMExpressControllerDeviceConfiguration"),
                           "initWithAttachment:", disk_att);
    }
    id storage[3];
    size_t storage_count = 0;

    int iso_is_raw = str_ends_with(cfg->iso, ".raw") || str_ends_with(cfg->iso, ".img");
    BOOL iso_force_usb = getenv("VMFC_ISO_USB") && *getenv("VMFC_ISO_USB");
    BOOL iso_readonly = !(getenv("VMFC_RW_ISO") && *getenv("VMFC_RW_ISO"));
    id iso_dev = NULL;
    if (cfg->iso[0] && path_exists(cfg->iso)) {
        err = NULL;
        id iso_att = ((id(*)(id, SEL, id, BOOL, id *))objc_msgSend)(
            o_alloc("VZDiskImageStorageDeviceAttachment"),
            o_sel("initWithURL:readOnly:error:"), o_url(cfg->iso), iso_readonly, &err);
        if (!iso_att) {
            set_error(errbuf, errlen, "apertura della ISO", err);
            return NULL;
        }
        if (iso_is_raw && !iso_force_usb) {
            iso_dev = o_msg1(o_alloc("VZNVMExpressControllerDeviceConfiguration"),
                             "initWithAttachment:", iso_att);
        } else {
            iso_dev = o_msg1(o_alloc("VZUSBMassStorageDeviceConfiguration"),
                             "initWithAttachment:", iso_att);
        }
    }
    if (iso_is_raw && !iso_force_usb) {
        if (iso_dev)
            storage[storage_count++] = iso_dev;
        storage[storage_count++] = main_disk;
    } else {
        storage[storage_count++] = main_disk;
        if (iso_dev)
            storage[storage_count++] = iso_dev;
    }

    if (cfg->drivers[0] && path_exists(cfg->drivers)) {
        err = NULL;
        id drv_att = ((id(*)(id, SEL, id, BOOL, id *))objc_msgSend)(
            o_alloc("VZDiskImageStorageDeviceAttachment"),
            o_sel("initWithURL:readOnly:error:"), o_url(cfg->drivers), YES, &err);
        if (!drv_att) {
            set_error(errbuf, errlen, "apertura della ISO driver", err);
            return NULL;
        }
        id drv = o_msg1(o_alloc("VZUSBMassStorageDeviceConfiguration"),
                        "initWithAttachment:", drv_att);
        storage[storage_count++] = drv;
    }
    o_vmsg1(conf, "setStorageDevices:", o_array(storage, storage_count));

    if (cfg->shared_dir[0] && is_directory(cfg->shared_dir)) {
        id shared = ((id(*)(id, SEL, id, BOOL))objc_msgSend)(
            o_alloc("VZSharedDirectory"),
            o_sel("initWithURL:readOnly:"), o_url(cfg->shared_dir),
            cfg->shared_readonly ? YES : NO);
        id single = o_msg1(o_alloc("VZSingleDirectoryShare"),
                           "initWithDirectory:", shared);
        id fs = o_msg1(o_alloc("VZVirtioFileSystemDeviceConfiguration"),
                       "initWithTag:", o_str("share"));
        o_vmsg1(fs, "setShare:", single);
        id share_arr[1] = {fs};
        o_vmsg1(conf, "setDirectorySharingDevices:", o_array(share_arr, 1));
    }

    err = NULL;
    BOOL valid = o_bool_err(conf, "validateWithError:", &err);
    if (!valid) {
        set_error(errbuf, errlen, "configurazione non valida", err);
        return NULL;
    }
    return conf;
}

static void signal_trampoline(int sig)
{
    char c = (char)sig;
    ssize_t r = write(g_sigpipe[1], &c, 1);
    (void)r;
}

static void install_signal_handlers(void)
{
    if (pipe(g_sigpipe) != 0)
        return;
    fcntl(g_sigpipe[0], F_SETFL, O_NONBLOCK);
    fcntl(g_sigpipe[1], F_SETFL, O_NONBLOCK);
    signal(SIGTERM, signal_trampoline);
    signal(SIGINT, signal_trampoline);
    signal(SIGUSR1, signal_trampoline);
    signal(SIGUSR2, signal_trampoline);
    dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)g_sigpipe[0], 0,
                                                   dispatch_get_main_queue());
    dispatch_source_set_event_handler(src, ^{
      char buf[32];
      int terminate = 0, pause = 0, resume = 0;
      ssize_t n;
      while ((n = read(g_sigpipe[0], buf, sizeof(buf))) > 0) {
          for (ssize_t i = 0; i < n; i++) {
              if (buf[i] == SIGUSR1)
                  pause = 1;
              else if (buf[i] == SIGUSR2)
                  resume = 1;
              else
                  terminate = 1;
          }
      }
      if (pause)
          act_pause(NULL, NULL, NULL);
      if (resume)
          act_resume(NULL, NULL, NULL);
      if (!terminate || g_terminating)
          return;
      g_terminating = 1;
      request_shutdown();
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10LL * NSEC_PER_SEC),
                     dispatch_get_main_queue(), ^{
                       force_stop();
                       o_vmsg1(g_app, "terminate:", NULL);
                     });
    });
    dispatch_resume(src);
}

int vz_run(vm_config *cfg, int headless, int seconds)
{
    g_cfg = cfg;
    g_headless = headless;
    g_running = 0;
    g_terminating = 0;
    g_terminate_replied = 0;

    void *pool = objc_autoreleasePoolPush();
    g_app = o_msg0(o_cls("NSApplication"), "sharedApplication");
    if (!g_app) {
        fprintf(stderr, "vmfc: impossibile inizializzare NSApplication\n");
        objc_autoreleasePoolPop(pool);
        return 1;
    }
    ((void(*)(id, SEL, long))objc_msgSend)(g_app, o_sel("setActivationPolicy:"),
                                           headless ? 1L : 0L);
    g_controller = make_controller();
    o_vmsg1(g_app, "setDelegate:", g_controller);
    build_menus();

    char err[2048] = {0};
    id vzconf = build_configuration(cfg, err, sizeof(err));
    if (!vzconf) {
        fprintf(stderr, "vmfc: %s\n", err);
        show_alert("Impossibile avviare la macchina virtuale", err);
        objc_autoreleasePoolPop(pool);
        return 1;
    }

    g_vm = o_msg1(o_alloc("VZVirtualMachine"), "initWithConfiguration:", vzconf);
    if (!g_vm) {
        fprintf(stderr, "vmfc: impossibile creare VZVirtualMachine\n");
        objc_autoreleasePoolPop(pool);
        return 1;
    }

    static const vmfc_method delegate_methods[] = {
        {"guestDidStopVirtualMachine:", (IMP)guest_did_stop, "v@:@"},
        {"virtualMachine:didStopWithError:", (IMP)did_stop_with_error, "v@:@@",
         },
    };
    Class delegate_class = register_class("VMFusionCloneDelegate", delegate_methods,
                                          sizeof(delegate_methods) / sizeof(delegate_methods[0]));
    id delegate = o_msg0((id)delegate_class, "new");
    o_vmsg1(g_vm, "setDelegate:", delegate);

    if (!headless)
        build_window();

    vm_pid_write(cfg);
    vm_log_start(cfg, getpid());
    install_signal_handlers();

    act_start(g_controller, o_sel("startVM:"), NULL);

    if (!headless) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300LL * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
                         o_vmsg0(g_app, "activate");
                         o_set_bool(g_app, "activateIgnoringOtherApps:", YES);
                         if (g_window) {
                             o_vmsg1(g_window, "makeKeyAndOrderFront:", NULL);
                             o_vmsg0(g_window, "orderFrontRegardless");
                         }
                       });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500LL * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
                         o_vmsg0(g_app, "activate");
                         if (g_window)
                             o_vmsg0(g_window, "orderFrontRegardless");
                       });
    }

    if (seconds > 0) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)seconds * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
                         o_vmsg1(g_app, "terminate:", NULL);
                       });
    }

    o_vmsg0(g_app, "run");

    vm_pid_remove(cfg);
    g_vm = NULL;
    g_window = NULL;
    objc_autoreleasePoolPop(pool);
    return 0;
}
