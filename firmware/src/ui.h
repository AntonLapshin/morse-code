#pragma once

#include <stdint.h>
#include "config.h"
#include "morse.h"

// Everything the renderer and the input state machine share.
struct MorseState {
  // Current position in the A-Z tree.
  uint8_t  node;       // MorseNode index, 0 = start
  uint8_t  depth;      // symbols in the current letter (0..4)
  uint8_t  bits;       // bit i = 1 -> dash at symbol i (oldest first)

  // Decoded text (always NUL terminated).
  char     text[TEXT_MAX + 1];
  uint8_t  len;

  // Timing / effects.
  uint32_t lastSymbolAt;   // last accepted symbol (for the commit countdown)
  uint32_t lastCommitAt;   // last committed letter (for the word gap)
  uint32_t lastCharAt;     // last appended char (green flash)
  bool     charFlashActive;
  uint8_t  burstNode;      // node to flash after a symbol/commit/clear
  uint32_t burstUntil;
  bool     burstActive;
  bool     burstError;     // red instead of green
};

// Preview symbol shown while the input button is held.
enum MorsePreview : uint8_t {
  PREVIEW_NONE    = 0,
  PREVIEW_DOT     = 1,
  PREVIEW_DASH    = 2,
  PREVIEW_CLEAR   = 3, // legacy: long-hold clear (kept for old scenes)
  PREVIEW_ACTIONS = 4  // long hold armed: release opens the action menu
};

// One incoming message. The display owns a small ring; a message that has
// been read is removed, so `unread == stored count`. `id` is unique and
// monotonically increasing (per boot in the sim; persisted NVS `last_seq`
// on the device later).
struct InboxMsg {
  uint32_t id;
  char     text[MSG_TEXT_MAX + 1];
};

class Adafruit_ST7789;

void uiBegin(Adafruit_ST7789* display);

// Complete repaint (title, tree, divider, pattern, text, countdown).
void uiFull(const MorseState& st, uint32_t now);

// Partial repaints for the animation-friendly bits.
void uiPattern(const MorseState& st, uint8_t preview, uint32_t now);
void uiResult(const MorseState& st, uint32_t now);
void uiProgress(const MorseState& st, bool pressed, uint32_t now);

// Incoming-message UI. All overlays compose in RAM first, so there is no
// black flash when a menu opens.
void uiBadge(uint8_t unread, uint32_t now); // "N NEW" at the top-right
void uiActionSheet(const char* const* items, uint8_t count, uint8_t selected,
                   uint32_t now); // COMPOSE long-hold menu (SEND/CLEAR/READ/...)
void uiMessageCard(uint32_t id, const char* text, uint8_t index1, uint8_t total,
                   bool menuOpen, uint8_t menuSel,
                   uint32_t now); // MESSAGE view; menu = NEXT/CLOSE
