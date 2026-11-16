#include <Arduino.h>
#include <SPI.h>
#include <stdio.h>
#include <string.h>

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <esp_system.h>

#include "config.h"
#include "morse.h"
#include "net.h"
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
// Set once a long-hold action fires while still held, so the release is ignored.
static bool     longFired    = false;

// Undo window for "dash ignores the letter gap": the last committed letter is
// kept so a dash arriving after LETTER_GAP_MS can extend it instead of starting
// a new letter (dot + dash = A, not E + T). Cleared on space/clear/mode change.
static uint8_t  undoNode      = 0;
static uint8_t  undoDepth     = 0;
static uint8_t  undoBits      = 0;
static char     undoLetter    = 0;
static bool     undoAvailable = false;

// --- app modes: COMPOSE Morse text, ACTIONS sheet, MESSAGE reader ------------
enum AppMode : uint8_t { MODE_COMPOSE = 0, MODE_ACTIONS = 1, MODE_MESSAGE = 2 };
static AppMode appMode = MODE_COMPOSE;

// Inbox: newest first (index 0 = newest, shown as 1/N). Reading removes the
// message, so stored count == unread count. `nextMsgId` gives each message a
// unique id.
static InboxMsg  inbox[INBOX_MAX];
static uint8_t   inboxCount = 0;
static uint32_t  nextMsgId  = 1;

// ACTIONS sheet state.
static char      actionBuf[5][16];
static const char* actionPtrs[5];
static uint8_t   actionCount = 0;
static uint8_t   actionSel   = 0;

// MESSAGE view state. `msgMenuOpen` selects between the card (tap = next)
// and the small NEXT/CLOSE menu (tap cycles, hold picks).
static uint8_t   msgIdx      = 0;
static bool      msgMenuOpen = false;
static uint8_t   msgMenuSel  = 0;

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
    // Remember this letter so a later dash can un-commit and extend it.
    undoNode      = st.node;
    undoDepth     = st.depth;
    undoBits      = st.bits;
    undoLetter    = c;
    undoAvailable = true;
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
  undoAvailable = false;
  st.lastCommitAt    = now;
  st.lastSymbolAt    = now;
  st.charFlashActive = false;
  st.burstNode       = MORSE_ROOT_INDEX;
  st.burstUntil      = now + BURST_MS;
  st.burstActive     = true;
  st.burstError      = true;

  uiFull(st, now);
  uiBadge(inboxCount, now);
  Serial.printf("clear (%s)\n", reason);
}

// -----------------------------------------------------------------------------
// Inbox + menus (long-hold opens menus; a read message is removed)
// -----------------------------------------------------------------------------
static void redrawCompose(uint32_t now) {
  appMode = MODE_COMPOSE;
  msgMenuOpen = false;
  uiFull(st, now);
  uiBadge(inboxCount, now);
}

static void rebuildActions() {
  actionCount = 0;
  if (st.len > 0) {
    snprintf(actionBuf[actionCount], sizeof(actionBuf[0]), "SEND");
    actionPtrs[actionCount] = actionBuf[actionCount];
    actionCount++;
    snprintf(actionBuf[actionCount], sizeof(actionBuf[0]), "CLEAR");
    actionPtrs[actionCount] = actionBuf[actionCount];
    actionCount++;
  }
  if (inboxCount > 0) {
    snprintf(actionBuf[actionCount], sizeof(actionBuf[0]), "READ %u",
             (unsigned)inboxCount);
    actionPtrs[actionCount] = actionBuf[actionCount];
    actionCount++;
  }
  snprintf(actionBuf[actionCount], sizeof(actionBuf[0]), "CLOSE");
  actionPtrs[actionCount] = actionBuf[actionCount];
  actionCount++;
  if (actionSel >= actionCount) actionSel = 0;
}

static void enterActions(uint32_t now) {
  appMode = MODE_ACTIONS;
  actionSel = 0;
  undoAvailable = false;
  rebuildActions();
  uiFull(st, now);
  uiBadge(inboxCount, now);
  uiActionSheet(actionPtrs, actionCount, actionSel, now);
  Serial.printf("menu actions (%u items)\n", (unsigned)actionCount);
}

static void redrawActions(uint32_t now) {
  rebuildActions();
  uiFull(st, now);
  uiBadge(inboxCount, now);
  uiActionSheet(actionPtrs, actionCount, actionSel, now);
}

static void enterMessage(uint32_t now, uint8_t idx) {
  if (inboxCount == 0) {
    redrawCompose(now);
    return;
  }
  if (idx >= inboxCount) idx = 0;
  appMode = MODE_MESSAGE;
  msgIdx = idx;
  msgMenuOpen = false;
  msgMenuSel = 0;
  undoAvailable = false;
  uiMessageCard(inbox[msgIdx].id, inbox[msgIdx].text, (uint8_t)(msgIdx + 1),
                inboxCount, false, 0, now);
  Serial.printf("open msg #%u (%u/%u)\n", (unsigned)inbox[msgIdx].id,
                (unsigned)msgIdx + 1, (unsigned)inboxCount);
}

static void redrawMessage(uint32_t now) {
  if (inboxCount == 0) {
    redrawCompose(now);
    return;
  }
  if (msgIdx >= inboxCount) msgIdx = 0;
  uiMessageCard(inbox[msgIdx].id, inbox[msgIdx].text, (uint8_t)(msgIdx + 1),
                inboxCount, msgMenuOpen, msgMenuSel, now);
}

// --- Toast overlay (Phase 1 transient status) --------------------------------
static char     toastMsg[24] = {0};
static uint8_t  toastKind = TOAST_INFO;
static uint32_t toastUntil = 0;
static bool     toastActive = false;
static uint8_t  lastLink = 0xFF;

static void repaintCurrent(uint32_t now) {
  if (appMode == MODE_ACTIONS) redrawActions(now);
  else if (appMode == MODE_MESSAGE) redrawMessage(now);
  else {
    uiFull(st, now);
    uiBadge(inboxCount, now);
  }
}

static void showToast(const char* msg, uint8_t kind, uint32_t now,
                      uint32_t durMs) {
  if (!msg || !*msg) return;
  snprintf(toastMsg, sizeof(toastMsg), "%s", msg);
  toastKind = kind;
  toastUntil = now + durMs;
  toastActive = true;
  uiToast(toastMsg, toastKind, now);
}

// Remove inbox[idx]; newest-first so deleting the viewed message reveals the
// next older one at the same index.
static void inboxRemoveAt(uint8_t idx) {
  if (idx >= inboxCount) return;
  Serial.printf("read msg #%u gone\n", (unsigned)inbox[idx].id);
  for (uint8_t i = idx; i + 1 < inboxCount; i++) inbox[i] = inbox[i + 1];
  inboxCount--;
}

// Newest-first insert; drops the oldest when full. Each message gets a unique
// id from nextMsgId.
static void inboxPush(const char* text, uint32_t now) {
  if (!text || !*text) return;
  InboxMsg m;
  m.id = nextMsgId++;
  size_t n = strlen(text);
  if (n > MSG_TEXT_MAX) n = MSG_TEXT_MAX;
  memcpy(m.text, text, n);
  m.text[n] = '\0';
  if (inboxCount >= INBOX_MAX) {
    // Drop the oldest (last entry).
    inboxCount = INBOX_MAX - 1;
  }
  for (uint8_t i = inboxCount; i > 0; i--) inbox[i] = inbox[i - 1];
  inbox[0] = m;
  inboxCount++;
  Serial.printf("rx msg #%u len=%u unread=%u\n", (unsigned)m.id, (unsigned)n,
                (unsigned)inboxCount);
  if (appMode == MODE_COMPOSE) {
    uiBadge(inboxCount, now);
  } else if (appMode == MODE_ACTIONS) {
    redrawActions(now);
  } else {
    redrawMessage(now);
  }
}

// "NEXT" from the message view/menu: the viewed message counts as read, so it
// is removed first, then the next older message is shown (or COMPOSE if none).
static void messageNext(uint32_t now) {
  inboxRemoveAt(msgIdx);
  if (inboxCount == 0) {
    redrawCompose(now);
    return;
  }
  if (msgIdx >= inboxCount) msgIdx = 0;
  msgMenuOpen = false;
  msgMenuSel = 0;
  redrawMessage(now);
}

// "CLOSE": the viewed message counts as read (removed), back to COMPOSE.
static void messageClose(uint32_t now) {
  inboxRemoveAt(msgIdx);
  redrawCompose(now);
}

static void activateAction(uint32_t now) {
  const char* sel = actionSel < actionCount ? actionPtrs[actionSel] : "CLOSE";
  undoAvailable = false;
  if (!strncmp(sel, "SEND", 4)) {
    // Phase 1: hand the text to the net outbox (like the sim's queueSend)
    // and clear the compose buffer immediately. The net task POSTs it and
    // reports back; the loop turns that into a SENT / error toast.
    const char* p = st.text;
    while (*p == ' ') p++;
    if (*p == '\0') {
      st.len = 0;
      st.text[0] = '\0';
      resetPath();
      st.lastCommitAt = now;
      st.lastSymbolAt = now;
      redrawCompose(now);
      return;
    }
    if (netEnqueueSend(st.text)) {
      Serial.printf("send queued text=\"%s\"\n", st.text);
      st.len = 0;
      st.text[0] = '\0';
      resetPath();
      st.lastCommitAt = now;
      st.lastSymbolAt = now;
      redrawCompose(now);
      showToast("QUEUED", TOAST_WARN, now, 1500);
    } else {
      Serial.println("send: outbox full");
      showToast("BUSY", TOAST_ERR, now, 1500);
    }
  } else if (!strncmp(sel, "CLEAR", 5)) {
    st.len = 0;
    st.text[0] = '\0';
    resetPath();
    st.lastCommitAt = now;
    st.lastSymbolAt = now;
    redrawCompose(now);
    Serial.println("clear (menu)");
  } else if (!strncmp(sel, "READ", 4)) {
    enterMessage(now, 0);
  } else {
    redrawCompose(now);
  }
}

static void onPressDown(uint32_t now) {
  (void)now;
  // Intentionally empty: the commit-vs-extend decision is deferred to release.
  // A short press (dot) after LETTER_GAP_MS starts a new letter, while a long
  // press (dash) extends the pending letter even if the gap already expired
  // (dot + dash = A). Committing here would break the dash-extends case.
}

static bool tryUncommitForDash() {
  if (!undoAvailable || st.depth != 0) return false;
  if (st.len == 0 || st.text[st.len - 1] != undoLetter) return false;
  // Only un-commit when the dash can actually extend the letter. A leaf
  // (e.g. H = ....) has no dash child, so keep it committed.
  if (undoDepth == 0 || morseChild(undoNode, true) < 0) return false;
  st.len--;
  st.text[st.len] = '\0';
  st.node  = undoNode;
  st.depth = undoDepth;
  st.bits  = undoBits;
  undoAvailable = false;
  return true;
}

static void onRelease(uint32_t now, uint32_t durationMs) {
  // A long-hold action already fired while still held: ignore the release.
  if (longFired) {
    longFired = false;
    return;
  }
  // Menus first: taps cycle, long holds pick.
  if (appMode == MODE_ACTIONS) {
    if (durationMs >= ACTION_HOLD_MS) {
      activateAction(now);
    } else {
      if (actionCount > 0) actionSel = (uint8_t)((actionSel + 1) % actionCount);
      uiActionSheet(actionPtrs, actionCount, actionSel, now);
    }
    return;
  }
  if (appMode == MODE_MESSAGE) {
    if (!msgMenuOpen) {
      if (durationMs >= ACTION_HOLD_MS) {
        msgMenuOpen = true;
        msgMenuSel = 0;
        redrawMessage(now);
      } else {
        messageNext(now); // tap = read (gone) + show next
      }
    } else {
      if (durationMs >= ACTION_HOLD_MS) {
        if (msgMenuSel == 0)
          messageNext(now);
        else
          messageClose(now);
      } else {
        msgMenuSel = (uint8_t)((msgMenuSel + 1) % 2);
        redrawMessage(now);
      }
    }
    return;
  }

  // COMPOSE: a long hold opens the action menu (SEND/CLEAR/READ...), never a
  // direct clear. Commit any pending letter first so SEND includes it.
  if (durationMs >= CLEAR_HOLD_MS) {
    if (st.depth > 0) {
      commitLetter(now);
    }
    undoAvailable = false;
    enterActions(now);
    return;
  }

  bool dash = durationMs >= DOT_MAX_MS;

  if (dash) {
    // Long press ignores the letter gap: extend the pending letter instead of
    // starting a new one (dot, pause, dash = A, not E + T).
    tryUncommitForDash();
  } else if (st.depth > 0 && now - st.lastSymbolAt >= LETTER_GAP_MS) {
    // Short press after a pause: the previous letter is done.
    commitLetter(now);
  }

  // Leaf reached and another symbol arrives: finish the letter and start a
  // fresh one with this symbol. This allows continuous typing without waiting.
  if (st.depth > 0 && morseChild(st.node, dash) < 0) {
    commitLetter(now);
  }

  addSymbol(dash, now);
  uiFull(st, now);
  uiBadge(inboxCount, now);
}

static void maybeCommit(uint32_t now) {
  if (appMode != MODE_COMPOSE) return;
  if (st.depth == 0) return;
  if (inputDownRaw()) return; // still holding: this press extends the letter
  if (now - st.lastSymbolAt < LETTER_GAP_MS) return;
  commitLetter(now);
  uiFull(st, now);
  uiBadge(inboxCount, now);
}

static void maybeWordSpace(uint32_t now) {
  if (appMode != MODE_COMPOSE) return;
  if (st.depth != 0) return;
  if (st.len == 0) return;
  if (st.text[st.len - 1] == ' ') return;
  if (inputDownRaw()) return;
  if (now - st.lastCommitAt < WORD_GAP_MS) return;

  appendChar(' ');
  st.lastCommitAt = now;
  undoAvailable = false;
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
      longFired       = false;
      onPressDown(now);
      if (appMode == MODE_COMPOSE) {
        uiPattern(st, preview, now);
        lastPreviewDraw = now;
      }
    } else {
      uint32_t duration = now - inPressedAt;
      preview           = PREVIEW_NONE;
      onRelease(now, duration);
    }
  }

  // Fire long-hold actions while still held so no release is needed.
  if (inPressed && !longFired) {
    uint32_t held = now - inPressedAt;
    if (appMode == MODE_COMPOSE && held >= CLEAR_HOLD_MS) {
      longFired = true;
      if (st.depth > 0) commitLetter(now);
      undoAvailable = false;
      enterActions(now);
    } else if (appMode == MODE_ACTIONS && held >= ACTION_HOLD_MS) {
      longFired = true;
      activateAction(now);
    } else if (appMode == MODE_MESSAGE && held >= ACTION_HOLD_MS) {
      longFired = true;
      if (!msgMenuOpen) {
        msgMenuOpen = true;
        msgMenuSel = 0;
        redrawMessage(now);
      } else {
        if (msgMenuSel == 0) messageNext(now);
        else messageClose(now);
      }
    }
  }

  if (inPressed && !longFired && appMode == MODE_COMPOSE) {
    uint32_t held = now - inPressedAt;
    uint8_t  p;
    if (held >= CLEAR_HOLD_MS)      p = PREVIEW_ACTIONS;
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

  // Phase 1: WiFi + relay. The net task owns uiSetLink() from here on:
  // red = no WiFi, orange = WiFi without relay, green = both OK (sim parity).
  netBegin();
  lastLink = LINK_OFF;
  uiFull(st, now);
  uiBadge(0, now);

  Serial.println();
  Serial.println("morse-code v2");
  Serial.printf("reset reason: %d\n", (int)esp_reset_reason());
  Serial.printf("first frame at %u ms\n", (unsigned)millis());
  Serial.printf("input: GPIO%d (BOOT)  clear: %s\n", BTN_INPUT_PIN,
                BTN_CLEAR_PIN >= 0 ? "GPIO (optional)" : "RESET/EN button");
  Serial.printf("free heap: %u\n", (unsigned)ESP.getFreeHeap());
  Serial.println("tap = dot, hold = dash, pause = letter, long pause = space");
  Serial.println("long hold = menu (SEND/CLEAR/READ), serial 'MSG <text>' = rx");
}

static void pollSerialRx(uint32_t now) {
  // Dev hook (and the future net task entry point): typing
  //   MSG HELLO WORLD
  // into the serial monitor simulates an incoming message, like the sim's
  // "receive" button. Each gets a unique id; the badge shows the count.
  static char line[MSG_TEXT_MAX + 8];
  static uint8_t n = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[n] = '\0';
      n = 0;
      if (!strncmp(line, "MSG ", 4) && line[4]) inboxPush(line + 4, now);
      else if (!strcmp(line, "CLEAR")) clearText(now, "serial");
      line[0] = '\0';
    } else if (n + 1 < sizeof(line)) {
      line[n++] = c;
    }
  }
}

void loop() {
  uint32_t now = millis();

  pollSerialRx(now);

  // Relay inbox: the net task polls GET /messages and queues arrivals here.
  NetInbound in;
  while (netPollInbound(&in)) {
    inboxPush(in.text, now);
    now = millis();
  }
  // Finished sends: QUEUED was shown at pick time; now show the outcome.
  NetSendResult sr;
  while (netPollSendResult(&sr)) {
    if (sr.ok) showToast("SENT", TOAST_OK, now, 1500);
    else showToast(sr.err[0] ? sr.err : "FAILED", TOAST_ERR, now, 2000);
    now = millis();
  }
  // Link dot: repaint the top strip when the net task changes state.
  {
    uint8_t lk = netLink();
    if (lk != lastLink) {
      lastLink = lk;
      uiSetLink(lk);
      if (appMode == MODE_ACTIONS) redrawActions(now);
      else if (appMode == MODE_MESSAGE) redrawMessage(now);
      else {
        uiFull(st, now);
        uiBadge(inboxCount, now);
      }
      if (toastActive && (int32_t)(now - toastUntil) < 0)
        uiToast(toastMsg, toastKind, now);
      now = millis();
    }
  }

  maybeCommit(now);
  maybeWordSpace(now);

  pollInput(now);
#if BTN_CLEAR_PIN >= 0
  pollClearButton(now);
#endif

  now = millis();

  // Remove the transient "flash" ring.
  if (appMode == MODE_COMPOSE && st.burstActive &&
      (int32_t)(now - st.burstUntil) >= 0) {
    st.burstActive = false;
    st.burstError  = false;
    uiFull(st, now);
    uiBadge(inboxCount, now);
  }

  // Let the freshly appended character settle from green to white.
  if (appMode == MODE_COMPOSE && st.charFlashActive &&
      (int32_t)(now - (st.lastCharAt + RESULT_FLASH_MS)) >= 0) {
    st.charFlashActive = false;
    uiResult(st, now);
  }

  // Commit countdown bar (hidden while the button is held).
  if (appMode == MODE_COMPOSE && (st.depth > 0 || inPressed)) {
    if (now - lastProgressDraw >= PROGRESS_REDRAW_MS) {
      lastProgressDraw = now;
      uiProgress(st, inPressed, now);
    }
  } else if (lastProgressDraw != 0) {
    uiProgress(st, false, now);
    lastProgressDraw = 0;
  }

  // Expire the toast overlay by repainting whatever is underneath.
  if (toastActive && (int32_t)(now - toastUntil) >= 0) {
    toastActive = false;
    repaintCurrent(now);
  }

  delay(5);
}
