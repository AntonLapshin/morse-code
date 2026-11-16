#include "net.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <ctype.h>
#include <string.h>

#include "ui.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "firmware/src/secrets.h missing - run ./scripts/gen-secrets.py dad (or emma) --force"
#endif

#ifndef WIFI_SSID
#error "WIFI_SSID missing in secrets.h"
#endif
#ifndef WIFI_PASS
#error "WIFI_PASS missing in secrets.h"
#endif
#ifndef SERVER_HOST
#error "SERVER_HOST missing in secrets.h"
#endif
#ifndef SERVER_PORT
#error "SERVER_PORT missing in secrets.h"
#endif
#ifndef DEVICE_ID
#error "DEVICE_ID missing in secrets.h"
#endif

// Sim parity: red = no WiFi, orange = WiFi without relay, green = both OK.
// Auth failures surface as orange (like the sim's "wifi-only") with an
// "AUTH ERR" toast; they never fake green.

// --- Outbox item (loop -> net task) ------------------------------------------
struct OutItem {
  char text[MSG_TEXT_MAX + 1];
  char cid[48];
};

static QueueHandle_t qOut = nullptr;  // OutItem, depth 8
static QueueHandle_t qIn = nullptr;   // NetInbound, depth 8
static QueueHandle_t qDone = nullptr; // NetSendResult, depth 8

static volatile uint8_t s_link = LINK_OFF;
static uint32_t s_lastSeq = 0;
static uint16_t s_bootId = 1;
static uint32_t s_counter = 0;
static Preferences s_prefs;
static bool s_prefsOpen = false;

static void setLink(uint8_t link) {
  s_link = link;
  uiSetLink(link);
}

static void loadLastSeq() {
  if (!s_prefsOpen) return;
  s_lastSeq = s_prefs.getUInt("last_seq", 0);
}

static void saveLastSeq() {
  if (!s_prefsOpen) return;
  s_prefs.putUInt("last_seq", s_lastSeq);
}

// --- Tiny JSON helpers (ASCII payloads only, like the sim) -------------------
static String jsonEscape(const char *s) {
  String out;
  out.reserve(strlen(s) + 8);
  for (const char *p = s; *p; p++) {
    char c = *p;
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if ((unsigned char)c < 0x20) {
      char tmp[8];
      snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)c);
      out += tmp;
    } else {
      out += c;
    }
  }
  return out;
}

// Find "key" : number (first occurrence). Returns false when missing.
static bool findIntVal(const String &body, const char *key, long &out) {
  String pat = String("\"") + key + "\"";
  int k = body.indexOf(pat);
  if (k < 0) return false;
  int c = body.indexOf(':', (unsigned)k + pat.length());
  if (c < 0) return false;
  unsigned i = (unsigned)c + 1;
  while (i < body.length() && isspace((unsigned char)body[i])) i++;
  bool neg = false;
  if (i < body.length() && (body[i] == '-' || body[i] == '+')) {
    neg = body[i] == '-';
    i++;
  }
  if (i >= body.length() || !isdigit((unsigned char)body[i])) return false;
  long v = 0;
  while (i < body.length() && isdigit((unsigned char)body[i])) {
    v = v * 10 + (body[i] - '0');
    i++;
  }
  out = neg ? -v : v;
  return true;
}

// Extract "key" : "string" with backslash unescaping. False when missing.
static bool extractTextVal(const String &obj, const char *key, String &out) {
  out = "";
  String pat = String("\"") + key + "\"";
  int k = obj.indexOf(pat);
  if (k < 0) return false;
  int c = obj.indexOf(':', (unsigned)k + pat.length());
  if (c < 0) return false;
  int q = obj.indexOf('"', (unsigned)c + 1);
  if (q < 0) return false;
  String val;
  val.reserve(96);
  for (unsigned i = (unsigned)q + 1; i < obj.length(); i++) {
    char ch = obj[i];
    if (ch == '\\') {
      if (i + 1 >= obj.length()) return false;
      char e = obj[++i];
      switch (e) {
        case '"': val += '"'; break;
        case '\\': val += '\\'; break;
        case '/': val += '/'; break;
        case 'n': val += '\n'; break;
        case 'r': val += '\r'; break;
        case 't': val += '\t'; break;
        case 'b': val += '\b'; break;
        case 'f': val += '\f'; break;
        case 'u': {
          if (i + 4 >= obj.length()) return false;
          unsigned v = 0;
          for (int k2 = 0; k2 < 4; k2++) {
            char h = obj[++i];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
            else return false;
          }
          val += (v < 0x80) ? (char)v : '?';
          break;
        }
        default: return false;
      }
    } else if (ch == '"') {
      out = val;
      return true;
    } else {
      val += ch;
    }
  }
  return false;
}

struct ParsedMsg {
  long seq = 0;
  String text;
};

// Parse the messages[] array: every {"seq":N,...,"text":"..."} object.
// Returns the item count; latest is set from "latest_seq" when present.
static size_t parseMessages(const String &body, ParsedMsg *out, size_t cap,
                            long &latest) {
  size_t n = 0;
  findIntVal(body, "latest_seq", latest);
  int arr = body.indexOf("\"messages\"");
  if (arr < 0) return 0;
  int b = body.indexOf('[', (unsigned)arr);
  if (b < 0) return 0;
  int arrEnd = body.indexOf(']', (unsigned)b + 1);
  unsigned i = (unsigned)b + 1;
  while (i < body.length() && (arrEnd < 0 || (int)i < arrEnd)) {
    int o = body.indexOf('{', i);
    if (o < 0 || (arrEnd >= 0 && o > arrEnd)) break;
    int e = body.indexOf('}', (unsigned)o + 1);
    if (e < 0) break;
    String obj = body.substring((unsigned)o, (unsigned)e + 1);
    long seq = 0;
    if (findIntVal(obj, "seq", seq) && seq > 0 && n < cap) {
      String text;
      if (extractTextVal(obj, "text", text)) {
        out[n].seq = seq;
        out[n].text = text;
        n++;
      }
    }
    i = (unsigned)e + 1;
  }
  return n;
}

// --- HTTP helpers (net task only) --------------------------------------------
static String baseUrl() {
  String url = "http://";
  url += SERVER_HOST;
  url += ":";
  url += String(SERVER_PORT);
  return url;
}

static int httpPostJson(const String &path, const String &payload,
                        String &respBody) {
  respBody = "";
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(10000);
  http.setReuse(false);
  String url = baseUrl() + path;
  if (!http.begin(url)) return -1;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
  int code = http.POST(payload);
  if (code > 0) {
    String body = http.getString();
    if (body.length() > 8192) body.remove(8192);
    respBody = body;
  }
  http.end();
  return code;
}

static int httpGet(const String &path, String &respBody) {
  respBody = "";
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  http.setReuse(false);
  String url = baseUrl() + path;
  if (!http.begin(url)) return -1;
  http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
  int code = http.GET();
  if (code > 0) {
    String body = http.getString();
    if (body.length() > 8192) body.remove(8192);
    respBody = body;
  }
  http.end();
  return code;
}

// --- Net task ------------------------------------------------------------------
static void netTask(void * /*arg*/) {
  s_prefs.begin("morse", false);
  s_prefsOpen = true;
  loadLastSeq();
  Serial.printf("net: device=%s relay=%s:%d last_seq=%u\n", DEVICE_ID,
                SERVER_HOST, (int)SERVER_PORT, (unsigned)s_lastSeq);

  OutItem cur;
  bool haveCur = false;
  uint32_t backoffMs = 1000;
  uint32_t notBefore = 0;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      setLink(LINK_OFF);
      WiFi.mode(WIFI_STA);
      WiFi.persistent(false);
      WiFi.setAutoReconnect(true);
      WiFi.setSleep(false);
      {
        String hn = String("morse-") + DEVICE_ID;
        WiFi.setHostname(hn.c_str());
      }
      Serial.printf("net: wifi connecting to \"%s\"\n", WIFI_SSID);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      uint32_t t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        vTaskDelay(pdMS_TO_TICKS(250));
      }
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("net: wifi timeout, retry in 5s");
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }
      Serial.printf("net: wifi ok ip=%s\n",
                    WiFi.localIP().toString().c_str());
      setLink(LINK_WIFI);
    }

    uint32_t now = millis();

    // --- TX: one POST per cycle ---------------------------------------------
    if (!haveCur && xQueueReceive(qOut, &cur, 0) == pdTRUE) haveCur = true;
    if (haveCur && (int32_t)(now - notBefore) >= 0) {
      String payload = String("{\"client_msg_id\":\"") + cur.cid +
                       "\",\"to\":\"family\",\"text\":\"" +
                       jsonEscape(cur.text) + "\"}";
      String resp;
      int code = httpPostJson("/api/v1/messages", payload, resp);
      if (code == 200 || code == 201) {
        long seq = 0;
        if (findIntVal(resp, "seq", seq) && seq > 0 &&
            (uint32_t)seq > s_lastSeq) {
          s_lastSeq = (uint32_t)seq;
          saveLastSeq();
        }
        setLink(LINK_OK);
        NetSendResult r;
        r.ok = true;
        r.err[0] = '\0';
        xQueueSend(qDone, &r, 0);
        Serial.printf("tx: ok seq=%ld text=\"%s\"\n", seq, cur.text);
        haveCur = false;
        backoffMs = 1000;
      } else if (code == 401) {
        NetSendResult r;
        r.ok = false;
        snprintf(r.err, sizeof(r.err), "AUTH ERR");
        xQueueSend(qDone, &r, 0);
        Serial.println("tx: 401 auth rejected (token mismatch with relay .env?)");
        setLink(LINK_WIFI);
        haveCur = false; // drop: sim parity, a wrong token never fixes itself
      } else if (code == 400 || code == 404 || code == 413) {
        NetSendResult r;
        r.ok = false;
        snprintf(r.err, sizeof(r.err), "REJECTED");
        xQueueSend(qDone, &r, 0);
        Serial.printf("tx: HTTP %d rejected, dropping: %s\n", code,
                      resp.c_str());
        haveCur = false;
      } else if (code == 429) {
        NetSendResult r;
        r.ok = false;
        snprintf(r.err, sizeof(r.err), "RATE LTD");
        xQueueSend(qDone, &r, 0);
        Serial.println("tx: 429 rate limited, dropping");
        haveCur = false;
        notBefore = now + 5000;
      } else if (code > 0) { // 5xx
        Serial.printf("tx: HTTP %d, retry in %ums\n", code,
                      (unsigned)backoffMs);
        setLink(LINK_WIFI);
        notBefore = now + backoffMs;
        backoffMs = backoffMs * 2 > 30000 ? 30000 : backoffMs * 2;
      } else { // transport failure / timeout
        Serial.printf("tx: unreachable, retry in %ums\n",
                      (unsigned)backoffMs);
        setLink(LINK_WIFI);
        notBefore = now + backoffMs;
        backoffMs = backoffMs * 2 > 30000 ? 30000 : backoffMs * 2;
      }
    }

    // --- RX poll: wait=0 so TX stays responsive (sim uses the same) ----------
    {
      char path[64];
      snprintf(path, sizeof(path),
               "/api/v1/messages?since=%u&wait=0&limit=8",
               (unsigned)s_lastSeq);
      String resp;
      int code = httpGet(path, resp);
      if (code == 200) {
        long latest = (long)s_lastSeq;
        ParsedMsg msgs[8];
        size_t n = parseMessages(resp, msgs, 8, latest);
        bool advanced = false;
        for (size_t i = 0; i < n; i++) {
          if (msgs[i].seq <= 0 || (uint32_t)msgs[i].seq <= s_lastSeq) continue;
          if (msgs[i].text.length() == 0) {
            if ((uint32_t)msgs[i].seq > s_lastSeq) {
              s_lastSeq = (uint32_t)msgs[i].seq;
              advanced = true;
            }
            continue;
          }
          NetInbound in;
          in.seq = (uint32_t)msgs[i].seq;
          size_t L = msgs[i].text.length();
          if (L > MSG_TEXT_MAX) L = MSG_TEXT_MAX;
          memcpy(in.text, msgs[i].text.c_str(), L);
          in.text[L] = '\0';
          if (xQueueSend(qIn, &in, 0) != pdTRUE) break; // full: retry next poll
          s_lastSeq = in.seq;
          advanced = true;
          Serial.printf("rx: seq=%u len=%u\n", (unsigned)in.seq,
                        (unsigned)L);
        }
        if (latest > 0 && (uint32_t)latest > s_lastSeq) {
          s_lastSeq = (uint32_t)latest; // truncated window: jump like the sim
          advanced = true;
        }
        if (advanced) saveLastSeq();
        // Best-effort ack so the relay unread count tracks the device.
        char ack[32];
        snprintf(ack, sizeof(ack), "{\"upto_seq\":%u}", (unsigned)s_lastSeq);
        String aresp;
        httpPostJson("/api/v1/ack", ack, aresp);
        setLink(LINK_OK);
      } else if (code == 401) {
        setLink(LINK_WIFI);
      } else {
        setLink(LINK_WIFI);
      }
    }

    UBaseType_t waiting = qOut ? uxQueueMessagesWaiting(qOut) : 0;
    if (haveCur || waiting > 0)
      vTaskDelay(pdMS_TO_TICKS(250));
    else
      vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// --- Public API ------------------------------------------------------------------
void netBegin() {
  if (!qOut) qOut = xQueueCreate(8, sizeof(OutItem));
  if (!qIn) qIn = xQueueCreate(8, sizeof(NetInbound));
  if (!qDone) qDone = xQueueCreate(8, sizeof(NetSendResult));
  s_bootId = (uint16_t)(esp_random() & 0xFFFF);
  if (s_bootId == 0) s_bootId = 1;
  s_counter = 0;
  setLink(LINK_OFF);
  xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, 0);
}

bool netEnqueueSend(const char *text) {
  if (!text || !qOut) return false;
  while (*text == ' ') text++; // server rejects whitespace-only
  if (*text == '\0') return false;
  size_t n = strlen(text);
  while (n > 0 && text[n - 1] == ' ') n--;
  if (n == 0) return false;
  if (n > MSG_TEXT_MAX) n = MSG_TEXT_MAX;
  OutItem it;
  memcpy(it.text, text, n);
  it.text[n] = '\0';
  s_counter++;
  snprintf(it.cid, sizeof(it.cid), "%s-%04x-%lu", DEVICE_ID,
           (unsigned)s_bootId, (unsigned long)s_counter);
  if (xQueueSend(qOut, &it, 0) != pdTRUE) return false;
  Serial.printf("tx: queued cid=%s len=%u\n", it.cid, (unsigned)n);
  return true;
}

bool netPollInbound(NetInbound *out) {
  if (!out || !qIn) return false;
  return xQueueReceive(qIn, out, 0) == pdTRUE;
}

bool netPollSendResult(NetSendResult *out) {
  if (!out || !qDone) return false;
  return xQueueReceive(qDone, out, 0) == pdTRUE;
}

uint8_t netLink() { return s_link; }
