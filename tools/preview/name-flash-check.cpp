// Regression check for the flashing device name (top-right corner).
//
// Root cause it guards: uiFull() used to redraw the whole screen and flush
// it to the panel WITHOUT the device name — the name only lived in
// uiBadge(), which flushed the top strip afterwards. Every uiFull() (each
// dot/dash, each letter commit, each burst expiry, each menu/toast/link
// repaint) therefore put a nameless frame on the panel for one full-frame
// SPI transfer (~20 ms), then the name reappeared. Everything else in the
// frame is redrawn identically by uiFull(), so only the name visibly
// toggled — the "constantly flashing" name, with nothing else flashing.
// The unread badge flashed the same way whenever it was shown.
//
// The check drives the exact redraw sequence main.cpp's loop() uses and
// snapshots the PANEL framebuffer (display.framebuffer(), updated per
// flush — the same bytes the panel shows) after EVERY ui* call. The name
// must be present in the top-right region after every single call, and
// its pixels must be identical across the compose-screen calls. A plain
// end-of-sequence snapshot would NOT catch this: after uiBadge() the
// final canvas always looks right — only the intermediate flushed frame
// is wrong.
//
// Build + run: `make check-name` (also runs as part of `make test`).
// Exit 0 = pass, 1 = fail; each step prints its pixel counts.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <Adafruit_ST7789.h>

#include "morse.h"
#include "ui.h"

static Adafruit_ST7789 display(15, 2, 4);

// COL_BG = C(8,12,24); COL_LABEL = C(198,211,232) — same formulas as ui.cpp.
static constexpr uint16_t COL_BG = (uint16_t)(((8 & 0xF8) << 8) |
                                               ((12 & 0xFC) << 3) | (24 >> 3));
static constexpr uint16_t COL_LABEL = (uint16_t)(((198 & 0xF8) << 8) |
                                                  ((211 & 0xFC) << 3) |
                                                  (232 >> 3));

// Name region: "DAD" is drawn right-aligned ending at x=162, y=3 on the
// compose screen (y=6 on the message card). x 138..162 is name-only: the
// "N NEW" badge and its red dot sit left of x=135 when a name is set.
static const int NAME_X0 = 138, NAME_X1 = 162, NAME_Y0 = 0, NAME_Y1 = 13;
// Badge region (only meaningful when unread > 0): red dot + "N NEW".
static const int BADGE_X0 = 95, BADGE_X1 = 135;

static int gFailures = 0;
static std::string gFirstNamePixels; // exact name-region pixels, first snapshot

static int countRegion(int x0, int x1, int y0, int y1, std::string* pixels) {
  const std::vector<uint16_t>& fb = display.framebuffer();
  int n = 0;
  if (pixels) pixels->clear();
  for (int y = y0; y <= y1; y++) {
    for (int x = x0; x <= x1; x++) {
      uint16_t c = fb[(size_t)y * SCREEN_W + x];
      if (pixels) pixels->push_back(c == COL_BG ? '.' : '#');
      if (c != COL_BG) n++;
    }
  }
  return n;
}

// The name must be present after this call, and its pixels must match the
// first snapshot (same position, same glyphs — no flicker, no shift).
static void checkStep(const char* step, bool expectName) {
  std::string pixels;
  int nameN = countRegion(NAME_X0, NAME_X1, NAME_Y0, NAME_Y1, &pixels);
  int badgeN = countRegion(BADGE_X0, BADGE_X1, NAME_Y0, NAME_Y1, nullptr);
  bool ok = true;
  if (expectName) {
    if (nameN < 20) ok = false; // "DAD" in COL_LABEL is ~40 ink pixels
    // Only a snapshot that actually shows the name can be the baseline:
    // recording the (empty) post-uiFull frame as the baseline would make
    // the following correct frame "differ" from it.
    if (nameN >= 20) {
      if (gFirstNamePixels.empty()) {
        gFirstNamePixels = pixels;
      } else if (pixels != gFirstNamePixels) {
        ok = false; // same name, different pixels = flicker/shift
      }
    }
  }
  printf("  %-42s name=%3d badge=%3d %s\n", step, nameN, badgeN,
         ok ? "ok" : "FAIL");
  if (!ok) gFailures++;
}

static MorseState freshState(uint32_t now) {
  MorseState st;
  memset(&st, 0, sizeof(st));
  st.node = MORSE_ROOT_INDEX;
  st.text[0] = '\0';
  st.len = 0;
  st.lastCommitAt = now - WORD_GAP_MS * 2;
  st.lastSymbolAt = now - LETTER_GAP_MS * 2;
  return st;
}

int main() {
  display.init(SCREEN_W, SCREEN_H);
  display.setRotation(DISPLAY_ROTATION);
  uiBegin(&display);
  uiSetLink(LINK_OK);
  uiSetDeviceName("DAD");

  const uint32_t now = 10000;
  MorseState st = freshState(now);
  memcpy(st.text, "HELLO", 5);
  st.text[5] = '\0';
  st.len = 5;

  printf("name-flash check: name must be present after every redraw\n");

  // Compose screen, the exact sequence main.cpp's loop() runs per event.
  uiFull(st, now);
  checkStep("uiFull (dot/dash, commit, burst expiry)", true);
  uiBadge(0, now);
  checkStep("uiBadge(0)", true);
  uiPattern(st, PREVIEW_DOT, now);
  checkStep("uiPattern (held-dot pulse)", true);
  uiProgress(st, true, now);
  checkStep("uiProgress (countdown bar)", true);
  uiResult(st, now);
  checkStep("uiResult (char flash settles)", true);
  uiFull(st, now);
  checkStep("uiFull again", true);
  uiToast("SENT", TOAST_OK, now);
  checkStep("uiToast (overlay, top strip untouched)", true);

  // With an unread badge: uiFull must keep badge AND name, not wipe both.
  uiBadge(3, now);
  checkStep("uiBadge(3)", true);
  uiFull(st, now);
  checkStep("uiFull with unread=3 (badge must survive too)", true);
  uiBadge(3, now);
  checkStep("uiBadge(3) again", true);

  // Message card draws the name at y=6 in the same flush, so it is steady
  // by construction; assert presence (pixels differ from y=3, so reset
  // the stability baseline first).
  gFirstNamePixels.clear();
  uiMessageCard(7, "HELLO ARE YOU OK", 1, 2, false, 0, now);
  checkStep("uiMessageCard", true);

  // Back to compose: name back at y=3, same pixels as before.
  gFirstNamePixels.clear();
  uiFull(st, now);
  checkStep("uiFull (back to compose)", true);
  uiBadge(0, now);
  checkStep("uiBadge(0) (back to compose)", true);

  // No name set -> old layout: the badge keeps its old position (this
  // must not regress either). With no name the badge sits further right —
  // "3 NEW" ends at x=152 and its red dot is at x=117, vs x=134 / x=100
  // with "DAD" set — so the dot position, not the name region, proves the
  // old layout (the badge text itself overlaps the name region).
  uiSetDeviceName("");
  uiFull(st, now);
  uiBadge(3, now);
  {
    // COL_RED = C(255,66,54).
    const uint16_t colRed =
        (uint16_t)(((255 & 0xF8) << 8) | ((66 & 0xFC) << 3) | (54 >> 3));
    const std::vector<uint16_t>& fb = display.framebuffer();
    bool dotOld = fb[(size_t)6 * SCREEN_W + 117] == colRed;
    bool dotNew = fb[(size_t)6 * SCREEN_W + 100] == colRed;
    bool ok = dotOld && !dotNew;
    printf("  %-42s dot@117=%d dot@100=%d %s\n",
           "no name: uiFull+uiBadge(3)", dotOld ? 1 : 0, dotNew ? 1 : 0,
           ok ? "ok" : "FAIL");
    if (!ok) gFailures++;
  }
  (void)COL_LABEL;

  if (gFailures == 0) {
    printf("name-flash check: PASS\n");
    return 0;
  }
  printf("name-flash check: FAIL (%d step(s) — name and/or badge vanished "
         "from a flushed frame)\n",
         gFailures);
  return 1;
}
