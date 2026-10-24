/* platform_vita.c - PS Vita implementations of the platform functions. */

#include "platform.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <debugnet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>

typedef SceUID SceFileDescriptor;

static void platform_vita_create_directories(void);
static bool platform_vita_network_logging_init(void);
static void platform_vita_network_logging_shutdown(void);

static bool g_network_logging_initialized = false;
static bool g_screen_sleep_prevented = false;
static bool g_use_network_logging = true;
static FILE *g_log_file = NULL;

#define DEBUGNET_DEFAULT_IP "192.168.0.10"
#define DEBUGNET_DEFAULT_PORT 18194

void platform_init(void) {
    FZERO_LOG("INFO", "Initializing Vita platform");
    
    platform_vita_create_directories();
    
    if (platform_vita_network_logging_init()) {
        g_network_logging_initialized = true;
        FZERO_LOG("INFO", "Network logging initialized");
    } else {
        FZERO_LOG("WARN", "Network logging initialization failed, using stderr fallback");
    }
    
    platform_prevent_screen_sleep(true);

    /* Run the hardware at its maximum rated clocks. Homebrew starts at a
     * reduced system default (333 MHz ARM), not the 444 MHz retail games get,
     * and this port is CPU-bound, so the ARM clock scales the frame rate.
     *
     * These calls are advisory; the OS may refuse or throttle them (thermal,
     * low battery), so the clocks actually granted are read back and logged
     * rather than the requests being assumed to have taken. */
    {
        int r_arm = scePowerSetArmClockFrequency(444);
        int r_bus = scePowerSetBusClockFrequency(222);
        int r_gpu = scePowerSetGpuClockFrequency(222);
        int r_xbar = scePowerSetGpuXbarClockFrequency(166);
        FZERO_LOG("INFO", "Clocks requested: ARM=%d MHz (0x%08X) bus=%d (0x%08X) "
                          "gpu=%d (0x%08X) xbar=%d (0x%08X)",
                  scePowerGetArmClockFrequency(), r_arm,
                  scePowerGetBusClockFrequency(), r_bus,
                  scePowerGetGpuClockFrequency(), r_gpu,
                  scePowerGetGpuXbarClockFrequency(), r_xbar);
    }
}

void platform_shutdown(void) {
    FZERO_LOG("INFO", "Shutting down Vita platform");
    
    platform_prevent_screen_sleep(false);
    
    if (g_network_logging_initialized) {
        platform_vita_network_logging_shutdown();
        g_network_logging_initialized = false;
    }
}

void platform_ensure_directories(void) {
    platform_vita_create_directories();
}

static void platform_vita_create_directories(void) {
    static const char *directories[] = {
        "ux0:/data",
        "ux0:/data/fzero_recomp",
        "ux0:/data/fzero_recomp/saves",
        "ux0:/data/fzero_recomp/screenshots",
        NULL
    };
    
    for (int i = 0; directories[i] != NULL; i++) {
        SceIoStat stat;
        if (sceIoGetstat(directories[i], &stat) < 0) {
            if (sceIoMkdir(directories[i], 0777) < 0) {
                FZERO_LOG("WARN", "Failed to create directory: %s", directories[i]);
            } else {
                FZERO_LOG("DEBUG", "Created directory: %s", directories[i]);
            }
        }
    }
}

bool platform_file_exists(const char *path) {
    SceIoStat stat;
    return sceIoGetstat(path, &stat) >= 0;
}

int64_t platform_file_size(const char *path) {
    SceIoStat stat;
    if (sceIoGetstat(path, &stat) < 0) {
        return -1;
    }
    return (int64_t)stat.st_size;
}

bool platform_read_file(const char *path, void **buffer, size_t *size) {
    SceFileDescriptor fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        FZERO_LOG("ERROR", "Failed to open file for reading: %s", path);
        return false;
    }
    
    SceIoStat stat;
    if (sceIoGetstat(path, &stat) < 0) {
        sceIoClose(fd);
        return false;
    }
    
    size_t file_size = (size_t)stat.st_size;
    void *buf = FZERO_MALLOC(file_size);
    if (!buf) {
        sceIoClose(fd);
        return false;
    }
    
    ssize_t bytes_read = sceIoRead(fd, buf, file_size);
    sceIoClose(fd);
    
    if (bytes_read < 0 || (size_t)bytes_read != file_size) {
        FZERO_FREE(buf);
        return false;
    }
    
    *buffer = buf;
    *size = file_size;
    return true;
}

bool platform_write_file(const char *path, const void *buffer, size_t size) {
    platform_ensure_directories();
    
    SceFileDescriptor fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        FZERO_LOG("ERROR", "Failed to open file for writing: %s", path);
        return false;
    }
    
    ssize_t bytes_written = sceIoWrite(fd, buffer, size);
    sceIoClose(fd);
    
    if (bytes_written < 0 || (size_t)bytes_written != size) {
        return false;
    }
    
    return true;
}

uint64_t platform_get_time_ms(void) {
    uint64_t ticks = sceKernelGetProcessTimeWide();
    /* The tick is 1 us. */
    return ticks / 1000;
}

void platform_sleep_ms(uint32_t ms) {
    sceKernelDelayThread(ms * 1000);
}

void platform_prevent_screen_sleep(bool prevent) {
    if (prevent && !g_screen_sleep_prevented) {
        scePowerRequestDisplayOn();
        g_screen_sleep_prevented = true;
        FZERO_LOG("INFO", "Screen sleep prevented");
    } else if (!prevent && g_screen_sleep_prevented) {
        scePowerRequestDisplayOff();
        g_screen_sleep_prevented = false;
        FZERO_LOG("INFO", "Screen sleep restored");
    }
}

int platform_get_battery_level(void) {
    int level = scePowerGetBatteryLifePercent();
    if (level < 0) {
        return 100; /* unknown: report full */
    }
    return level;
}

bool platform_is_charging(void) {
    return scePowerIsBatteryCharging() > 0;
}

static int platform_vita_log_level_to_debugnet(const char *tag) {
    if (strcmp(tag, "ERROR") == 0) return ERROR;
    if (strcmp(tag, "WARN") == 0) return ERROR;
    if (strcmp(tag, "INFO") == 0) return INFO;
    if (strcmp(tag, "DEBUG") == 0) return DEBUG;
    return INFO;
}

static bool platform_vita_network_logging_init(void) {
#if FZERO_LOG_FILE
    /* Vita3K has no working sceNet: sceNetSyscallSocket is unimplemented, and
     * debugNetInit faults inside libnet.suprx (a `ldr r1, [r0]` on a NULL that
     * the failed syscall returned) before any line is logged, and the app does
     * not boot. Under the emulator, skip it and log to the file below. */
    g_use_network_logging = false;
    g_log_file = fopen("ux0:/data/fzero_recomp/log.txt", "w");
    return g_log_file != NULL;
#else
    const char *server_ip = getenv("PSVITAIP");
    if (!server_ip || server_ip[0] == '\0') {
        server_ip = DEBUGNET_DEFAULT_IP;
    }

    /* debugNetInit internally loads SCE_SYSMODULE_NET, allocates the 1MB network pool,
     * calls sceNetInit/sceNetCtlInit, and sets up the UDP socket.
     * Use DEBUG level so that INFO, ERROR, and DEBUG messages pass through. */
    int debugnet_result = debugNetInit(server_ip, DEBUGNET_DEFAULT_PORT, DEBUG);
    
    if (debugnet_result == 1) {
        g_use_network_logging = true;
        FZERO_LOG("INFO", "Vita network logging initialized - sending to %s:%d", 
                  server_ip, DEBUGNET_DEFAULT_PORT);
        return true;
    } else {
        g_use_network_logging = false;
        FZERO_LOG("WARN", "debugNetInit failed (result=%d), trying file logging fallback", debugnet_result);
        
        g_log_file = fopen("ux0:/data/fzero_recomp/log.txt", "a");
        if (g_log_file) {
            FZERO_LOG("INFO", "File logging fallback initialized: ux0:/data/fzero_recomp/log.txt");
            return true;
        } else {
            FZERO_LOG("ERROR", "File logging fallback also failed, using stderr only");
            return false;
        }
    }
#endif
}

static void platform_vita_network_logging_shutdown(void) {
    if (g_use_network_logging) {
        debugNetFinish();
        g_use_network_logging = false;
    }
    
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

void platform_vita_log(const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (g_network_logging_initialized && g_use_network_logging) {
        int level = platform_vita_log_level_to_debugnet(tag);
        /* One UDP packet, with debugNet's [VITA][<level>]: prefix. */
        debugNetPrintf(level, "%s\n", buffer);
        return;
    }
    
    if (g_log_file) {
        fprintf(g_log_file, "[%s] %s\n", tag, buffer);
        fflush(g_log_file);
        return;
    }
    
    fprintf(stderr, "[%s] %s\n", tag, buffer);
    fflush(stderr);
}

void platform_vita_logging_init(void) {
    if (platform_vita_network_logging_init()) {
        g_network_logging_initialized = true;
    }
}

void platform_vita_logging_shutdown(void) {
    if (g_network_logging_initialized) {
        platform_vita_network_logging_shutdown();
        g_network_logging_initialized = false;
    }
}

void Die(const char *error) {
    platform_vita_log("FATAL", "%s", error);
    sceKernelExitProcess(1);
}

FILE *popen(const char *command, const char *mode) {
    (void)command; (void)mode;
    platform_vita_log("WARNING", "popen called but not implemented on Vita");
    return NULL;
}

int pclose(FILE *stream) {
    (void)stream;
    platform_vita_log("WARNING", "pclose called but not implemented on Vita");
    return -1;
}
