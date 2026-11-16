#include <Arduino.h>
#include <SPI.h>
#include <string.h>

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <esp_system.h>

#include "config.h"
#include "morse.h"
#include "ui.h"

// IdeaSpark 1.9" ST7789 (fixed on-board wiring).
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  4
#define TFT_MOSI 23
#define TFT_SCLK 18

// RST is driven manually so the panel is never reset by this firmware.
class MorsePanel : public Adafruit_ST7789 {
public:
  MorsePanel(int8_t cs, int8_t dc) : Adafruit_ST7789(cs, dc, -1) {}

  // Bring the panel up without pulsing its reset line and without issuing
  // SWRESET. The panel's controller keeps its GRAM powered by the 3.3 V rail
  // across an ESP32 reset, so the previous frame stays on screen until the
  // app blits the new one. On a true power-on the panel has already reset
  // itself, so re-sending the same configuration is all that is needed.
  //
  // This mirrors the library's generic ST7789 init minus the 150 ms SWRESET
  // and the reset pulse: the command set below is exactly what remains.
  void bringUp() {
    begin();  // SPI + control pins; constructed without an RST pin

    sendCommand(ST77XX_SLPOUT);
    delay(10);

    uint8_t colmod = 0x55;  // 16-bit colour
    sendCommand(ST77XX_COLMOD, &colmod, 1);
    delay(10);

    uint8_t madctl = ST77XX_MADCTL_RGB;  // rotation 2
    sendCommand(ST77XX_MADCTL, &madctl, 1);

    invertDisplay(true);
    sendCommand(ST77XX_NORON);
    delay(10);
    sendCommand(ST77XX_DISPON);
    delay(10);

    // Geometry for rotation 2 on the 170x320 panel.
    _xstart = (240 - SCREEN_W) / 2;
    _ystart = (320 - SCREEN_H) / 2;
    _width  = SCREEN_W;
    _height = SCREEN_H;
  }
};

MorsePanel tft(TFT_CS, TFT_DC);
MorseState st;

// --- input button state ------------------------------------------------------
static bool     inRaw        = false;
static bool     inPressed    = false;
static uint32_t inRawChanged = 0;
static uint32_t inPressedAt  = 0;
static uint8_t  preview      = PREVIEW_NONE;
static uint32_t lastPreviewDraw  = 0;
static uint32_t lastProgressDraw = 0;

static inline bool inputDownRaw() {
#if BTN_ACTIVE_LOW
  return digitalRead(BTN_INPUT_PIN) == LOW;
#else
  return digitalRead(BTN_INPUT_PIN) == HIGH;
#endif
}

// -----------------------------------------------------------------------------
// Text buffer
// -----------------------------------------------------------------------------
static void appendChar(char c) {
  if (st.len >= TEXT_MAX) {
    memmove(st.text, st.text + 1, TEXT_MAX - 1);
    st.len = TEXT_MAX - 1;
  }
  st.text[st.len++] = c;
  st.text[st.len] = '\0';
}

static void resetPath() {
  st.node  = MORSE_ROOT_INDEX;
  st.depth = 0;
  st.bits  = 0;
}

// -----------------------------------------------------------------------------
// State transitions
// -----------------------------------------------------------------------------
static char commitLetter(uint32_t now) {
  if (st.depth == 0) return 0;

  char c = MORSE_NODES[st.node].letter;
  if (c != '\0') {
    appendChar(c);
    st.lastCharAt      = now;
    st.charFlashActive = true;
  }

  // Flash the letter we just left, then move the cursor back to the start.
  st.burstNode  = st.node;
  st.burstUntil = now + BURST_MS;
  st.burstActive = true;
  st.burstError  = false;

  resetPath();
  st.lastCommitAt = now;

  Serial.printf("letter '%c'  text=\"%s\"\n", c ? c : '?', st.text);
  return c;
}

static void addSymbol(bool dash, uint32_t now) {
  int8_t child = morseChild(st.node, dash);

  // Defensive: if the current node has no such branch, finish the letter first.
  if (child < 0) {
    commitLetter(now);
    child = morseChild(MORSE_ROOT_INDEX, dash);
  }
  if (child < 0) return; // impossible with a correct tree

  st.node = (uint8_t)child;
  if (dash) st.bits |= (uint8_t)(1u << st.depth);
  else      st.bits &= (uint8_t)~(1u << st.depth);
  st.depth++;
  st.lastSymbolAt = now;

  st.burstNode   = st.node;
  st.burstUntil  = now + BURST_MS;
  st.burstActive = true;
  st.burstError  = false;

  Serial.printf("  %s -> %c  (len %u)\n", dash ? "dash" : "dot",
                MORSE_NODES[st.node].letter, st.depth);
}

static void clearText(uint32_t now, const char* reason) {
  st.len = 0;
  st.text[0] = '\0';
  resetPath();
  st.lastCommitAt    = now;
  st.lastSymbolAt    = now;
  st.charFlashActive = false;
  st.burstNode       = MORSE_ROOT_INDEX;
  st.burstUntil      = now + BURST_MS;
  st.burstActive     = true;
  st.burstError      = true;

  uiFull(st, now);
  Serial.printf("clear (%s)\n", reason);
}

static void onPressDown(uint32_t now) {
  // Starting a new symbol after a long pause: finish the previous letter
  // before the new symbol is accepted. This makes consecutive letters feel
  // immediate even if the commit timer has not fired yet.
  if (st.depth > 0 && now - st.lastSymbolAt >= LETTER_GAP_MS) {
    commitLetter(now);
    uiFull(st, now);
  }
}

static void onRelease(uint32_t now, uint32_t durationMs) {
  if (durationMs >= CLEAR_HOLD_MS) {
    clearText(now, "long hold");
    return;
  }

  bool dash = durationMs >= DOT_MAX_MS;

  // Leaf reached and another symbol arrives: finish the letter and start a
  // fresh one with this symbol. This allows continuous typing without waiting.
  if (st.depth > 0 && morseChild(st.node, dash) < 0) {
    commitLetter(now);
  }

  addSymbol(dash, now);
  uiFull(st, now);
}

static void maybeCommit(uint32_t now) {
  if (st.depth == 0) return;
  if (inputDownRaw()) return; // still holding: this press extends the letter
  if (now - st.lastSymbolAt < LETTER_GAP_MS) return;
  commitLetter(now);
  uiFull(st, now);
}

static void maybeWordSpace(uint32_t now) {
  if (st.depth != 0) return;
  if (st.len == 0) return;
  if (st.text[st.len - 1] == ' ') return;
  if (inputDownRaw()) return;
  if (now - st.lastCommitAt < WORD_GAP_MS) return;

  appendChar(' ');
  st.lastCommitAt = now;
  uiResult(st, now);
  Serial.println("space");
}

// -----------------------------------------------------------------------------
// Buttons
// -----------------------------------------------------------------------------
static void pollInput(uint32_t now) {
  bool raw = inputDownRaw();
  if (raw != inRaw) {
    inRaw        = raw;
    inRawChanged = now;
  }

  if (now - inRawChanged >= DEBOUNCE_MS && raw != inPressed) {
    inPressed = raw;
    if (inPressed) {
      inPressedAt     = now;
      preview         = PREVIEW_DOT;
      onPressDown(now);
      uiPattern(st, preview, now);
      lastPreviewDraw = now;
    } else {
      uint32_t duration = now - inPressedAt;
      preview           = PREVIEW_NONE;
      onRelease(now, duration);
    }
  }

  if (inPressed) {
    uint32_t held = now - inPressedAt;
    uint8_t  p;
    if (held >= CLEAR_HOLD_MS)      p = PREVIEW_CLEAR;
    else if (held >= DOT_MAX_MS)    p = PREVIEW_DASH;
    else                            p = PREVIEW_DOT;

    if (p != preview) {
      preview         = p;
      uiPattern(st, preview, now);
      lastPreviewDraw = now;
    } else if (now - lastPreviewDraw >= PREVIEW_PULSE_MS) {
      uiPattern(st, preview, now);
      lastPreviewDraw = now;
    }
  }
}

#if BTN_CLEAR_PIN >= 0
static bool     clrRaw        = false;
static bool     clrPressed    = false;
static uint32_t clrRawChanged = 0;

static void pollClearButton(uint32_t now) {
  bool raw = digitalRead(BTN_CLEAR_PIN) == LOW;
  if (raw != clrRaw) {
    clrRaw        = raw;
    clrRawChanged = now;
  }
  if (now - clrRawChanged >= DEBOUNCE_MS && raw != clrPressed) {
    clrPressed = raw;
    if (!clrPressed) clearText(now, "button");
  }
}
#endif

// -----------------------------------------------------------------------------
// Arduino entry points
// -----------------------------------------------------------------------------
void setup() {
  // Turn the backlight on first. The panel controller keeps its last frame
  // across an ESP32 reset, so lighting the backlight immediately makes a
  // RESET/EN press look seamless instead of blanking the screen for the whole
  // boot. On a true power-on the panel is still in display-off state, so this
  // shows black rather than a white flash.
  pinMode(LCD_BL_PIN, OUTPUT);
  analogWrite(LCD_BL_PIN, LCD_BRIGHTNESS);

  // Never hold the panel in reset: drive RST high and leave it there.
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, HIGH);

  Serial.begin(115200);

  pinMode(BTN_INPUT_PIN, INPUT_PULLUP);
#if BTN_CLEAR_PIN >= 0
  pinMode(BTN_CLEAR_PIN, INPUT_PULLUP);
#endif

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.bringUp();
  uiBegin(&tft);

  memset(&st, 0, sizeof(st));
  st.node = MORSE_ROOT_INDEX;
  st.text[0] = '\0';
  uint32_t now = millis();
  st.lastCommitAt = now;
  st.lastSymbolAt = now;

  inRaw        = inputDownRaw();
  inRawChanged = now;
  inPressed    = false;
  preview      = PREVIEW_NONE;

  uiFull(st, now);

  Serial.println();
  Serial.println("morse-code v2");
  Serial.printf("reset reason: %d\n", (int)esp_reset_reason());
  Serial.printf("first frame at %u ms\n", (unsigned)millis());
  Serial.printf("input: GPIO%d (BOOT)  clear: %s\n", BTN_INPUT_PIN,
                BTN_CLEAR_PIN >= 0 ? "GPIO (optional)" : "RESET/EN button");
  Serial.printf("free heap: %u\n", (unsigned)ESP.getFreeHeap());
  Serial.println("tap = dot, hold = dash, pause = letter, long pause = space");
}

void loop() {
  uint32_t now = millis();

  maybeCommit(now);
  maybeWordSpace(now);

  pollInput(now);
#if BTN_CLEAR_PIN >= 0
  pollClearButton(now);
#endif

  now = millis();

  // Remove the transient "flash" ring.
  if (st.burstActive && (int32_t)(now - st.burstUntil) >= 0) {
    st.burstActive = false;
    st.burstError  = false;
    uiFull(st, now);
  }

  // Let the freshly appended character settle from green to white.
  if (st.charFlashActive &&
      (int32_t)(now - (st.lastCharAt + RESULT_FLASH_MS)) >= 0) {
    st.charFlashActive = false;
    uiResult(st, now);
  }

  // Commit countdown bar (hidden while the button is held).
  if (st.depth > 0 || inPressed) {
    if (now - lastProgressDraw >= PROGRESS_REDRAW_MS) {
      lastProgressDraw = now;
      uiProgress(st, inPressed, now);
    }
  } else if (lastProgressDraw != 0) {
    uiProgress(st, false, now);
    lastProgressDraw = 0;
  }

  delay(5);
}
