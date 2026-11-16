// morse-code UI simulator — ESP32 + ST7789 device simulation with display.
//
// Links the REAL firmware renderer (firmware/src/ui.cpp + morse.cpp) against
// the host Adafruit shims (tools/preview/) and exposes it through a tiny
// stdlib-only HTTP server, so a browser shows exactly what the panel shows.
//
// Same API as the device:
//   uiBegin / uiFull / uiPattern / uiResult / uiProgress
// are called here exactly as in firmware/src/main.cpp. The state machine
// (commit, word-space, burst, flash, countdown) mirrors main.cpp with a
// virtual BOOT button driven from the web UI.
//
// Usage:
//   make sim              # build + serve on http://localhost:8077
//   ./tools/sim/build/sim --port 8077
//   ./tools/sim/build/sim --dump tools/sim/out   # headless screenshots (PPM)
//
// No dependencies beyond a C++17 compiler and POSIX sockets.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <Adafruit_ST7789.h>

#include "config.h"
#include "morse.h"
#include "ui.h"

// -----------------------------------------------------------------------------
// Virtual device state (mirrors firmware/src/main.cpp)
// -----------------------------------------------------------------------------
static Adafruit_ST7789 display(15, 2, 4);
static MorseState st;

static bool     inRaw = false;
static bool     inPressed = false;
static uint32_t inRawChanged = 0;
static uint32_t inPressedAt = 0;
static uint8_t  previewState = PREVIEW_NONE;
static uint32_t lastPreviewDraw = 0;
static uint32_t lastProgressDraw = 0;

// App modes mirror firmware/src/main.cpp: COMPOSE Morse text, ACTIONS sheet
// (long-hold menu), MESSAGE reader. Inbox is newest-first; a read message is
// removed so stored count == unread count. Each message gets a unique id.
enum AppMode : uint8_t { MODE_COMPOSE = 0, MODE_ACTIONS = 1, MODE_MESSAGE = 2 };
static AppMode appMode = MODE_COMPOSE;
static InboxMsg inbox[INBOX_MAX];
static uint8_t inboxCount = 0;
static uint32_t nextMsgId = 1;
static char actionBuf[5][16];
static const char* actionPtrs[5];
static uint8_t actionCount = 0;
static uint8_t actionSel = 0;
static uint8_t msgIdx = 0;
static bool msgMenuOpen = false;
static uint8_t msgMenuSel = 0;

// Wall-clock boot time; virtual millis() like Arduino.
static std::chrono::steady_clock::time_point bootTp;
static uint32_t vnow() {
  using namespace std::chrono;
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - bootTp)
      .count();
}

static void appendChar(char c) {
  if (st.len >= TEXT_MAX) {
    memmove(st.text, st.text + 1, TEXT_MAX - 1);
    st.len = TEXT_MAX - 1;
  }
  st.text[st.len++] = c;
  st.text[st.len] = '\0';
}

static void resetPath() {
  st.node = MORSE_ROOT_INDEX;
  st.depth = 0;
  st.bits = 0;
}

static char commitLetter(uint32_t now) {
  if (st.depth == 0) return 0;
  char c = MORSE_NODES[st.node].letter;
  if (c != '\0') {
    appendChar(c);
    st.lastCharAt = now;
    st.charFlashActive = true;
  }
  st.burstNode = st.node;
  st.burstUntil = now + BURST_MS;
  st.burstActive = true;
  st.burstError = false;
  resetPath();
  st.lastCommitAt = now;
  return c;
}

static void addSymbol(bool dash, uint32_t now) {
  int8_t child = morseChild(st.node, dash);
  if (child < 0) {
    commitLetter(now);
    child = morseChild(MORSE_ROOT_INDEX, dash);
  }
  if (child < 0) return;
  st.node = (uint8_t)child;
  if (dash)
    st.bits |= (uint8_t)(1u << st.depth);
  else
    st.bits &= (uint8_t)~(1u << st.depth);
  st.depth++;
  st.lastSymbolAt = now;
  st.burstNode = st.node;
  st.burstUntil = now + BURST_MS;
  st.burstActive = true;
  st.burstError = false;
}

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
  rebuildActions();
  uiFull(st, now);
  uiBadge(inboxCount, now);
  uiActionSheet(actionPtrs, actionCount, actionSel, now);
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
  uiMessageCard(inbox[msgIdx].id, inbox[msgIdx].text, (uint8_t)(msgIdx + 1),
                inboxCount, false, 0, now);
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

static void inboxRemoveAt(uint8_t idx) {
  if (idx >= inboxCount) return;
  for (uint8_t i = idx; i + 1 < inboxCount; i++) inbox[i] = inbox[i + 1];
  inboxCount--;
}

static void inboxPush(const char* text, uint32_t now) {
  if (!text || !*text) return;
  InboxMsg m;
  m.id = nextMsgId++;
  size_t n = strlen(text);
  if (n > MSG_TEXT_MAX) n = MSG_TEXT_MAX;
  memcpy(m.text, text, n);
  m.text[n] = '\0';
  if (inboxCount >= INBOX_MAX) inboxCount = INBOX_MAX - 1;
  for (uint8_t i = inboxCount; i > 0; i--) inbox[i] = inbox[i - 1];
  inbox[0] = m;
  inboxCount++;
  if (appMode == MODE_COMPOSE) {
    uiBadge(inboxCount, now);
  } else if (appMode == MODE_ACTIONS) {
    redrawActions(now);
  } else {
    redrawMessage(now);
  }
}

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

static void messageClose(uint32_t now) {
  inboxRemoveAt(msgIdx);
  redrawCompose(now);
}

static void activateAction(uint32_t now) {
  const char* sel = actionSel < actionCount ? actionPtrs[actionSel] : "CLOSE";
  if (!strncmp(sel, "SEND", 4)) {
    st.len = 0;
    st.text[0] = '\0';
    resetPath();
    st.lastCommitAt = now;
    st.lastSymbolAt = now;
    redrawCompose(now);
  } else if (!strncmp(sel, "CLEAR", 5)) {
    st.len = 0;
    st.text[0] = '\0';
    resetPath();
    st.lastCommitAt = now;
    st.lastSymbolAt = now;
    redrawCompose(now);
  } else if (!strncmp(sel, "READ", 4)) {
    enterMessage(now, 0);
  } else {
    redrawCompose(now);
  }
}

static void clearTextSim(uint32_t now) {
  st.len = 0;
  st.text[0] = '\0';
  resetPath();
  st.lastCommitAt = now;
  st.lastSymbolAt = now;
  st.charFlashActive = false;
  st.burstNode = MORSE_ROOT_INDEX;
  st.burstUntil = now + BURST_MS;
  st.burstActive = true;
  st.burstError = true;
  redrawCompose(now);
}

static void onPressDown(uint32_t now) {
  if (appMode != MODE_COMPOSE) return;
  if (st.depth > 0 && now - st.lastSymbolAt >= LETTER_GAP_MS) {
    commitLetter(now);
    uiFull(st, now);
    uiBadge(inboxCount, now);
  }
}

static void onRelease(uint32_t now, uint32_t durationMs) {
  if (appMode == MODE_ACTIONS) {
    if (durationMs >= ACTION_HOLD_MS) {
      activateAction(now);
    } else if (actionCount > 0) {
      actionSel = (uint8_t)((actionSel + 1) % actionCount);
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
        messageNext(now);
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
  if (durationMs >= CLEAR_HOLD_MS) {
    if (st.depth > 0) commitLetter(now);
    enterActions(now);
    return;
  }
  bool dash = durationMs >= DOT_MAX_MS;
  if (st.depth > 0 && morseChild(st.node, dash) < 0) {
    commitLetter(now);
  }
  addSymbol(dash, now);
  uiFull(st, now);
  uiBadge(inboxCount, now);
}

static void maybeCommit(uint32_t now, bool downRaw) {
  if (appMode != MODE_COMPOSE) return;
  if (st.depth == 0) return;
  if (downRaw) return;
  if (now - st.lastSymbolAt < LETTER_GAP_MS) return;
  commitLetter(now);
  uiFull(st, now);
  uiBadge(inboxCount, now);
}

static void maybeWordSpace(uint32_t now, bool downRaw) {
  if (appMode != MODE_COMPOSE) return;
  if (st.depth != 0) return;
  if (st.len == 0) return;
  if (st.text[st.len - 1] == ' ') return;
  if (downRaw) return;
  if (now - st.lastCommitAt < WORD_GAP_MS) return;
  appendChar(' ');
  st.lastCommitAt = now;
  uiResult(st, now);
}

// Virtual BOOT level set by the web UI.
static bool virtDown = false;

static void pollInput(uint32_t now) {
  bool raw = virtDown;
  if (raw != inRaw) {
    inRaw = raw;
    inRawChanged = now;
  }
  if (now - inRawChanged >= DEBOUNCE_MS && raw != inPressed) {
    inPressed = raw;
    if (inPressed) {
      inPressedAt = now;
      previewState = PREVIEW_DOT;
      onPressDown(now);
      if (appMode == MODE_COMPOSE) {
        uiPattern(st, previewState, now);
        lastPreviewDraw = now;
      }
    } else {
      uint32_t duration = now - inPressedAt;
      previewState = PREVIEW_NONE;
      onRelease(now, duration);
    }
  }
  if (inPressed && appMode == MODE_COMPOSE) {
    uint32_t held = now - inPressedAt;
    uint8_t p;
    if (held >= CLEAR_HOLD_MS)
      p = PREVIEW_ACTIONS;
    else if (held >= DOT_MAX_MS)
      p = PREVIEW_DASH;
    else
      p = PREVIEW_DOT;
    if (p != previewState) {
      previewState = p;
      uiPattern(st, previewState, now);
      lastPreviewDraw = now;
    } else if (now - lastPreviewDraw >= PREVIEW_PULSE_MS) {
      uiPattern(st, previewState, now);
      lastPreviewDraw = now;
    }
  }
}

static void tick(uint32_t now) {
  maybeCommit(now, virtDown);
  maybeWordSpace(now, virtDown);
  pollInput(now);
  now = vnow();
  if (appMode == MODE_COMPOSE && st.burstActive &&
      (int32_t)(now - st.burstUntil) >= 0) {
    st.burstActive = false;
    st.burstError = false;
    uiFull(st, now);
    uiBadge(inboxCount, now);
  }
  if (appMode == MODE_COMPOSE && st.charFlashActive &&
      (int32_t)(now - (st.lastCharAt + RESULT_FLASH_MS)) >= 0) {
    st.charFlashActive = false;
    uiResult(st, now);
  }
  if (appMode == MODE_COMPOSE && (st.depth > 0 || inPressed)) {
    if (now - lastProgressDraw >= PROGRESS_REDRAW_MS) {
      lastProgressDraw = now;
      uiProgress(st, inPressed, now);
    }
  } else if (lastProgressDraw != 0) {
    uiProgress(st, false, now);
    lastProgressDraw = 0;
  }
}

static void resetSim(uint32_t now) {
  memset(&st, 0, sizeof(st));
  st.node = MORSE_ROOT_INDEX;
  st.text[0] = '\0';
  st.lastCommitAt = now;
  st.lastSymbolAt = now;
  inRaw = virtDown;
  inRawChanged = now;
  inPressed = false;
  previewState = PREVIEW_NONE;
  lastPreviewDraw = 0;
  lastProgressDraw = 0;
  appMode = MODE_COMPOSE;
  inboxCount = 0;
  nextMsgId = 1;
  actionCount = 0;
  actionSel = 0;
  msgIdx = 0;
  msgMenuOpen = false;
  msgMenuSel = 0;
  uiFull(st, now);
  uiBadge(0, now);
}

// -----------------------------------------------------------------------------
// Scenes (same set as tools/preview/main.cpp)
// -----------------------------------------------------------------------------
static void setText(MorseState& s, const char* t) {
  size_t n = strlen(t);
  if (n > TEXT_MAX) n = TEXT_MAX;
  memcpy(s.text, t, n);
  s.text[n] = '\0';
  s.len = (uint8_t)n;
}

static void applyScene(const std::string& name) {
  uint32_t now = vnow();
  resetSim(now);
  virtDown = false;
  if (name == "root") {
    // idle
  } else if (name == "e-selected") {
    st.node = 1;
    st.depth = 1;
    st.bits = 0;
    st.lastSymbolAt = now - 120;
    st.burstNode = 1;
    st.burstUntil = now + BURST_MS / 2;
    st.burstActive = true;
  } else if (name == "hello") {
    setText(st, "HELLO");
    st.lastCharAt = now - 120;
    st.charFlashActive = true;
    st.lastCommitAt = now - 4000;
  } else if (name == "sos-help") {
    setText(st, "SOS HELP");
    st.lastCommitAt = now - 3000;
  } else if (name == "mid-letter-s") {
    st.node = 7;
    st.depth = 3;
    st.bits = 0;
    st.lastSymbolAt = now - 300;
  } else if (name == "wrapped") {
    setText(st, "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG");
    st.lastCommitAt = now - 5000;
  } else if (name == "hold-dash") {
    st.node = 5;
    st.depth = 1;
    st.bits = 1;
    st.lastSymbolAt = now - 80;
    st.burstNode = 5;
    st.burstUntil = now + BURST_MS / 2;
    st.burstActive = true;
    // show held-dash preview + pressed countdown
    uiFull(st, now);
    uiPattern(st, PREVIEW_DASH, now);
    uiProgress(st, true, now);
    return;
  } else if (name == "hold-clear") {
    setText(st, "SOS HELP");
    uiFull(st, now);
    uiPattern(st, PREVIEW_CLEAR, now);
    return;
  } else if (name == "hold-menu") {
    setText(st, "SOS HELP");
    uiFull(st, now);
    uiBadge(2, now);
    uiPattern(st, PREVIEW_ACTIONS, now);
    return;
  } else if (name == "menu") {
    setText(st, "HELLO");
    uiFull(st, now);
    uiBadge(2, now);
    inboxPush("ARE YOU OK", now);
    inboxPush("MEET AT SIX", now);
    // inboxPush paints badges; repaint compose then open the sheet
    uiFull(st, now);
    uiBadge(inboxCount, now);
    appMode = MODE_ACTIONS;
    actionSel = 0;
    rebuildActions();
    uiActionSheet(actionPtrs, actionCount, actionSel, now);
    return;
  } else if (name == "badge") {
    setText(st, "HELLO");
    uiFull(st, now);
    uiBadge(3, now);
    return;
  } else if (name == "message") {
    inboxPush("HELLO ARE YOU OK MEET ME AT THE PARK AT SIX", now);
    uiMessageCard(inbox[0].id, inbox[0].text, 1, inboxCount, false, 0, now);
    return;
  } else if (name == "message-menu") {
    inboxPush("FIRST MESSAGE", now);
    inboxPush("SECOND MESSAGE", now);
    appMode = MODE_MESSAGE;
    msgIdx = 0;
    msgMenuOpen = true;
    msgMenuSel = 0;
    uiMessageCard(inbox[msgIdx].id, inbox[msgIdx].text,
                  (uint8_t)(msgIdx + 1), inboxCount, true, 0, now);
    return;
  }
  uiFull(st, now);
  uiBadge(inboxCount, now);
  if (name == "e-selected") uiProgress(st, false, now);
  if (name == "mid-letter-s") {
    // countdown visible
  }
}

// -----------------------------------------------------------------------------
// Framebuffer encoding
// -----------------------------------------------------------------------------
static void framebufferRGB888(std::vector<uint8_t>& out) {
  const auto& fb = display.framebuffer();
  out.resize((size_t)SCREEN_W * SCREEN_H * 3);
  for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) {
    uint16_t c = fb[i];
    out[3 * i + 0] = (uint8_t)(((c >> 11) & 0x1F) << 3 | ((c >> 11) & 0x1F) >> 2);
    out[3 * i + 1] = (uint8_t)(((c >> 5) & 0x3F) << 2 | ((c >> 5) & 0x3F) >> 4);
    out[3 * i + 2] = (uint8_t)((c & 0x1F) << 3 | (c & 0x1F) >> 2);
  }
}

static void writePPM(const std::string& path) {
  std::vector<uint8_t> rgb;
  framebufferRGB888(rgb);
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
  fwrite(rgb.data(), 1, rgb.size(), f);
  fclose(f);
}

// BMP (24-bit, bottom-up) so screenshots open everywhere.
static std::vector<uint8_t> encodeBMP() {
  std::vector<uint8_t> rgb;
  framebufferRGB888(rgb);
  int rowPad = (4 - (SCREEN_W * 3) % 4) % 4;
  int rowSize = SCREEN_W * 3 + rowPad;
  int imgSize = rowSize * SCREEN_H;
  std::vector<uint8_t> bmp(54 + imgSize, 0);
  bmp[0] = 'B';
  bmp[1] = 'M';
  uint32_t fs = 54 + imgSize;
  memcpy(&bmp[2], &fs, 4);
  uint32_t off = 54;
  memcpy(&bmp[10], &off, 4);
  uint32_t hs = 40;
  memcpy(&bmp[14], &hs, 4);
  int32_t w = SCREEN_W, h = SCREEN_H;
  memcpy(&bmp[18], &w, 4);
  memcpy(&bmp[22], &h, 4);
  bmp[26] = 1;
  bmp[28] = 24;
  for (int y = 0; y < SCREEN_H; y++) {
    const uint8_t* src = rgb.data() + (size_t)(SCREEN_H - 1 - y) * SCREEN_W * 3;
    uint8_t* dst = bmp.data() + 54 + (size_t)y * rowSize;
    for (int x = 0; x < SCREEN_W; x++) {
      dst[3 * x + 0] = src[3 * x + 2]; // B
      dst[3 * x + 1] = src[3 * x + 1]; // G
      dst[3 * x + 2] = src[3 * x + 0]; // R
    }
  }
  return bmp;
}

static std::string jsonEscape(const char* s) {
  std::string esc;
  for (const char* p = s; *p; p++) {
    char c = *p;
    if (c == '"' || c == '\\') {
      esc += '\\';
      esc += c;
    } else if (c >= 32 && c < 127) {
      esc += c;
    } else {
      char tmp[8];
      snprintf(tmp, sizeof(tmp), "\\u%04x", c);
      esc += tmp;
    }
  }
  return esc;
}

static std::string stateJSON() {
  uint32_t now = vnow();
  std::string esc = jsonEscape(st.text);
  char letter = MORSE_NODES[st.node].letter ? MORSE_NODES[st.node].letter : '.';
  const char* modeStr = appMode == MODE_COMPOSE ? "compose"
                        : appMode == MODE_ACTIONS ? "actions"
                                                  : "message";
  char buf[2048];
  snprintf(buf, sizeof(buf),
           "{\"now\":%u,\"node\":%u,\"nodeLetter\":\"%c\",\"depth\":%u,"
           "\"text\":\"%s\",\"pressed\":%s,\"preview\":%u,"
           "\"mode\":\"%s\",\"unread\":%u,\"nextId\":%u,"
           "\"msgIdx\":%u,\"msgMenu\":%s,\"msgMenuSel\":%u,"
           "\"actionSel\":%u}",
           now, st.node, letter, st.depth, esc.c_str(),
           (virtDown || inPressed) ? "true" : "false", previewState, modeStr,
           (unsigned)inboxCount, (unsigned)nextMsgId, (unsigned)msgIdx,
           msgMenuOpen ? "true" : "false", (unsigned)msgMenuSel,
           (unsigned)actionSel);
  std::string out(buf);
  // inbox: newest-first [{id,text}]
  out.pop_back(); // remove }
  out += ",\"inbox\":[";
  for (uint8_t i = 0; i < inboxCount; i++) {
    if (i) out += ",";
    char ib[64];
    snprintf(ib, sizeof(ib), "{\"id\":%u,\"text\":\"",
             (unsigned)inbox[i].id);
    out += ib;
    out += jsonEscape(inbox[i].text);
    out += "\"}";
  }
  out += "],\"actions\":[";
  for (uint8_t i = 0; i < actionCount; i++) {
    if (i) out += ",";
    out += "\"";
    out += jsonEscape(actionPtrs[i] ? actionPtrs[i] : "");
    out += "\"";
  }
  out += "]";
  if (appMode == MODE_MESSAGE && inboxCount > 0 && msgIdx < inboxCount) {
    char mb[64];
    snprintf(mb, sizeof(mb), ",\"msgId\":%u,", (unsigned)inbox[msgIdx].id);
    out += mb;
    out += "\"msgText\":\"";
    out += jsonEscape(inbox[msgIdx].text);
    out += "\"";
  }
  out += "}";
  return out;
}

// -----------------------------------------------------------------------------
// Minimal HTTP server
// -----------------------------------------------------------------------------
static std::string g_indexPath = "tools/sim/index.html";

static std::string readFile(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return "";
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::string s;
  s.resize(n > 0 ? (size_t)n : 0);
  if (n > 0) fread(s.data(), 1, (size_t)n, f);
  fclose(f);
  return s;
}

static void sendAll(int fd, const char* p, size_t n) {
  while (n > 0) {
    ssize_t w = send(fd, p, n, 0);
    if (w <= 0) return;
    p += w;
    n -= (size_t)w;
  }
}

static void handleClient(int fd) {
  char req[8192];
  ssize_t nr = recv(fd, req, sizeof(req) - 1, 0);
  if (nr <= 0) return;
  req[nr] = '\0';
  std::string r(req, (size_t)nr);

  char method[8] = {0}, path[256] = {0};
  sscanf(r.c_str(), "%7s %255s", method, path);
  std::string body;
  size_t hdrEnd = r.find("\r\n\r\n");
  if (hdrEnd != std::string::npos) body = r.substr(hdrEnd + 4);
  // If Content-Length says more is coming, read it.
  size_t clPos = r.find("Content-Length:");
  if (clPos != std::string::npos && hdrEnd != std::string::npos) {
    int cl = atoi(r.c_str() + clPos + 15);
    while ((int)body.size() < cl) {
      ssize_t m = recv(fd, req, sizeof(req), 0);
      if (m <= 0) break;
      body.append(req, (size_t)m);
    }
  }

  uint32_t now = vnow();
  tick(now);

  std::string head, payload;
  std::vector<uint8_t> payloadBin;
  bool isBin = false;

  if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
    payload = readFile(g_indexPath);
    if (payload.empty()) payload = "<h1>index.html missing</h1>";
    head = "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
           std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n";
    sendAll(fd, head.c_str(), head.size());
    sendAll(fd, payload.c_str(), payload.size());
    return;
  } else if (!strcmp(path, "/api/state")) {
    payload = stateJSON();
    head = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
           "Content-Length: " +
           std::to_string(payload.size()) +
           "\r\nConnection: close\r\nAccess-Control-Allow-Origin: "
           "*\r\n\r\n";
  } else if (!strcmp(path, "/api/frame.raw")) {
    framebufferRGB888(payloadBin);
    isBin = true;
    head = "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n"
           "Content-Length: " +
           std::to_string(payloadBin.size()) +
           "\r\nConnection: close\r\nAccess-Control-Allow-Origin: "
           "*\r\n\r\n";
  } else if (!strcmp(path, "/api/frame.bmp") || !strcmp(path, "/frame.bmp")) {
    payloadBin = encodeBMP();
    isBin = true;
    head = "HTTP/1.0 200 OK\r\nContent-Type: image/bmp\r\nContent-Length: " +
           std::to_string(payloadBin.size()) +
           "\r\nConnection: close\r\nAccess-Control-Allow-Origin: "
           "*\r\n\r\n";
  } else if (!strcmp(path, "/api/press") && !strcmp(method, "POST")) {
    if (body.find("down") != std::string::npos)
      virtDown = true;
    else if (body.find("up") != std::string::npos)
      virtDown = false;
    else if (body.find("true") != std::string::npos)
      virtDown = true;
    else if (body.find("false") != std::string::npos)
      virtDown = false;
    payload = stateJSON();
    head = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
           "Content-Length: " +
           std::to_string(payload.size()) +
           "\r\nConnection: close\r\nAccess-Control-Allow-Origin: "
           "*\r\n\r\n";
  } else if (!strcmp(path, "/api/scene") && !strcmp(method, "POST")) {
    // body: {"scene":"hello"} or plain name
    std::string name = "root";
    size_t q = body.find("\"scene\"");
    if (q != std::string::npos) {
      size_t c = body.find(':', q);
      size_t q1 = body.find('"', c);
      size_t q2 = body.find('"', q1 + 1);
      if (q1 != std::string::npos && q2 != std::string::npos)
        name = body.substr(q1 + 1, q2 - q1 - 1);
    } else if (!body.empty()) {
      name = body;
      // trim quotes/whitespace
      std::string t;
      for (char ch : name)
        if (ch != '"' && ch != '\'' && ch != ' ' && ch != '\n' && ch != '\r')
          t += ch;
      // allow {"scene":...} fallback already handled; else raw path form
      size_t b = t.find(':');
      if (b != std::string::npos) t = t.substr(b + 1);
      size_t a = t.find('{');
      size_t z = t.find('}');
      if (a != std::string::npos && z != std::string::npos && z > a)
        t = t.substr(a + 1, z - a - 1);
      if (!t.empty()) name = t;
    }
    applyScene(name);
    payload = stateJSON();
    head = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
           "Content-Length: " +
           std::to_string(payload.size()) +
           "\r\nConnection: close\r\nAccess-Control-Allow-Origin: "
           "*\r\n\r\n";
  } else if (!strcmp(path, "/api/text") && !strcmp(method, "POST")) {
    std::string t;
    size_t q = body.find("\"text\"");
    if (q != std::string::npos) {
      size_t c = body.find(':', q);
      size_t q1 = body.find('"', c);
      size_t q2 = body.find('"', q1 + 1);
      if (q1 != std::string::npos && q2 != std::string::npos)
        t = body.substr(q1 + 1, q2 - q1 - 1);
    }
    uint32_t n2 = vnow();
    resetSim(n2);
    setText(st, t.c_str());
    uiFull(st, n2);
    uiBadge(inboxCount, n2);
    payload = stateJSON();
    head = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
           "Content-Length: " +
           std::to_string(payload.size()) +
           "\r\nConnection: close\r\nAccess-Control-Allow-Origin: "
           "*\r\n\r\n";
  } else if (!strcmp(path, "/api/receive") && !strcmp(method, "POST")) {
    // Simulate an incoming message. Body: {"text":"HELLO"} or plain text.
    // Empty body cycles through sample texts. Each gets a unique id.
    std::string t;
    size_t q = body.find("\"text\"");
    if (q != std::string::npos) {
      size_t c = body.find(':', q);
      size_t q1 = body.find('"', c);
      size_t q2 = body.find('"', q1 + 1);
      if (q1 != std::string::npos && q2 != std::string::npos)
        t = body.substr(q1 + 1, q2 - q1 - 1);
    } else if (!body.empty()) {
      // plain text (trim whitespace/quotes)
      std::string u;
      for (char ch : body)
        if (ch != '\r' && ch != '\n') u += ch;
      size_t a = u.find_first_not_of(" \t\"'");
      size_t b = u.find_last_not_of(" \t\"'");
      if (a != std::string::npos && b != std::string::npos)
        t = u.substr(a, b - a + 1);
      else
        t = "";
      if (t == "{}" || t == "receive") t = "";
    }
    if (t.empty()) {
      static const char* samples[] = {"HELLO", "ARE YOU OK",
                                      "MEET AT SIX", "SOS",
                                      "CALL ME BACK SOON", nullptr};
      static int si = 0;
      t = samples[si];
      si = (si + 1) % 5;
    }
    // Cap to display length.
    if (t.size() > MSG_TEXT_MAX) t.resize(MSG_TEXT_MAX);
    inboxPush(t.c_str(), vnow());
    payload = stateJSON();
    head = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
           "Content-Length: " +
           std::to_string(payload.size()) +
           "\r\nConnection: close\r\nAccess-Control-Allow-Origin: "
           "*\r\n\r\n";
  } else if (!strcmp(path, "/api/messages")) {
    payload = stateJSON();
    head = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
           "Content-Length: " +
           std::to_string(payload.size()) +
           "\r\nConnection: close\r\nAccess-Control-Allow-Origin: "
           "*\r\n\r\n";
  } else if (!strcmp(path, "/api/clear") && !strcmp(method, "POST")) {
    clearTextSim(vnow());
    payload = stateJSON();
    head = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
           "Content-Length: " +
           std::to_string(payload.size()) +
           "\r\nConnection: close\r\nAccess-Control-Allow-Origin: "
           "*\r\n\r\n";
  } else {
    payload = "{\"error\":\"not found\"}";
    head = "HTTP/1.0 404 Not Found\r\nContent-Type: "
           "application/json\r\nContent-Length: " +
           std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n";
  }

  if (isBin) {
    sendAll(fd, head.c_str(), head.size());
    sendAll(fd, (const char*)payloadBin.data(), payloadBin.size());
  } else {
    sendAll(fd, head.c_str(), head.size());
    sendAll(fd, payload.c_str(), payload.size());
  }
}

int main(int argc, char** argv) {
  int port = 8077;
  std::string dumpDir;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
    if (!strncmp(argv[i], "--port=", 7)) port = atoi(argv[i] + 7);
    if (!strcmp(argv[i], "--dump") && i + 1 < argc) dumpDir = argv[++i];
    if (!strcmp(argv[i], "--index") && i + 1 < argc) g_indexPath = argv[++i];
  }
  // Resolve index.html relative to CWD or binary location dir candidates.
  if (readFile(g_indexPath).empty()) {
    const char* cands[] = {"tools/sim/index.html", "index.html",
                           "../sim/index.html", nullptr};
    for (int i = 0; cands[i]; i++) {
      if (!readFile(cands[i]).empty()) {
        g_indexPath = cands[i];
        break;
      }
    }
  }

  bootTp = std::chrono::steady_clock::now();
  display.init(SCREEN_W, SCREEN_H);
  display.setRotation(DISPLAY_ROTATION);
  uiBegin(&display);
  resetSim(vnow());

  if (!dumpDir.empty()) {
    const char* scenes[] = {"root",       "e-selected", "hello",
                            "sos-help",   "mid-letter-s", "wrapped",
                            "hold-dash",  "hold-menu",  "menu",
                            "badge",      "message",    "message-menu",
                            nullptr};
    const char* fnames[] = {"00-root",       "01-e-selected", "02-hello",
                            "03-sos-help",   "04-mid-letter-s", "05-wrapped",
                            "06-hold-dash",  "07-hold-menu",  "08-menu",
                            "09-badge",      "10-message",    "11-message-menu",
                            nullptr};
    for (int i = 0; scenes[i]; i++) {
      applyScene(scenes[i]);
      std::string p = dumpDir + "/" + fnames[i] + ".ppm";
      writePPM(p);
      printf("  %s\n", p.c_str());
    }
    return 0;
  }

  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) {
    perror("socket");
    return 1;
  }
  int opt = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)port);
  if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    return 1;
  }
  if (listen(srv, 8) < 0) {
    perror("listen");
    return 1;
  }
  printf("morse-code sim: http://localhost:%d/  (index: %s)\n", port,
         g_indexPath.c_str());
  printf("same renderer as the panel: uiFull/uiPattern/uiResult/uiProgress\n");
  fflush(stdout);

  while (true) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(srv, &rfds);
    timeval tv{0, 5000}; // 5 ms tick, like the firmware loop delay
    int r = select(srv + 1, &rfds, nullptr, nullptr, &tv);
    if (r > 0 && FD_ISSET(srv, &rfds)) {
      int fd = accept(srv, nullptr, nullptr);
      if (fd >= 0) {
        handleClient(fd);
        close(fd);
      }
    }
    tick(vnow());
  }
  return 0;
}
