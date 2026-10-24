/* platform.h - PS Vita platform definitions for FZeroRecomp. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VITA 1
#define __vita__ 1

#define FZERO_BASE_PATH "ux0:/data/fzero_recomp/"
#define FZERO_ROM_PATH  FZERO_BASE_PATH "fzero.sfc"
#define FZERO_SAVE_PATH FZERO_BASE_PATH "save.srm"
#define FZERO_CONFIG_PATH FZERO_BASE_PATH "config.ini"
#define FZERO_KEYBINDS_PATH FZERO_BASE_PATH "keybinds.ini"
#define FZERO_SCREENSHOTS_PATH FZERO_BASE_PATH "screenshots/"
#define FZERO_SAVES_PATH FZERO_BASE_PATH "saves/"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>

#define FZERO_OPEN(path, flags, ...) sceIoOpen(path, flags, __VA_ARGS__)
#define FZERO_READ(fd, buf, size) sceIoRead(fd, buf, size)
#define FZERO_WRITE(fd, buf, size) sceIoWrite(fd, buf, size)
#define FZERO_CLOSE(fd) sceIoClose(fd)
#define FZERO_SEEK(fd, offset, whence) sceIoLseek(fd, offset, whence)
#define FZERO_STAT(path, buf) sceIoGetstat(path, buf)
#define FZERO_MKDIR(path, mode) sceIoMkdir(path, mode)

#include <psp2/kernel/processmgr.h>

#define FZERO_MALLOC(size) malloc(size)
#define FZERO_FREE(ptr) free(ptr)
#define FZERO_CALLOC(n, size) calloc(n, size)
#define FZERO_REALLOC(ptr, size) realloc(ptr, size)

#include <psp2/kernel/threadmgr.h>

#define FZERO_THREAD_RET_TYPE SceUID
#define FZERO_THREAD_FUNC_RET SCE_KERNEL_START_SUCCESS
#define FZERO_THREAD_FUNC void *

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <debugnet.h>

void platform_vita_log(const char *tag, const char *fmt, ...);
void platform_vita_logging_init(void);
void platform_vita_logging_shutdown(void);

#define FZERO_LOG_INIT() platform_vita_logging_init()
#define FZERO_LOG_SHUTDOWN() platform_vita_logging_shutdown()
#define FZERO_LOG(tag, fmt, ...) platform_vita_log(tag, fmt, ##__VA_ARGS__)
#define FZERO_LOG_ERROR(fmt, ...) platform_vita_log("ERROR", fmt, ##__VA_ARGS__)
#define FZERO_LOG_WARN(fmt, ...) platform_vita_log("WARN", fmt, ##__VA_ARGS__)
#define FZERO_LOG_INFO(fmt, ...) platform_vita_log("INFO", fmt, ##__VA_ARGS__)
#define FZERO_LOG_DEBUG(fmt, ...) platform_vita_log("DEBUG", fmt, ##__VA_ARGS__)

#define FZERO_AUDIO_BACKEND_LIBAUDIO 1

#define FZERO_RENDERER_SDL3 1
#define FZERO_RENDERER_GLES 1

#define FZERO_INPUT_SDL3 1

#define FZERO_VITA_SCREEN_WIDTH  960
#define FZERO_VITA_SCREEN_HEIGHT 544

/* Resolutions live in display_layout.h, as an enum shared with the layers
 * pipeline. Do not define them as macros here: a macro of the same name breaks
 * the enum (`enum { FZERO_WIDE_WIDTH = 398 }` becomes `398 = 398`). */

void platform_init(void);

void platform_shutdown(void);

void platform_ensure_directories(void);

static inline bool platform_is_vita(void) {
    return true;
}

static inline bool platform_is_desktop(void) {
    return false;
}

bool platform_file_exists(const char *path);

int64_t platform_file_size(const char *path);

bool platform_read_file(const char *path, void **buffer, size_t *size);

bool platform_write_file(const char *path, const void *buffer, size_t size);

uint64_t platform_get_time_ms(void);

void platform_sleep_ms(uint32_t ms);

void platform_prevent_screen_sleep(bool prevent);

/* 0-100 */
int platform_get_battery_level(void);

bool platform_is_charging(void);
