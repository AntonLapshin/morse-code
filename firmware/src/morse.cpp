#include "morse.h"

// Index  Letter      x    y    lx   ly   parent dot dash shape
// ---------------------------------------------------------------------------
// Grid layout: 8 columns x 7 rows, uniform 21px horizontal / 30px vertical.
//   xs = 11, 32, 53, 74, 95, 116, 137, 158 (left rect edge = 5px padding)
//   ys = 70, 100, 130, 160, 190, 220, 250 (top moved down 8px from title)
// Every parent->child edge is axis-aligned: horizontal = 1 column (21px),
// vertical = N rows (30px multiples). All node centres sit on grid lines.
// Label convention (5x7 font, 5px ink): top row centred above (x-2, 48);
// side labels vertically centred (y-4) with ~6px gap from the node edge
// (dot: x+10 right / x-15 left, dash: x+12 right / x-17 left);
// below labels centred (x-2, y+9 dot / y+9 dash, R uses y+10);
// K/Y use above-centred (x-2, y-16) to stay off the y=160 wire.
const MorseNode MORSE_NODES[] = {
  { '\0',         74,  70,  -1,  -1,  -1,  1,  2, MORSE_ROOT }, // 0  start
  { 'E',          95,  70,  93,  48,   0,  3,  4, MORSE_DOT  }, // 1  .
  { 'T',          53,  70,  51,  48,   0,  5,  6, MORSE_DASH }, // 2  -
  { 'I',         116,  70, 114,  48,   1,  7,  8, MORSE_DOT  }, // 3  ..
  { 'A',          95, 160,  78, 169,   1,  9, 10, MORSE_DASH }, // 4  .-
  { 'N',          53, 160,  63, 156,   2, 11, 12, MORSE_DOT  }, // 5  -.
  { 'M',          32,  70,  30,  48,   2, 13, 14, MORSE_DASH }, // 6  --
  { 'S',         137,  70, 135,  48,   3, 15, 16, MORSE_DOT  }, // 7  ...
  { 'U',         116, 100,  99,  96,   3, 17, -1, MORSE_DASH }, // 8  ..-
  { 'R',         116, 160, 114, 170,   4, 18, -1, MORSE_DOT  }, // 9  .-.
  { 'W',          95, 220,  78, 229,   4, 19, 20, MORSE_DASH }, // 10 .--
  { 'D',          53, 220,  63, 216,   5, 21, 22, MORSE_DOT  }, // 11 -..
  { 'K',          32, 160,  30, 144,   5, 23, 24, MORSE_DASH }, // 12 -.-
  { 'G',          32, 100,  42,  96,   6, 25, 26, MORSE_DOT  }, // 13 --.
  { 'O',          11,  70,   9,  48,   6, -1, -1, MORSE_DASH }, // 14 ---
  { 'H',         158,  70, 156,  48,   7, -1, -1, MORSE_DOT  }, // 15 ....
  { 'V',         137, 100, 149,  96,   7, -1, -1, MORSE_DASH }, // 16 ...-
  { 'F',         116, 130, 126, 126,   8, -1, -1, MORSE_DOT  }, // 17 ..-.
  { 'L',         137, 160, 147, 156,   9, -1, -1, MORSE_DOT  }, // 18 .-..
  { 'P',         116, 220, 126, 216,  10, -1, -1, MORSE_DOT  }, // 19 .--.
  { 'J',          95, 250, 107, 246,  10, -1, -1, MORSE_DASH }, // 20 .---
  { 'B',          53, 250,  38, 246,  11, -1, -1, MORSE_DOT  }, // 21 -...
  { 'X',          32, 220,  15, 216,  11, -1, -1, MORSE_DASH }, // 22 -..-
  { 'C',          32, 190,  42, 186,  12, -1, -1, MORSE_DOT  }, // 23 -.-.
  { 'Y',          11, 160,   9, 144,  12, -1, -1, MORSE_DASH }, // 24 -.--
  { 'Z',          32, 130,  42, 126,  13, -1, -1, MORSE_DOT  }, // 25 --..
  { 'Q',          11, 100,   9, 109,  13, -1, -1, MORSE_DASH }, // 26 --.-
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
