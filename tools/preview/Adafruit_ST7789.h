// Host-side Adafruit_ST7789 shim for the UI preview tool.
#pragma once

#include "Adafruit_GFX.h"

class Adafruit_ST7789 : public Adafruit_GFX {
public:
  Adafruit_ST7789(int8_t cs, int8_t dc, int8_t rst) : Adafruit_GFX(170, 320) {
    (void)cs;
    (void)dc;
    (void)rst;
  }

  void init(uint16_t w, uint16_t h, uint8_t mode = 0) {
    (void)mode;
    _width = (int16_t)w;
    _height = (int16_t)h;
    _buf.assign((size_t)w * h, 0);
  }

  void setRotation(uint8_t r) { rotation = r; }
  void invertDisplay(bool invert) { (void)invert; }

private:
  uint8_t rotation = 0;
};
