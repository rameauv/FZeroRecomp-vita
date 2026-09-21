/*
 * platform.h - PS Vita platform definitions for FZeroRecomp
 *
 * Simplified for Vita-only builds.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ========================================================================
 * PLATFORM DETECTION
 * ======================================================================== */

#define VITA 1
#define __vita__ 1

/* ========================================================================
 * PATH HANDLING
 * ======================================================================== */

/* Vita uses ux0: partition for user data */
#define FZERO_BASE_PATH "ux0:/data/fzero_recomp/"
#define FZERO_ROM_PATH  FZERO_BASE_PATH "fzero.sfc"
#define FZERO_SAVE_PATH FZERO_BASE_PATH "save.srm"
#define FZERO_CONFIG_PATH FZERO_BASE_PATH "config.ini"
#define FZERO_KEYBINDS_PATH FZERO_BASE_PATH "keybinds.ini"
#define FZERO_SCREENSHOTS_PATH FZERO_BASE_PATH "screenshots/"
#define FZERO_SAVES_PATH FZERO_BASE_PATH "saves/"

/* ========================================================================
 * FILE SYSTEM ABSTRACTION
 * ======================================================================== */

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

/* ========================================================================
 * MEMORY ALLOCATION
 * ======================================================================== */

#include <psp2/kernel/processmgr.h>

#define FZERO_MALLOC(size) malloc(size)
#define FZERO_FREE(ptr) free(ptr)
#define FZERO_CALLOC(n, size) calloc(n, size)
#define FZERO_REALLOC(ptr, size) realloc(ptr, size)

/* ========================================================================
 * THREADING
 * ======================================================================== */

#include <psp2/kernel/threadmgr.h>

#define FZERO_THREAD_RET_TYPE SceUID
#define FZERO_THREAD_FUNC_RET SCE_KERNEL_START_SUCCESS
#define FZERO_THREAD_FUNC void *

/* ========================================================================
 * LOGGING
 * ======================================================================== */

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <debugnet.h>

/* Forward declarations for Vita logging functions */
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

/* ========================================================================
 * AUDIO BACKEND
 * ======================================================================== */

#define FZERO_AUDIO_BACKEND_LIBAUDIO 1

/* ========================================================================
 * RENDERER BACKEND
 * ======================================================================== */

#define FZERO_RENDERER_SDL2 1
#define FZERO_RENDERER_GLES 1

/* ========================================================================
 * INPUT BACKEND
 * ======================================================================== */

#define FZERO_INPUT_SDL2 1

/* ========================================================================
 * DISPLAY CONSTANTS
 * ======================================================================== */

/* PS Vita screen resolution */
#define FZERO_VITA_SCREEN_WIDTH  960
#define FZERO_VITA_SCREEN_HEIGHT 544

/* Resolutions live in display_layout.h, which defines them as an enum shared
 * with the layers pipeline. Four macros duplicating them used to sit here with
 * no users; they shadowed the enum and broke any translation unit that
 * included both (`enum { FZERO_WIDE_WIDTH = 398 }` after a #define of the same
 * name is `398 = 398`). Do not reintroduce them. */

/* ========================================================================
 * PLATFORM INITIALIZATION
 * ======================================================================== */

/* Initialize platform-specific systems */
void platform_init(void);

/* Shutdown platform-specific systems */
void platform_shutdown(void);

/* Ensure required directories exist */
void platform_ensure_directories(void);

/* Check if running on Vita - always true */
static inline bool platform_is_vita(void) {
    return true;
}

/* Check if running on desktop - always false */
static inline bool platform_is_desktop(void) {
    return false;
}

/* ========================================================================
 * FILE UTILITIES
 * ======================================================================== */

/* Check if a file exists */
bool platform_file_exists(const char *path);

/* Get file size */
int64_t platform_file_size(const char *path);

/* Read entire file into buffer */
bool platform_read_file(const char *path, void **buffer, size_t *size);

/* Write entire buffer to file */
bool platform_write_file(const char *path, const void *buffer, size_t size);

/* ========================================================================
 * TIME UTILITIES
 * ======================================================================== */

/* Get current time in milliseconds */
uint64_t platform_get_time_ms(void);

/* Sleep for specified milliseconds */
void platform_sleep_ms(uint32_t ms);

/* ========================================================================
 * POWER MANAGEMENT (Vita-specific)
 * ======================================================================== */

/* Prevent screen from sleeping */
void platform_prevent_screen_sleep(bool prevent);

/* Check battery level (0-100) */
int platform_get_battery_level(void);

/* Check if charging */
bool platform_is_charging(void);
