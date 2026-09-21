/*
 * input_vita.c - PS Vita controller input handling for FZeroRecomp
 *
 * Based on patterns from sm64-vita's controller_vita.c
 *
 * Reads the Vita pad once per frame and maps it onto the runner's 12-bit
 * SNES controller word.
 *
 * F-Zero's control scheme drives the face-button choice: B accelerates and
 * Y brakes, so those sit on Cross and Square where a thumb rests. L/R are
 * the air brakes and map straight through to the Vita's L/R triggers.
 */

#include "input_vita.h"

#include <psp2/ctrl.h>
#include <string.h>

#include "platform.h"

/* ========================================================================
 * LOCAL DEFINITIONS
 * ======================================================================== */

/* SNES input button masks (12-bit serial controller format) */
/* Matches snesrecomp runner format: */
/* B=0x001, Y=0x002, SELECT=0x004, START=0x008, UP=0x010, DOWN=0x020, */
/* LEFT=0x040, RIGHT=0x080, A=0x100, X=0x200, L=0x400, R=0x800 */
#define SNES_INPUT_B       (1 << 0)   /* 0x001 */
#define SNES_INPUT_Y       (1 << 1)   /* 0x002 */
#define SNES_INPUT_SELECT  (1 << 2)   /* 0x004 */
#define SNES_INPUT_START   (1 << 3)   /* 0x008 */
#define SNES_INPUT_UP      (1 << 4)   /* 0x010 */
#define SNES_INPUT_DOWN    (1 << 5)   /* 0x020 */
#define SNES_INPUT_LEFT    (1 << 6)   /* 0x040 */
#define SNES_INPUT_RIGHT   (1 << 7)   /* 0x080 */
#define SNES_INPUT_A       (1 << 8)   /* 0x100 */
#define SNES_INPUT_X       (1 << 9)   /* 0x200 */
#define SNES_INPUT_L       (1 << 10)  /* 0x400 */
#define SNES_INPUT_R       (1 << 11)  /* 0x800 */

/* Player 1 connected indicator, as the desktop host reports it. */
#define SNES_CONTROLLER_P1_ACTIVE (1u << 30)

/* Vita analog stick: 0-255 per axis, nominal center at 128. */
#define ANALOG_CENTER 128

/* Radial deadzone, in stick units away from center. The Vita's sticks rest
 * a few units off center and drift with age, so this has to cover more than
 * a perfectly centered pad would need. */
#define ANALOG_DEADZONE 32

/* How far the stick must travel before it counts as a digital direction.
 * Above the deadzone but below this the stick is live yet still neutral,
 * which keeps a resting thumb from steering. */
#define ANALOG_DIGITAL_THRESHOLD 48

/* START + SELECT must be held this many frames (~1s at 60fps) to quit. */
#define QUIT_HOLD_FRAMES 60

/* ========================================================================
 * GLOBAL STATE
 * ======================================================================== */

static bool g_input_initialized = false;

/* The single pad sample for the current frame. */
static SceCtrlData g_ctrl;
static bool g_ctrl_valid = false;

/* Consecutive frames the quit combo has been held. */
static uint32_t g_quit_hold_frames = 0;

/* ========================================================================
 * INITIALIZATION
 * ======================================================================== */

void input_init(void) {
    if (g_input_initialized) {
        return;
    }

    FZERO_LOG("INFO", "Initializing Vita controller input");

    /* Analog-wide gives the sticks their full travel rather than the
     * clamped range the digital-only default reports. */
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

    memset(&g_ctrl, 0, sizeof(g_ctrl));
    g_ctrl.lx = ANALOG_CENTER;
    g_ctrl.ly = ANALOG_CENTER;
    g_ctrl.rx = ANALOG_CENTER;
    g_ctrl.ry = ANALOG_CENTER;
    g_ctrl_valid = false;
    g_quit_hold_frames = 0;

    g_input_initialized = true;
    FZERO_LOG("INFO", "Vita controller input initialized");
}

void input_shutdown(void) {
    if (!g_input_initialized) {
        return;
    }

    FZERO_LOG("INFO", "Shutting down Vita controller input");
    g_input_initialized = false;
}

/* ========================================================================
 * CONTROLLER READING
 * ======================================================================== */

bool input_poll(void) {
    if (!g_input_initialized) {
        input_init();
    }

    SceCtrlData ctrl;
    int ret = sceCtrlPeekBufferPositive(0, &ctrl, 1);
    if (ret < 0) {
        /* Log once per failure streak; a pad read that fails every frame
         * would otherwise flood the log at 60Hz. */
        if (g_ctrl_valid) {
            FZERO_LOG("ERROR", "sceCtrlPeekBufferPositive failed: 0x%08X", ret);
        }
        memset(&g_ctrl, 0, sizeof(g_ctrl));
        g_ctrl.lx = ANALOG_CENTER;
        g_ctrl.ly = ANALOG_CENTER;
        g_ctrl.rx = ANALOG_CENTER;
        g_ctrl.ry = ANALOG_CENTER;
        g_ctrl_valid = false;
        g_quit_hold_frames = 0;
        return false;
    }

    g_ctrl = ctrl;
    g_ctrl_valid = true;

    /* Track the quit combo here so the count advances exactly once a frame,
     * no matter how many times the frame asks about it. */
    if ((g_ctrl.buttons & SCE_CTRL_START) && (g_ctrl.buttons & SCE_CTRL_SELECT)) {
        if (g_quit_hold_frames < QUIT_HOLD_FRAMES) {
            g_quit_hold_frames++;
        }
    } else {
        g_quit_hold_frames = 0;
    }

    return true;
}

bool input_read_raw(SceCtrlData *ctrl) {
    if (!ctrl) {
        return false;
    }
    *ctrl = g_ctrl;
    return g_ctrl_valid;
}

bool input_get_analog_stick(int32_t *x, int32_t *y) {
    int32_t raw_x = (int32_t)g_ctrl.lx - ANALOG_CENTER;
    /* Screen-space Y grows downward; flip it so up is positive. */
    int32_t raw_y = ANALOG_CENTER - (int32_t)g_ctrl.ly;

    if (!g_ctrl_valid ||
        (raw_x * raw_x) + (raw_y * raw_y) < (ANALOG_DEADZONE * ANALOG_DEADZONE)) {
        if (x) *x = 0;
        if (y) *y = 0;
        return false;
    }

    if (x) *x = raw_x;
    if (y) *y = raw_y;
    return true;
}

uint32_t input_read(void) {
    /* Player 1 is always "connected" on a Vita — the pad is the console. */
    uint32_t inputs = SNES_CONTROLLER_P1_ACTIVE;

    if (!g_ctrl_valid) {
        return inputs;
    }

    /* Face buttons. F-Zero: B accelerates, Y brakes. */
    if (g_ctrl.buttons & SCE_CTRL_CROSS)    inputs |= SNES_INPUT_B;
    if (g_ctrl.buttons & SCE_CTRL_SQUARE)   inputs |= SNES_INPUT_Y;
    if (g_ctrl.buttons & SCE_CTRL_CIRCLE)   inputs |= SNES_INPUT_A;
    if (g_ctrl.buttons & SCE_CTRL_TRIANGLE) inputs |= SNES_INPUT_X;

    /* Shoulders: F-Zero's air brakes. */
    if (g_ctrl.buttons & SCE_CTRL_LTRIGGER) inputs |= SNES_INPUT_L;
    if (g_ctrl.buttons & SCE_CTRL_RTRIGGER) inputs |= SNES_INPUT_R;

    /* Start/Select. Withheld while the quit combo is being held so holding
     * it out doesn't also toggle the pause menu on the way to exiting. */
    if (g_quit_hold_frames == 0) {
        if (g_ctrl.buttons & SCE_CTRL_START)  inputs |= SNES_INPUT_START;
        if (g_ctrl.buttons & SCE_CTRL_SELECT) inputs |= SNES_INPUT_SELECT;
    }

    /* D-Pad. */
    if (g_ctrl.buttons & SCE_CTRL_UP)    inputs |= SNES_INPUT_UP;
    if (g_ctrl.buttons & SCE_CTRL_DOWN)  inputs |= SNES_INPUT_DOWN;
    if (g_ctrl.buttons & SCE_CTRL_LEFT)  inputs |= SNES_INPUT_LEFT;
    if (g_ctrl.buttons & SCE_CTRL_RIGHT) inputs |= SNES_INPUT_RIGHT;

    /* The left stick doubles as the d-pad, so either one can steer. */
    int32_t stick_x = 0, stick_y = 0;
    if (input_get_analog_stick(&stick_x, &stick_y)) {
        if (stick_x <= -ANALOG_DIGITAL_THRESHOLD) inputs |= SNES_INPUT_LEFT;
        if (stick_x >=  ANALOG_DIGITAL_THRESHOLD) inputs |= SNES_INPUT_RIGHT;
        if (stick_y >=  ANALOG_DIGITAL_THRESHOLD) inputs |= SNES_INPUT_UP;
        if (stick_y <= -ANALOG_DIGITAL_THRESHOLD) inputs |= SNES_INPUT_DOWN;
    }

    /* RtlRunFrame cancels opposing directions itself, so a d-pad press and a
     * stick push in opposite directions resolve there rather than here. */

    return inputs;
}

bool input_should_quit(void) {
    return g_quit_hold_frames >= QUIT_HOLD_FRAMES;
}
