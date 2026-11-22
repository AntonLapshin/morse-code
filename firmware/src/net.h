#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

// Network layer: WiFi lifecycle + Worker POST (TX) + long-poll GET
// (RX link check + inbox delivery), mirroring tools/sim/sim.cpp behavior.
//
// Since 0.2.0 the relay is a Cloudflare Worker + Durable Object
// (https://morse.gulfrenderstudio.com, see worker/README.md) — no laptop
// server, no LAN address. SERVER_URL comes from firmware/src/secrets.h,
// generated from MORSE_SERVER_URL in .env.
//
// The UI task (`loop()`) owns all rendering/SPI. The net task only calls
// uiSetLink() and otherwise talks through these queues; `loop()` drains them.
struct NetInbound {
  uint32_t seq;
  char text[MSG_TEXT_MAX + 1];
};

struct NetSendResult {
  bool ok;
  char err[24]; // short display string: "" on ok, e.g. "AUTH ERR"
};

// Start WiFi + the net task. Requires firmware/src/secrets.h (generated from
// .env by scripts/gen-secrets.py). Safe to call once from setup().
void netBegin();

// Queue text for POST /api/v1/messages. Copies at most MSG_TEXT_MAX chars.
// Returns false when blank or the outbox (depth 8) is full. The compose buffer
// should clear immediately on true, exactly like the sim's queueSend().
bool netEnqueueSend(const char *text);

// Drain one inbound relay message (false when empty). Call repeatedly.
bool netPollInbound(NetInbound *out);

// Drain one finished send (false when none). ok=true means the relay stored
// it ("SENT"); ok=false carries a short err ("AUTH ERR", "REJECTED", ...).
bool netPollSendResult(NetSendResult *out);

// Current link dot state (LINK_OFF / LINK_WIFI / LINK_OK from ui.h).
uint8_t netLink();
