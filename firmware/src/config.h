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
#define CLEAR_HOLD_MS     1500 // hold longer than this = open the action menu
#define ACTION_HOLD_MS    CLEAR_HOLD_MS // long-press opens/selects in menus
#define NAV_TAP_MAX_MS    600  // tap vs. hold inside menus/message view
#define LETTER_GAP_MS     700  // silence after a symbol -> commit the letter
#define WORD_GAP_MS       4000 // silence after a letter -> insert a space
#define BURST_MS          240  // node "flash" ring duration
#define RESULT_FLASH_MS   350  // new character stays green in the result line
#define PREVIEW_PULSE_MS  110  // held-symbol preview pulse period
#define PROGRESS_REDRAW_MS 40  // commit-countdown bar refresh period

// --- Messages ----------------------------------------------------------------
#define INBOX_MAX         8    // max stored incoming messages (each gone on read)
#define MSG_TEXT_MAX      96   // max chars per incoming message on the display

// --- Cloudflare relay (0.2.0) ------------------------------------------------
// The relay is a Cloudflare Worker + Durable Object at MORSE_SERVER_URL
// (see worker/README.md). The long-poll wait is 20 s: at wait=0 the two
// boards would burn ~43,200 requests/day each against the Workers Free
// 100,000/day limit; at wait=20 it is ~4,320/day each. Cost of the single
// network task (an ESP32 RAM decision — two TLS tasks do not fit next to
// the 108 KB canvas): a SEND queued while a long-poll is in flight waits
// for that poll to return (up to NET_POLL_WAIT_S) before it is POSTed — the
// UI shows QUEUED, then SENT. The first request after boot uses wait=0 so
// the link dot turns green immediately. See worker/README.md for the
// DO-duration side of the same trade.
#define NET_POLL_WAIT_S   20

// Protocol generation, persisted in NVS next to last_seq (see net.cpp).
// The Worker + DO starts with an EMPTY database, so seq restarts at 1:
// a board that kept its LAN-era last_seq (e.g. 42) would poll since=42
// and ignore every new message forever. Bump this whenever the server
// side restarts its sequence space; a mismatch resets last_seq to 0.
//
// 3: fixes the 0.2.0 TX bug that advanced last_seq to the sender's own seq
// and skipped peer messages with lower seqs. A one-time reset on reflash
// also recovers anything the buggy build skipped.
#define MORSE_PROTO_GEN   3

// --- Result text -------------------------------------------------------------
#define TEXT_MAX          96
