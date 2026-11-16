#pragma once

#include <stdint.h>

// A stylised, fully connected A-Z Morse tree.
//
// The layout mirrors the reference UI: the root sits on the top horizontal
// circuit line, DOT branches run right/down, DASH branches run left/down.
// "Dot" letters are drawn as open circles, "dash" letters as open rectangles,
// exactly like the reference image.
//
// Coordinates are in the 170 x 320 portrait screen space.

enum MorseShape : uint8_t {
  MORSE_DOT  = 0, // letter ends in a dot   -> circle
  MORSE_DASH = 1, // letter ends in a dash  -> rectangle
  MORSE_ROOT = 2  // unlabelled start node -> small dot
};

struct MorseNode {
  char    letter;   // '\0' for the root
  int16_t x, y;     // node centre
  int16_t labelX;   // top-left of the size-1 label glyph cell
  int16_t labelY;
  int8_t  parent;   // -1 for the root
  int8_t  dot;      // child for a DOT, -1 if none
  int8_t  dash;     // child for a DASH, -1 if none
  uint8_t shape;    // MorseShape
};

#define MORSE_ROOT_INDEX 0

extern const MorseNode MORSE_NODES[];
extern const uint8_t   MORSE_NODE_COUNT;

// Child for a symbol, or -1 when this node has no such branch.
int8_t morseChild(uint8_t node, bool dash);

// Number of symbols from the root to this node (root = 0).
uint8_t morseDepth(uint8_t node);
