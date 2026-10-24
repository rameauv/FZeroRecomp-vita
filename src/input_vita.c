/*
 * input_vita.c - PS Vita controller input for FZeroRecomp.
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

/* The runner's 12-bit SNES controller word. */
#define SNES_INPUT_B       (1 << 0)
#define SNES_INPUT_Y       (1 << 1)
#define SNES_INPUT_SELECT  (1 << 2)
#define SNES_INPUT_START   (1 << 3)
#define SNES_INPUT_UP      (1 << 4)
#define SNES_INPUT_DOWN    (1 << 5)
#define SNES_INPUT_LEFT    (1 << 6)
#define SNES_INPUT_RIGHT   (1 << 7)
#define SNES_INPUT_A       (1 << 8)
#define SNES_INPUT_X       (1 << 9)
#define SNES_INPUT_L       (1 << 10)
#define SNES_INPUT_R       (1 << 11)

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

/* L + R + START must be held this many frames (~0.17s at 60fps) to soft-reset
 * the SNES. F-Zero sometimes locks up with no way back to the title screen. */
#define RESET_HOLD_FRAMES 10

/* START + SELECT held this many frames (~0.08s at 60fps) writes battery SRAM
 * to disk on demand. SELECT is unused by F-Zero, so the combo can't collide
 * with gameplay. */
#define SAVE_HOLD_FRAMES 5

static bool g_input_initialized = false;

/* The single pad sample for the current frame. */
static SceCtrlData g_ctrl;
static bool g_ctrl_valid = false;

/* Consecutive frames the reset combo has been held. */
static uint32_t g_reset_hold_frames = 0;

/* Set when the reset combo fires; stays set until L, R and START are all
 * released. While set, the combo counter cannot re-accumulate (the player is
 * usually still holding the buttons when RtlReset returns) and input_read
 * reports neutral, so the held START does not leak into the freshly reset
 * game as a phantom press. */
static bool g_reset_latched = false;

/* Consecutive frames the save combo (START + SELECT) has been held. */
static uint32_t g_save_hold_frames = 0;

/* Set when the save combo fires; stays set until START and SELECT are both
 * released, so a held combo writes SRAM once instead of once every
 * SAVE_HOLD_FRAMES frames. Unlike the reset latch this does not suppress
 * input_read: the guest still sees START/SELECT. */
static bool g_save_latched = false;

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
    g_reset_hold_frames = 0;
    g_reset_latched = false;
    g_save_hold_frames = 0;
    g_save_latched = false;

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
        g_reset_hold_frames = 0;
        g_save_hold_frames = 0;
        return false;
    }

    g_ctrl = ctrl;
    g_ctrl_valid = true;

    /* Track the reset combo (L + R + START) so the count advances exactly
     * once a frame. While the latch is set the counter stays at zero: the
     * player is still holding the combo when the reset fires, and without
     * the latch the count would climb back to RESET_HOLD_FRAMES and fire a
     * second reset ~10 frames later. The latch clears only once all three
     * buttons are released. */
    bool l_held = (g_ctrl.buttons & SCE_CTRL_LTRIGGER) != 0;
    bool r_held = (g_ctrl.buttons & SCE_CTRL_RTRIGGER) != 0;
    bool start_held = (g_ctrl.buttons & SCE_CTRL_START) != 0;

    if (g_reset_latched) {
        if (!l_held && !r_held && !start_held) {
            g_reset_latched = false;
        }
        g_reset_hold_frames = 0;
    } else if (l_held && r_held && start_held) {
        if (g_reset_hold_frames < RESET_HOLD_FRAMES) {
            g_reset_hold_frames++;
        }
    } else {
        g_reset_hold_frames = 0;
    }

    /* Track the save combo (START + SELECT) with its own latch, so a held
     * combo writes SRAM once instead of once every SAVE_HOLD_FRAMES frames.
     * The latch clears as soon as START or SELECT is released. */
    bool select_held = (g_ctrl.buttons & SCE_CTRL_SELECT) != 0;
    if (g_save_latched) {
        if (!start_held || !select_held) {
            g_save_latched = false;
        }
        g_save_hold_frames = 0;
    } else if (start_held && select_held) {
        if (g_save_hold_frames < SAVE_HOLD_FRAMES) {
            g_save_hold_frames++;
        }
    } else {
        g_save_hold_frames = 0;
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

    /* While the reset latch is held the pad still has L/R/START down; report
     * neutral so those presses don't reach the freshly reset game. */
    if (!g_ctrl_valid || g_reset_latched) {
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

    /* Start/Select. */
    if (g_ctrl.buttons & SCE_CTRL_START)  inputs |= SNES_INPUT_START;
    if (g_ctrl.buttons & SCE_CTRL_SELECT) inputs |= SNES_INPUT_SELECT;

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

bool input_should_reset(void) {
    if (g_reset_hold_frames >= RESET_HOLD_FRAMES) {
        g_reset_hold_frames = 0; /* edge-trigger: fire once per hold */
        /* Latch until the combo is fully released so the still-held buttons
         * can neither re-fire the reset nor reach the guest input word. */
        g_reset_latched = true;
        return true;
    }
    return false;
}

bool input_should_save(void) {
    if (g_save_hold_frames >= SAVE_HOLD_FRAMES) {
        g_save_hold_frames = 0; /* edge-trigger: fire once per hold */
        /* Latch until the combo is released so the write does not repeat
         * every SAVE_HOLD_FRAMES frames while the buttons stay held. */
        g_save_latched = true;
        return true;
    }
    return false;
}
