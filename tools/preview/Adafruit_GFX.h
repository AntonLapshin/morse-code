// Host-side Adafruit_GFX shim for the UI preview tool.
// Mirrors the classic 5x7 font drawing and the primitives used by ui.cpp.
#pragma once

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "font5x7.h"

class Adafruit_GFX {
public:
  Adafruit_GFX(int16_t w, int16_t h)
      : _width(w), _height(h), _buf((size_t)w * (size_t)h, 0) {}
  virtual ~Adafruit_GFX() = default;

  int16_t width() const { return _width; }
  int16_t height() const { return _height; }

  const std::vector<uint16_t>& framebuffer() const { return _buf; }

  void setTextWrap(bool w) { wrap = w; }
  void setTextSize(uint8_t s) { textsize_x = textsize_y = s; }
  void setTextColor(uint16_t c) { textcolor = textbgcolor = c; }
  void setTextColor(uint16_t c, uint16_t b) { textcolor = c; textbgcolor = b; }
  void setCursor(int16_t x, int16_t y) { cursor_x = x; cursor_y = y; }

  // Adafruit_SPITFT-style blit API, used by ui.cpp to push the canvas.
  void startWrite() {}
  void endWrite() {}
  void setAddrWindow(int16_t x, int16_t y, int16_t w, int16_t h) {
    win_x = x;
    win_y = y;
    win_w = w;
    win_h = h;
    win_i = 0;
  }
  void writePixels(uint16_t* colors, uint32_t len) {
    if (win_w <= 0) return;
    for (uint32_t i = 0; i < len; i++) {
      int16_t x = win_x + (int16_t)(win_i % win_w);
      int16_t y = win_y + (int16_t)(win_i / win_w);
      if (x >= 0 && y >= 0 && x < _width && y < _height)
        _buf[(size_t)y * _width + x] = colors[i];
      win_i++;
    }
  }

  void fillScreen(uint16_t color) { std::fill(_buf.begin(), _buf.end(), color); }

  void drawPixel(int16_t x, int16_t y, uint16_t color) {
    if (x < 0 || y < 0 || x >= _width || y >= _height) return;
    _buf[(size_t)y * _width + x] = color;
  }
  void writePixel(int16_t x, int16_t y, uint16_t color) { drawPixel(x, y, color); }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    int16_t x0 = x < 0 ? 0 : x;
    int16_t y0 = y < 0 ? 0 : y;
    int16_t x1 = x + w > _width ? _width : x + w;
    int16_t y1 = y + h > _height ? _height : y + h;
    for (int16_t yy = y0; yy < y1; yy++)
      for (int16_t xx = x0; xx < x1; xx++)
        _buf[(size_t)yy * _width + xx] = color;
  }

  void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    fillRect(x, y, w, 1, color);
    fillRect(x, y + h - 1, w, 1, color);
    fillRect(x, y, 1, h, color);
    fillRect(x + w - 1, y, 1, h, color);
  }

  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
    int16_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int16_t dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int16_t sx = (x0 < x1) ? 1 : -1;
    int16_t sy = (y0 < y1) ? 1 : -1;
    int16_t err = dx - dy;
    for (;;) {
      drawPixel(x0, y0, color);
      if (x0 == x1 && y0 == y1) break;
      int16_t e2 = 2 * err;
      if (e2 > -dy) { err -= dy; x0 += sx; }
      if (e2 < dx) { err += dx; y0 += sy; }
    }
  }

  void drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
    int16_t f = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t x = 0;
    int16_t y = r;
    drawPixel(x0, y0 + r, color);
    drawPixel(x0, y0 - r, color);
    drawPixel(x0 + r, y0, color);
    drawPixel(x0 - r, y0, color);
    while (x < y) {
      if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
      x++;
      ddF_x += 2;
      f += ddF_x;
      drawPixel(x0 + x, y0 + y, color);
      drawPixel(x0 - x, y0 + y, color);
      drawPixel(x0 + x, y0 - y, color);
      drawPixel(x0 - x, y0 - y, color);
      drawPixel(x0 + y, y0 + x, color);
      drawPixel(x0 - y, y0 + x, color);
      drawPixel(x0 + y, y0 - x, color);
      drawPixel(x0 - y, y0 - x, color);
    }
  }

  void fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
    for (int16_t y = -r; y <= r; y++) {
      int16_t w = (int16_t)(sqrtf((float)(r * r - y * y)) + 0.5f);
      fillRect(x0 - w, y0 + y, 2 * w + 1, 1, color);
    }
  }

  // Classic 5x7 font, opaque background like Adafruit_GFX::drawChar().
  void drawChar(int16_t x, int16_t y, unsigned char c, uint16_t color,
                uint16_t bg, uint8_t size_x, uint8_t size_y) {
    if (x >= _width || y >= _height || (x + 6 * size_x - 1) < 0 ||
        (y + 8 * size_y - 1) < 0)
      return;
    if (c >= 176) c++;
    for (int8_t i = 0; i < 5; i++) {
      uint8_t line = font[c * 5 + i];
      for (int8_t j = 0; j < 8; j++, line >>= 1) {
        if (line & 1) {
          fillRect(x + i * size_x, y + j * size_y, size_x, size_y, color);
        } else if (bg != color) {
          fillRect(x + i * size_x, y + j * size_y, size_x, size_y, bg);
        }
      }
    }
    if (bg != color) fillRect(x + 5 * size_x, y, size_x, 8 * size_y, bg);
  }

  void print(char c) {
    if (c == '\n') {
      cursor_x = 0;
      cursor_y += textsize_y * 8;
      return;
    }
    if (c == '\r') return;
    drawChar(cursor_x, cursor_y, (uint8_t)c, textcolor, textbgcolor,
             textsize_x, textsize_y);
    cursor_x += textsize_x * 6;
  }

  void print(const char* s) {
    while (s && *s) print(*s++);
  }

  void getTextBounds(const char* str, int16_t x, int16_t y, int16_t* x1,
                     int16_t* y1, uint16_t* w, uint16_t* h) {
    *x1 = x;
    *y1 = y;
    *w = 0;
    *h = 0;
    int n = (int)strlen(str);
    if (n == 0) return;
    *w = (uint16_t)(6 * textsize_x * n);
    *h = (uint16_t)(8 * textsize_y);
  }

protected:
  int16_t _width;
  int16_t _height;
  std::vector<uint16_t> _buf;

  int16_t cursor_x = 0;
  int16_t cursor_y = 0;
  uint8_t textsize_x = 1;
  uint8_t textsize_y = 1;
  uint16_t textcolor = 0xFFFF;
  uint16_t textbgcolor = 0xFFFF;
  bool wrap = true;

  int16_t win_x = 0;
  int16_t win_y = 0;
  int16_t win_w = 0;
  int16_t win_h = 0;
  uint32_t win_i = 0;
};

// Minimal host-side stand-in for the real GFXcanvas16.
class GFXcanvas16 : public Adafruit_GFX {
public:
  GFXcanvas16(uint16_t w, uint16_t h) : Adafruit_GFX((int16_t)w, (int16_t)h) {}
  uint16_t* getBuffer() { return _buf.data(); }
};
