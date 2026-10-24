/* input_vita.h - PS Vita controller input for FZeroRecomp. */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <psp2/ctrl.h>

void input_init(void);

void input_shutdown(void);

/*
 * Sample the pad once for this frame.
 *
 * Every other query below reads the cached sample, so the whole frame sees
 * one consistent controller state. Call this once at the top of each frame,
 * before input_read()/input_should_reset().
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
 * Whether the player asked for a soft reset.
 *
 * The combo is L + R + START held for RESET_HOLD_FRAMES consecutive frames.
 * Edge-triggered: returns true once per hold, then resets the counter so the
 * caller doesn't need to track release.
 *
 * @return true once the combo has been held long enough
 */
bool input_should_reset(void);

/*
 * Whether the player asked for a manual save.
 *
 * The combo is START + SELECT held for SAVE_HOLD_FRAMES consecutive frames.
 * Edge-triggered: returns true once per hold, then resets the counter so the
 * caller doesn't need to track release.
 *
 * @return true once the combo has been held long enough
 */
bool input_should_save(void);
