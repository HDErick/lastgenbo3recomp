/* Diagnostic startup only: preserve libnx services, but mount SD before AM/window.
 * Sequence checked against libnx runtime/init.c and the linked baseline __appInit.
 * No C++ objects, logger, Vulkan or guest code are needed to write this trace.
 */
#include <switch.h>
#include <stdio.h>
#include <sys/stat.h>

extern u32 __nx_applet_type;
void __libnx_init_time(void);
void __libnx_init_cwd(void);
void __nx_win_init(void) __attribute__((weak));
void userAppInit(void) __attribute__((weak));
void __real___libc_init_array(void);
int __real_main(int argc, char **argv);

static const char *trace_path = "sdmc:/switch/bo3recomp/bo3_nx_startup.log";

static void trace(const char *stage, Result result) {
    /* Close every entry so an immediate crash leaves the last completed stage. */
    FILE *file = fopen(trace_path, "a");
    if (!file) return;
    fprintf(file, "%llu %s result=0x%08x\n",
            (unsigned long long)armGetSystemTick(), stage, (unsigned)result);
    fflush(file);
    fclose(file);
}

static void require_service(const char *stage, Result result) {
    trace(stage, result);
    if (R_FAILED(result)) diagAbortWithResult(result);
}

void __appInit(void) {
    /* SM/FS must work before a file can be created. Keep their raw failure code. */
    Result rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(rc);
    rc = fsInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(rc);
    rc = fsdevMountSdmc();
    if (R_FAILED(rc)) diagAbortWithResult(rc);
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/bo3recomp", 0777);
    FILE *file = fopen(trace_path, "w");
    if (!file) diagAbortWithResult(MAKERESULT(Module_Libnx, 105));
    fputs("BO3 NX startup trace01: reached __appInit; SM/FS/SD ready\n", file);
    fclose(file);
    trace("heap override present", envHasHeapOverride() ? 1 : 0);
    trace("configured applet type", __nx_applet_type);

    if (hosversionGet() == 0) {
        rc = setsysInitialize();
        trace("setsysInitialize", rc);
        if (R_SUCCEEDED(rc)) {
            SetSysFirmwareVersion fw;
            rc = setsysGetFirmwareVersion(&fw);
            trace("setsysGetFirmwareVersion", rc);
            if (R_SUCCEEDED(rc))
                hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            setsysExit();
        }
    }
    trace("before appletInitialize", 0);
    require_service("appletInitialize", appletInitialize());
    if (__nx_applet_type != AppletType_None)
        require_service("hidInitialize", hidInitialize());
    require_service("timeInitialize", timeInitialize());
    __libnx_init_time();
    __libnx_init_cwd();
    trace("before window initialization", 0);
    if (__nx_win_init) __nx_win_init();
    trace("after window initialization", 0);
    if (userAppInit) userAppInit();
    trace("__appInit complete", 0);
}

void __wrap___libc_init_array(void) {
    trace("before static constructors", 0);
    __real___libc_init_array();
    trace("after static constructors", 0);
}

int __wrap_main(int argc, char **argv) {
    trace("entered main", 0);
    int result = __real_main(argc, argv);
    trace("main returned", (Result)result);
    return result;
}
