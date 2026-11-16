#include "morse.h"

// Index  Letter      x    y    lx   ly   parent dot dash shape
// ---------------------------------------------------------------------------
// Label convention (5x7 font, 5px ink): top row centred above (x-2, 40);
// side labels vertically centred (y-4) with a 5px gap from the node edge
// (dot: x+/-10/15, dash: x+/-12/17); below labels (y+9 dash / y+10 dot)
// centred (x-2) except A/W which sit left-below to stay off the x=101 wire.
const MorseNode MORSE_NODES[] = {
  { '\0',         87,  62,  -1,  -1,  -1,  1,  2, MORSE_ROOT }, // 0  start
  { 'E',         101,  62,  99,  40,   0,  3,  4, MORSE_DOT  }, // 1  .
  { 'T',          72,  62,  70,  40,   0,  5,  6, MORSE_DASH }, // 2  -
  { 'I',         119,  62, 117,  40,   1,  7,  8, MORSE_DOT  }, // 3  ..
  { 'A',         101, 160,  84, 169,   1,  9, 10, MORSE_DASH }, // 4  .-
  { 'N',          72, 160,  82, 156,   2, 11, 12, MORSE_DOT  }, // 5  -.
  { 'M',          46,  62,  44,  40,   2, 13, 14, MORSE_DASH }, // 6  --
  { 'S',         137,  62, 135,  40,   3, 15, 16, MORSE_DOT  }, // 7  ...
  { 'U',         119,  98, 102,  94,   3, 17, -1, MORSE_DASH }, // 8  ..-
  { 'R',         121, 160, 119, 170,   4, 18, -1, MORSE_DOT  }, // 9  .-.
  { 'W',         101, 215,  84, 224,   4, 19, 20, MORSE_DASH }, // 10 .--
  { 'D',          72, 215,  82, 211,   5, 21, 22, MORSE_DOT  }, // 11 -..
  { 'K',          44, 160,  42, 144,   5, 23, 24, MORSE_DASH }, // 12 -.-
  { 'G',          46,  98,  56,  94,   6, 25, 26, MORSE_DOT  }, // 13 --.
  { 'O',          22,  62,  20,  40,   6, -1, -1, MORSE_DASH }, // 14 ---
  { 'H',         156,  62, 154,  40,   7, -1, -1, MORSE_DOT  }, // 15 ....
  { 'V',         137,  98, 149,  94,   7, -1, -1, MORSE_DASH }, // 16 ...-
  { 'F',         119, 126, 129, 122,   8, -1, -1, MORSE_DOT  }, // 17 ..-.
  { 'L',         142, 160, 152, 156,   9, -1, -1, MORSE_DOT  }, // 18 .-..
  { 'P',         119, 215, 129, 211,  10, -1, -1, MORSE_DOT  }, // 19 .--.
  { 'J',         101, 249, 113, 245,  10, -1, -1, MORSE_DASH }, // 20 .---
  { 'B',          72, 249,  57, 245,  11, -1, -1, MORSE_DOT  }, // 21 -...
  { 'X',          41, 215,  24, 211,  11, -1, -1, MORSE_DASH }, // 22 -..-
  { 'C',          46, 188,  56, 184,  12, -1, -1, MORSE_DOT  }, // 23 -.-.
  { 'Y',          23, 160,  21, 144,  12, -1, -1, MORSE_DASH }, // 24 -.--
  { 'Z',          46, 126,  56, 122,  13, -1, -1, MORSE_DOT  }, // 25 --..
  { 'Q',          24,  98,  22, 107,  13, -1, -1, MORSE_DASH }, // 26 --.-
};

const uint8_t MORSE_NODE_COUNT = sizeof(MORSE_NODES) / sizeof(MORSE_NODES[0]);

int8_t morseChild(uint8_t node, bool dash) {
  if (node >= MORSE_NODE_COUNT) return -1;
  return dash ? MORSE_NODES[node].dash : MORSE_NODES[node].dot;
}

uint8_t morseDepth(uint8_t node) {
  uint8_t depth = 0;
  int8_t  idx   = (int8_t)node;
  while (idx > 0 && depth < 16) {
    idx = MORSE_NODES[idx].parent;
    depth++;
  }
  return depth;
}
