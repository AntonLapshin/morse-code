#include "ui.h"

#include <new>
#include <string.h>

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// -----------------------------------------------------------------------------
// Palette (16-bit RGB565, same style as the reference image)
// -----------------------------------------------------------------------------
static constexpr uint16_t C(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static const uint16_t COL_BG         = C(8, 12, 24);
static const uint16_t COL_TITLE      = C(246, 211, 104);
static const uint16_t COL_LINE       = C(126, 148, 180);
static const uint16_t COL_LABEL      = C(198, 211, 232);
static const uint16_t COL_GREEN      = C(70, 235, 55);
static const uint16_t COL_GREEN_IN   = C(178, 255, 158);
static const uint16_t COL_GREEN_MID  = C(18, 120, 36);
static const uint16_t COL_GREEN_DEEP = C(8, 54, 18);
static const uint16_t COL_GREEN_DARK = C(3, 24, 10);
static const uint16_t COL_PATH       = C(22, 130, 52);
static const uint16_t COL_HINT       = C(116, 132, 156);
static const uint16_t COL_DIVIDER    = C(40, 56, 82);
static const uint16_t COL_RED        = C(255, 66, 54);
static const uint16_t COL_RED_MID    = C(150, 22, 20);
static const uint16_t COL_RED_DEEP   = C(72, 12, 10);
static const uint16_t COL_RED_DARK   = C(30, 6, 6);
static const uint16_t COL_YELLOW     = C(255, 214, 96);
static const uint16_t COL_YELLOW_DIM = C(120, 94, 32);
static const uint16_t COL_WHITE      = 0xFFFF;

// -----------------------------------------------------------------------------
// Layout
// -----------------------------------------------------------------------------
#define TITLE_Y      18
#define PATTERN_TOP  261
#define PATTERN_H    10
#define PROGRESS_Y   270
#define RESULT_TOP   276
#define RESULT_H     44

// The whole UI is composed in RAM and then blitted to the panel in one go.
// This keeps every refresh free of clear-then-draw blanking: the panel only
// ever sees complete frames.
static Adafruit_ST7789* tft = nullptr;
static GFXcanvas16*     canvas = nullptr;
static Adafruit_GFX*    gfx = nullptr;

// -----------------------------------------------------------------------------
// Text helpers
// -----------------------------------------------------------------------------
static int textAdvance(const char* s, uint8_t size, uint8_t spacing) {
  int n = (int)strlen(s);
  if (n <= 0) return 0;
  return n * 6 * size - size + (n - 1) * spacing;
}

static void drawText(const char* s, int x, int y, uint8_t size, uint16_t fg,
                     uint16_t bg, uint8_t spacing = 0) {
  gfx->setTextSize(size);
  gfx->setTextColor(fg, bg);
  int cx = x;
  for (const char* p = s; *p; ++p) {
    if (*p != ' ' || bg != fg) {
      gfx->setCursor(cx, y);
      gfx->print(*p);
    }
    cx += 6 * size + spacing;
  }
}

static void centeredText(const char* s, int y, uint8_t size, uint16_t fg,
                         uint16_t bg, uint8_t spacing = 0) {
  int w = textAdvance(s, size, spacing);
  drawText(s, (SCREEN_W - w) / 2, y, size, fg, bg, spacing);
}

// -----------------------------------------------------------------------------
// Circuit-tree drawing
// -----------------------------------------------------------------------------
static void drawEdge(int x1, int y1, int x2, int y2, uint16_t c) {
  if (y1 == y2) {
    int xa = x1 < x2 ? x1 : x2;
    int xb = x1 < x2 ? x2 : x1;
    gfx->fillRect(xa, y1 - 1, xb - xa + 1, 2, c);
  } else if (x1 == x2) {
    int ya = y1 < y2 ? y1 : y2;
    int yb = y1 < y2 ? y2 : y1;
    gfx->fillRect(x1 - 1, ya, 2, yb - ya + 1, c);
  } else {
    gfx->drawLine(x1, y1, x2, y2, c);
  }
}

// Soft stepped halo, like the green glow on "E" in the reference image.
static void drawGlow(int cx, int cy, bool error) {
  uint16_t dark = error ? COL_RED_DARK : COL_GREEN_DARK;
  uint16_t deep = error ? COL_RED_DEEP : COL_GREEN_DEEP;
  uint16_t mid  = error ? COL_RED_MID  : COL_GREEN_MID;
  gfx->fillCircle(cx, cy, 12, dark);
  gfx->fillCircle(cx, cy, 9, deep);
  gfx->fillCircle(cx, cy, 7, mid);
}

static void drawNodeShape(const MorseNode& n, bool current) {
  if (n.shape == MORSE_DOT) {
    gfx->fillCircle(n.x, n.y, 6, COL_BG);
    if (current) {
      gfx->fillCircle(n.x, n.y, 6, COL_GREEN);
      gfx->drawCircle(n.x, n.y, 6, COL_GREEN_IN);
      gfx->drawCircle(n.x, n.y, 5, COL_GREEN_IN);
      gfx->fillCircle(n.x, n.y, 3, COL_GREEN_IN);
      gfx->fillCircle(n.x, n.y, 1, COL_WHITE);
    } else {
      gfx->drawCircle(n.x, n.y, 6, COL_LINE);
      gfx->drawCircle(n.x, n.y, 5, COL_LINE);
    }
  } else if (n.shape == MORSE_DASH) {
    int x = n.x - 8;
    int y = n.y - 4;
    gfx->fillRect(x, y, 16, 9, COL_BG);
    if (current) {
      gfx->fillRect(x, y, 16, 9, COL_GREEN);
      gfx->drawRect(x, y, 16, 9, COL_GREEN_IN);
      gfx->drawRect(x + 1, y + 1, 14, 7, COL_GREEN_IN);
      gfx->fillRect(x + 3, y + 2, 10, 5, COL_GREEN_IN);
    } else {
      gfx->drawRect(x, y, 16, 9, COL_LINE);
      gfx->drawRect(x + 1, y + 1, 14, 7, COL_LINE);
    }
  } else { // root
    gfx->fillCircle(n.x, n.y, 3, COL_BG);
    if (current) {
      gfx->fillCircle(n.x, n.y, 3, COL_GREEN);
      gfx->drawCircle(n.x, n.y, 4, COL_GREEN_IN);
    } else {
      gfx->drawCircle(n.x, n.y, 3, COL_LINE);
      gfx->drawCircle(n.x, n.y, 2, COL_LINE);
    }
  }
}

static void drawBurstRing(const MorseNode& n, bool error) {
  uint16_t c = error ? COL_RED : COL_GREEN_IN;
  if (n.shape == MORSE_DASH) {
    gfx->drawRect(n.x - 12, n.y - 8, 24, 16, c);
    gfx->drawRect(n.x - 13, n.y - 9, 26, 18, c);
  } else {
    gfx->drawCircle(n.x, n.y, 10, c);
    gfx->drawCircle(n.x, n.y, 11, c);
  }
}

static void drawTree(const MorseState& st, uint32_t now) {
  // 1. Static circuit: every parent->child edge.
  for (uint8_t i = 1; i < MORSE_NODE_COUNT; i++) {
    const MorseNode& n = MORSE_NODES[i];
    const MorseNode& p = MORSE_NODES[n.parent];
    drawEdge(p.x, p.y, n.x, n.y, COL_LINE);
  }

  // 2. Light up the path from the root to the current node.
  int idx = st.node;
  while (idx > 0) {
    const MorseNode& n = MORSE_NODES[idx];
    const MorseNode& p = MORSE_NODES[n.parent];
    drawEdge(p.x, p.y, n.x, n.y, COL_PATH);
    idx = n.parent;
  }

  bool burstOn = st.burstActive && (int32_t)(now - st.burstUntil) < 0;
  bool errorGlow = burstOn && st.burstError && st.burstNode == st.node;

  // 3. Halo behind the active node.
  drawGlow(MORSE_NODES[st.node].x, MORSE_NODES[st.node].y, errorGlow);

  // 4. Every node (fills its own background, masking the edges nicely).
  for (uint8_t i = 0; i < MORSE_NODE_COUNT; i++) {
    drawNodeShape(MORSE_NODES[i], i == st.node);
  }

  // 5. Flash ring around the node that just changed / just committed.
  if (burstOn && st.burstNode < MORSE_NODE_COUNT) {
    drawBurstRing(MORSE_NODES[st.burstNode], st.burstError);
  }

  // 6. Letter labels on top.
  for (uint8_t i = 0; i < MORSE_NODE_COUNT; i++) {
    const MorseNode& n = MORSE_NODES[i];
    if (n.letter == '\0') continue;
    uint16_t c = COL_LABEL;
    if (i == st.node) c = COL_GREEN_IN;
    if (burstOn && i == st.burstNode) c = st.burstError ? COL_RED : COL_GREEN_IN;
    char s[2] = { n.letter, '\0' };
    drawText(s, n.labelX, n.labelY, 1, c, COL_BG);
  }
}

// -----------------------------------------------------------------------------
// Pattern row: dots / dashes of the letter being typed
// -----------------------------------------------------------------------------
static void drawPattern(const MorseState& st, uint8_t preview, uint32_t now) {
  gfx->fillRect(0, PATTERN_TOP, SCREEN_W, PATTERN_H, COL_BG);

  if (preview == PREVIEW_CLEAR) {
    centeredText("CLEAR", PATTERN_TOP, 1, COL_RED, COL_BG);
    return;
  }

  uint8_t k = st.depth;
  if (preview == PREVIEW_DOT || preview == PREVIEW_DASH) k++;
  if (k == 0) return;
  if (k > 5) k = 5;

  bool     isDash[5];
  int      widths[5];
  uint16_t colors[5];
  uint8_t  out = 0;

  for (uint8_t i = 0; i < st.depth && out < 5; i++) {
    bool d = ((st.bits >> i) & 1) != 0;
    isDash[out] = d;
    widths[out] = d ? 10 : 4;
    colors[out] = COL_GREEN;
    out++;
  }

  if ((preview == PREVIEW_DOT || preview == PREVIEW_DASH) && out < 5) {
    bool d = preview == PREVIEW_DASH;
    bool on = ((now / PREVIEW_PULSE_MS) & 1) == 0;
    isDash[out] = d;
    widths[out] = d ? 10 : 4;
    colors[out] = d ? (on ? COL_YELLOW : COL_YELLOW_DIM)
                    : (on ? COL_GREEN_IN : COL_GREEN_MID);
    out++;
  }

  const int gap = 5;
  int total = 0;
  for (uint8_t i = 0; i < out; i++) total += widths[i] + (i ? gap : 0);

  int x = (SCREEN_W - total) / 2;
  for (uint8_t i = 0; i < out; i++) {
    if (isDash[i]) {
      gfx->fillRect(x, PATTERN_TOP + 2, 10, 4, colors[i]);
    } else {
      gfx->fillCircle(x + 2, PATTERN_TOP + 4, 2, colors[i]);
    }
    x += widths[i] + gap;
  }
}

// -----------------------------------------------------------------------------
// Result text
// -----------------------------------------------------------------------------
static void drawResultLine(const char* s, int y, int globalStart,
                           const MorseState& st, uint32_t now) {
  int n = (int)strlen(s);
  if (n == 0) return;

  int ink = n;
  while (ink > 0 && s[ink - 1] == ' ') ink--;
  int w = ink > 0 ? ink * 12 - 2 : 0;
  int x = (SCREEN_W - w) / 2;

  bool flash = st.charFlashActive &&
               (int32_t)(now - (st.lastCharAt + RESULT_FLASH_MS)) < 0;

  gfx->setTextSize(2);
  for (int i = 0; i < n; i++) {
    uint16_t c = COL_WHITE;
    if (flash && globalStart + i == (int)st.len - 1) c = COL_GREEN_IN;
    gfx->setTextColor(c, COL_BG);
    gfx->setCursor(x + i * 12, y);
    gfx->print(s[i]);
  }
}

static void drawHintLine(const char* a, const char* b, int y) {
  int na = (int)strlen(a);
  int nb = (int)strlen(b);
  int n = na + 1 + nb;
  int w = n * 12 - 2;
  int x = (SCREEN_W - w) / 2;

  gfx->setTextSize(2);
  gfx->setTextColor(COL_GREEN, COL_BG);
  gfx->setCursor(x, y);
  gfx->print(a);
  gfx->setTextColor(COL_HINT, COL_BG);
  gfx->setCursor(x + (na + 1) * 12, y);
  gfx->print(b);
}

static void drawResult(const MorseState& st, uint32_t now) {
  gfx->fillRect(0, RESULT_TOP, SCREEN_W, RESULT_H, COL_BG);
  gfx->setTextWrap(false);

  if (st.len == 0) {
    drawHintLine("TAP", "DOT", 283);
    drawHintLine("HOLD", "DASH", 301);
    return;
  }

  // Keep the last two 13-character lines visible.
  int start = st.len > 26 ? (int)st.len - 26 : 0;

  // Do not start the window in the middle of a word. Keep the hard cut when
  // the next space is so late that almost nothing would remain visible.
  if (start > 0) {
    int s = start;
    while (s < (int)st.len && st.text[s] != ' ') s++;
    if (s < (int)st.len && (int)st.len - (s + 1) >= 8) start = s + 1;
  }

  int n = (int)st.len - start;
  if (n <= 0) return;

  char buf[28];
  if (n > 26) n = 26;
  memcpy(buf, st.text + start, n);
  buf[n] = '\0';

  if (n <= 13) {
    drawResultLine(buf, 286, start, st, now);
    return;
  }

  // Split on the last space that still fits the first line.
  int split = 13;
  int skip  = 0;
  for (int i = 13; i >= 7; --i) {
    if (buf[i] == ' ') {
      split = i;
      skip  = 1;
      break;
    }
  }

  char a[28];
  char b[28];
  int na = split;
  memcpy(a, buf, na);
  a[na] = '\0';

  int nb = n - na - skip;
  if (nb > 13) nb = 13;
  if (nb > 0) memcpy(b, buf + na + skip, nb);
  b[nb > 0 ? nb : 0] = '\0';

  drawResultLine(a, 280, start, st, now);
  drawResultLine(b, 298, start + na + skip, st, now);
}

// -----------------------------------------------------------------------------
// Commit countdown
// -----------------------------------------------------------------------------
static void drawProgress(const MorseState& st, bool pressed, uint32_t now) {
  gfx->fillRect(0, PROGRESS_Y, SCREEN_W, 3, COL_BG);
  if (pressed || st.depth == 0) return;

  int elapsed = (int)(now - st.lastSymbolAt);
  int remain = (int)LETTER_GAP_MS - elapsed;
  if (remain < 0) remain = 0;
  int w = remain * 96 / LETTER_GAP_MS;

  const int x = 37;
  gfx->fillRect(x, PROGRESS_Y, 96, 2, COL_DIVIDER);
  if (w > 0) gfx->fillRect(x, PROGRESS_Y, w, 2, COL_GREEN);
}

// -----------------------------------------------------------------------------
// Panel blit
// -----------------------------------------------------------------------------
static void flushRect(int y, int h) {
  if (!tft || !canvas || !canvas->getBuffer() || h <= 0) return;
  if (y < 0) { h += y; y = 0; }
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  if (h <= 0) return;

  tft->startWrite();
  tft->setAddrWindow(0, y, SCREEN_W, h);
  tft->writePixels(canvas->getBuffer() + (size_t)y * SCREEN_W,
                   (uint32_t)SCREEN_W * h);
  tft->endWrite();
}

static void flushAll() { flushRect(0, SCREEN_H); }

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
void uiBegin(Adafruit_ST7789* display) {
  tft = display;
  if (!canvas) canvas = new (std::nothrow) GFXcanvas16(SCREEN_W, SCREEN_H);
  gfx = canvas;
  if (!gfx) return;
  gfx->setTextWrap(false);
  gfx->fillScreen(COL_BG);
}

void uiFull(const MorseState& st, uint32_t now) {
  if (!gfx) return;
  gfx->fillScreen(COL_BG);
  gfx->setTextWrap(false);

  centeredText("MORSE CODE", TITLE_Y, 2, COL_TITLE, COL_BG, 1);
  drawTree(st, now);

  drawPattern(st, PREVIEW_NONE, now);
  drawResult(st, now);
  drawProgress(st, false, now);
  flushAll();
}

void uiPattern(const MorseState& st, uint8_t preview, uint32_t now) {
  if (!gfx) return;
  drawPattern(st, preview, now);
  flushRect(PATTERN_TOP, PATTERN_H);
}

void uiResult(const MorseState& st, uint32_t now) {
  if (!gfx) return;
  drawResult(st, now);
  flushRect(RESULT_TOP, RESULT_H);
}

void uiProgress(const MorseState& st, bool pressed, uint32_t now) {
  if (!gfx) return;
  drawProgress(st, pressed, now);
  flushRect(PROGRESS_Y, 3);
}
