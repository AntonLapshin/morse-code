// Host-side preview renderer.
//
// Compiles the real firmware renderer (firmware/src/ui.cpp + morse.cpp) against
// the Adafruit_GFX / Adafruit_ST7789 shims in this folder and dumps a handful of
// scenes as PPM images. scripts/preview.sh then turns them into preview.png.
//
// This is purely a layout aid: it exercises the same code the panel runs, so a
// coordinate fix here is a coordinate fix on the device.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <Adafruit_ST7789.h>

#include "morse.h"
#include "ui.h"

static Adafruit_ST7789 display(15, 2, 4);

// -----------------------------------------------------------------------------
// Scene helpers
// -----------------------------------------------------------------------------
static MorseState freshState(uint32_t now) {
  MorseState st;
  memset(&st, 0, sizeof(st));
  st.node       = MORSE_ROOT_INDEX;
  st.text[0]    = '\0';
  st.len        = 0;
  st.lastCommitAt = now - WORD_GAP_MS * 2;
  st.lastSymbolAt = now - LETTER_GAP_MS * 2;
  return st;
}

static void setText(MorseState& st, const char* s) {
  size_t n = strlen(s);
  if (n > TEXT_MAX) n = TEXT_MAX;
  memcpy(st.text, s, n);
  st.text[n] = '\0';
  st.len      = (uint8_t)n;
}

// Position the cursor on a node, with the given symbols already typed.
static void setCursor(MorseState& st, uint8_t node, uint8_t depth, uint8_t bits) {
  st.node  = node;
  st.depth = depth;
  st.bits  = bits;
}

// -----------------------------------------------------------------------------
// Output
// -----------------------------------------------------------------------------
static void writePPM(const std::string& path) {
  const int w = display.width();
  const int h = display.height();
  const std::vector<uint16_t>& fb = display.framebuffer();

  FILE* f = fopen(path.c_str(), "wb");
  if (!f) {
    fprintf(stderr, "cannot write %s\n", path.c_str());
    return;
  }
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int i = 0; i < w * h; ++i) {
    uint16_t c = fb[(size_t)i];
    uint8_t r5 = (uint8_t)((c >> 11) & 0x1F);
    uint8_t g6 = (uint8_t)((c >> 5) & 0x3F);
    uint8_t b5 = (uint8_t)(c & 0x1F);
    uint8_t rgb[3] = {
        (uint8_t)((r5 << 3) | (r5 >> 2)),
        (uint8_t)((g6 << 2) | (g6 >> 4)),
        (uint8_t)((b5 << 3) | (b5 >> 2)),
    };
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
  printf("  %s\n", path.c_str());
}

static int gFrame = 0;
static std::string gOutDir;

static void render(const char* name, const MorseState& st, uint32_t now,
                   uint8_t preview = PREVIEW_NONE, bool pressed = false) {
  char file[512];
  snprintf(file, sizeof(file), "%s/%02d-%s.ppm", gOutDir.c_str(), gFrame++, name);

  uiFull(st, now);
  if (preview != PREVIEW_NONE) uiPattern(st, preview, now);
  if (pressed) uiProgress(st, true, now);
  writePPM(file);
}

// -----------------------------------------------------------------------------
// Scenes
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
  gOutDir = argc > 1 ? argv[1] : "tools/preview/out";

  display.init(SCREEN_W, SCREEN_H);
  display.setRotation(DISPLAY_ROTATION);
  uiBegin(&display);
  uiSetLink(LINK_OK); // preview assumes an online device (green dot)

  const uint32_t now = 10000;

  {  // Idle at the root: hint lines at the bottom.
    MorseState st = freshState(now);
    render("root", st, now);
  }

  {  // First dot: the "E" node glows, commit countdown is running.
    MorseState st = freshState(now);
    setCursor(st, 1, 1, 0);
    st.lastSymbolAt = now - 120;
    st.burstNode    = 1;
    st.burstUntil   = now + BURST_MS / 2;
    st.burstActive  = true;
    render("e-selected", st, now);
  }

  {  // "HELLO", freshly committed O still green in the result line.
    MorseState st = freshState(now);
    setText(st, "HELLO");
    st.lastCharAt      = now - 120;
    st.charFlashActive = true;
    st.lastCommitAt    = now - 4000;
    st.burstNode       = 0;
    render("hello", st, now);
  }

  {  // Two short words: no wrapping yet.
    MorseState st = freshState(now);
    setText(st, "SOS HELP");
    st.lastCommitAt = now - 3000;
    render("sos-help", st, now);
  }

  {  // Mid-letter "S" (three dots), countdown partly elapsed.
    MorseState st = freshState(now);
    setCursor(st, 7, 3, 0);
    st.lastSymbolAt = now - 300;
    render("mid-letter-s", st, now);
  }

  {  // Long text: the last two 13-character rows are kept, split on a space.
    MorseState st = freshState(now);
    setText(st, "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG");
    st.lastCommitAt = now - 5000;
    render("wrapped", st, now);
  }

  {  // "N" already typed, next symbol being held as a dash.
    MorseState st = freshState(now);
    setCursor(st, 5, 1, 1);
    st.lastSymbolAt = now - 80;
    st.burstNode    = 5;
    st.burstUntil   = now + BURST_MS / 2;
    st.burstActive  = true;
    render("hold-dash", st, now, PREVIEW_DASH, true);
  }

  {  // Long hold: the action menu is armed (replaces the old clear gesture).
    MorseState st = freshState(now);
    setText(st, "SOS HELP");
    render("hold-menu", st, now, PREVIEW_ACTIONS, true);
  }

  {  // Unread badge: 3 new messages.
    MorseState st = freshState(now);
    setText(st, "HELLO");
    st.lastCommitAt = now - 4000;
    uiFull(st, now);
    uiBadge(3, now);
    char file[512];
    snprintf(file, sizeof(file), "%s/%02d-%s.ppm", gOutDir.c_str(), gFrame++,
             "badge");
    writePPM(file);
  }

  {  // Action sheet: long-hold menu with SEND/CLEAR/READ/CLOSE.
    MorseState st = freshState(now);
    setText(st, "HELLO");
    st.lastCommitAt = now - 4000;
    uiFull(st, now);
    uiBadge(2, now);
    static const char* items[] = {"SEND", "CLEAR", "READ 2", "CLOSE"};
    uiActionSheet(items, 4, 0, now);
    char file[512];
    snprintf(file, sizeof(file), "%s/%02d-%s.ppm", gOutDir.c_str(), gFrame++,
             "menu");
    writePPM(file);
  }

  {  // Incoming message card, wrapped body.
    uiMessageCard(7, "HELLO ARE YOU OK MEET ME AT THE PARK AT SIX", 1, 2, false,
                  0, now);
    char file[512];
    snprintf(file, sizeof(file), "%s/%02d-%s.ppm", gOutDir.c_str(), gFrame++,
             "message");
    writePPM(file);
  }

  {  // Message menu: NEXT/CLOSE on a long hold inside the reader.
    uiMessageCard(8, "SECOND MESSAGE", 1, 2, true, 0, now);
    char file[512];
    snprintf(file, sizeof(file), "%s/%02d-%s.ppm", gOutDir.c_str(), gFrame++,
             "message-menu");
    writePPM(file);
  }

  printf("%d frames -> %s\n", gFrame, gOutDir.c_str());
  return 0;
}
