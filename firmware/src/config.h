#pragma once

// -----------------------------------------------------------------------------
// morse-code — board + behaviour configuration
// IdeaSpark ESP32-WROOM + on-board 1.9" ST7789 (170 x 320)
// -----------------------------------------------------------------------------

// --- Display (fixed on-board wiring, no jumpers) -----------------------------
#define LCD_BL_PIN        32
#define SCREEN_W          170
#define SCREEN_H          320

// 0 = USB at the bottom, 2 = flipped 180 deg.
// The IdeaSpark panel is usually mounted upside-down relative to its native
// rotation; 2 matches the reference esp-status project. Use 0 if upside-down.
#define DISPLAY_ROTATION  2

// 0..255. 128 is the esp-status default; 160 gives the green tree a little pop.
#define LCD_BRIGHTNESS    160

// --- Buttons -----------------------------------------------------------------
// The IdeaSpark 1.9" board exposes exactly two buttons:
//   BOOT  -> GPIO0  (readable, active LOW)
//   RESET -> EN     (hardware reset; cannot be read by software)
//
// Input  = BOOT  (tap = dot, hold = dash)
// Clear  = RESET (hardware reset -> RAM is cleared -> text resets), or the
//          optional second GPIO below, or a >=1.5 s hold of the BOOT button.
#define BTN_INPUT_PIN     0
#define BTN_ACTIVE_LOW    1

// Wire a second push button between this pin and GND to get a dedicated,
// non-rebooting "clear text" button. Set to -1 to use the RESET/EN button and
// the 1.5 s BOOT hold instead.
//
// Pick a pin with an internal pull-up (27, 26, 25, 33, ...). GPIO34-39 are
// input-only and have no internal pull-up, so they need an external 10k.
#define BTN_CLEAR_PIN     -1

// --- Morse timing (milliseconds) ---------------------------------------------
#define DEBOUNCE_MS       20   // button debounce
#define DOT_MAX_MS        220  // release before this = DOT
#define CLEAR_HOLD_MS     1500 // hold longer than this = clear the text
#define LETTER_GAP_MS     700  // silence after a symbol -> commit the letter
#define WORD_GAP_MS       4000 // silence after a letter -> insert a space
#define BURST_MS          240  // node "flash" ring duration
#define RESULT_FLASH_MS   350  // new character stays green in the result line
#define PREVIEW_PULSE_MS  110  // held-symbol preview pulse period
#define PROGRESS_REDRAW_MS 40  // commit-countdown bar refresh period

// --- Result text -------------------------------------------------------------
#define TEXT_MAX          96
