/*
 * input_vita.h - PS Vita controller input handling for FZeroRecomp
 *
 * Based on patterns from sm64-vita's controller_vita.h
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <psp2/ctrl.h>

/* ========================================================================
 * INITIALIZATION
 * ======================================================================== */

/* Initialize controller input system */
void input_init(void);

/* Shutdown controller input system */
void input_shutdown(void);

/* ========================================================================
 * INPUT READING
 * ======================================================================== */

/*
 * Sample the pad once for this frame.
 *
 * Every other query below reads the cached sample, so the whole frame sees
 * one consistent controller state. Call this once at the top of each frame,
 * before input_read()/input_should_quit().
 *
 * @return true if the sample succeeded; on failure the cached state is
 *         cleared to neutral and the frame runs with no buttons pressed.
 */
bool input_poll(void);

/*
 * Read the cached raw controller data for this frame.
 *
 * @param ctrl Output parameter for controller data
 * @return true if the last input_poll() succeeded
 */
bool input_read_raw(SceCtrlData *ctrl);

/*
 * Map the cached controller state to the runner's input word.
 *
 * @return Player 1 buttons in the low 12 bits (the RtlRunFrame layout:
 *         B=0x001, Y=0x002, SELECT=0x004, START=0x008, UP=0x010, DOWN=0x020,
 *         LEFT=0x040, RIGHT=0x080, A=0x100, X=0x200, L=0x400, R=0x800),
 *         plus bit 30 marking player 1 as connected.
 */
uint32_t input_read(void);

/*
 * Read the cached analog stick position.
 *
 * @param x Output for X axis (-128 to 127, 0 at center, right positive)
 * @param y Output for Y axis (-128 to 127, 0 at center, up positive)
 * @return true if the stick is outside the deadzone
 */
bool input_get_analog_stick(int32_t *x, int32_t *y);

/*
 * Check whether the user asked to quit.
 *
 * The combo is START + SELECT held for QUIT_HOLD_FRAMES consecutive frames,
 * so a stray press during a race can't drop the player out of the game.
 * While the combo is held, input_read() withholds START and SELECT from the
 * game so the hold doesn't also toggle pause.
 *
 * @return true once the combo has been held long enough
 */
bool input_should_quit(void);
