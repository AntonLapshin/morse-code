// morse-code UI simulator — two ESP32 + ST7789 devices with live display.
//
// Links the REAL firmware renderer (firmware/src/ui.cpp + morse.cpp) against
// the host Adafruit shims (tools/preview/) and exposes it through a tiny
// stdlib-only HTTP server, so a browser shows exactly what each panel shows.
//
// Two virtual devices (dad + emma) run in this one process. Each has its own
// input state machine (mirroring firmware/src/main.cpp), its own inbox, and
// its own relay poller, so a SEND on one panel appears on the other panel via
// the REAL relay — the local Cloudflare Worker dev server by default
// (MORSE_SERVER_URL=http://localhost:8787, see --relay-url / .env).
//
// The sim's HTTP client is plain HTTP over raw sockets: it CANNOT do TLS,
// so it cannot talk to the production Worker at the HTTPS URL in
// MORSE_SERVER_URL. Point the sim at the local Worker
// (make worker-dev) and test production with curl instead — see
// worker/README.md. Pointing the sim at an https:// URL reports
// "sim cannot do TLS" on the link dot instead of silently failing.
//
// Link dot (top-left, see UiLink in ui.h):
//   red    = no WiFi (toggle per device, or no relay configured)
//   orange = WiFi OK, but no connection to the relay (down / bad token)
//   green  = both WiFi and relay OK
//
// Usage:
//   make sim                 # build + serve on http://localhost:8077
//   ./tools/sim/build/sim --port 8077
//   ./tools/sim/build/sim --dump tools/sim/out   # headless screenshots (PPM)
//
// The relay tokens come from the environment (MORSE_TOKEN_DAD /
// MORSE_TOKEN_EMMA) or the repo-root .env, or --dad-token / --emma-token.
// The browser UI can also reconfigure host/port/tokens at runtime via
// POST /api/relay.
//
// No dependencies beyond a C++17 compiler, POSIX sockets, and pthreads.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <Adafruit_ST7789.h>

#include "config.h"
#include "morse.h"
#include "ui.h"

// -----------------------------------------------------------------------------
// One virtual device (mirrors firmware/src/main.cpp per-device state)
// -----------------------------------------------------------------------------
enum AppMode : uint8_t { MODE_COMPOSE = 0, MODE_ACTIONS = 1, MODE_MESSAGE = 2 };

struct InboundItem {
  long long seq = 0;
  std::string from;
  std::string text;
};

struct DeviceSim {
  std::string id;
  Adafruit_ST7789 display;

  MorseState st;
  bool inRaw = false;
  bool inPressed = false;
  bool virtDown = false;
  uint32_t inRawChanged = 0;
  uint32_t inPressedAt = 0;
  uint8_t previewState = PREVIEW_NONE;
  uint32_t lastPreviewDraw = 0;
  uint32_t lastProgressDraw = 0;
  bool longFired = false;

  AppMode appMode = MODE_COMPOSE;
  InboxMsg inbox[INBOX_MAX];
  uint8_t inboxCount = 0;
  uint32_t nextMsgId = 1;
  char actionBuf[5][16];
  const char* actionPtrs[5];
  uint8_t actionCount = 0;
  uint8_t actionSel = 0;
  uint8_t msgIdx = 0;
  bool msgMenuOpen = false;
  uint8_t msgMenuSel = 0;

  // Relay sync, shared with the background poll thread. Everything below is
  // guarded by mtx; the Morse/input state above is main-thread only.
  std::mutex mtx;
  std::string token;
  long long lastSeq = 0;
  long long ackedSeq = 0;
  uint8_t link = LINK_OFF;
  uint8_t linkCache = 0xFF; // main-thread copy to detect dot changes
  std::string relayError = "starting";
  std::string pendingSend;
  uint64_t sendCounter = 0;
  bool wifiOn = true;
  std::vector<InboundItem> inbound;

  DeviceSim(const char* devId) : id(devId), display(15, 2, 4) {
    memset(&st, 0, sizeof(st));
    memset(actionBuf, 0, sizeof(actionBuf));
    memset(actionPtrs, 0, sizeof(actionPtrs));
  }
  DeviceSim(const DeviceSim&) = delete;
  DeviceSim& operator=(const DeviceSim&) = delete;
};

static DeviceSim g_dad("dad");
static DeviceSim g_emma("emma");

static DeviceSim* devById(const std::string& id) {
  if (id == "dad") return &g_dad;
  if (id == "emma") return &g_emma;
  return nullptr;
}

// Render target switch for the shared ui.cpp canvas (see uiAttach).
static void selectDevice(DeviceSim& d, uint8_t link) {
  uiAttach(&d.display);
  uiSetLink(link);
  // The on-screen name is the device id, upper-cased ("DAD" / "EMMA") —
  // the same value gen-secrets.py writes as DEVICE_NAME for the firmware.
  std::string upper = d.id;
  for (char& c : upper) c = (char)toupper((unsigned char)c);
  uiSetDeviceName(upper.c_str());
}

static uint8_t deviceLink(DeviceSim& d) {
  std::lock_guard<std::mutex> lk(d.mtx);
  return d.wifiOn ? d.link : (uint8_t)LINK_OFF;
}

// -----------------------------------------------------------------------------
// Relay configuration (one server URL shared, tokens per device)
// -----------------------------------------------------------------------------
// Single URL, not host/port: the relay is the Cloudflare Worker, and the
// sim talks to the LOCAL Worker dev server over plain HTTP (see the header
// comment — the sim cannot do TLS, so no https:// URL works here).
static std::string g_serverUrl = "http://localhost:8787";
static std::atomic<bool> g_relayEnabled{true};
static std::atomic<bool> g_running{true};

// Parse "http://host:port[/path]" into host + port. https:// (or anything
// else) is an error: the sim has no TLS stack. Returns false with `error`
// set when the URL cannot be used by this sim.
static bool parseServerUrl(const std::string& url, std::string& host, int& port,
                           std::string& error) {
  host.clear();
  port = 80;
  error.clear();
  if (url.rfind("https://", 0) == 0) {
    error = "sim cannot do TLS: use the local Worker http://localhost:8787 (make worker-dev)";
    return false;
  }
  std::string rest = url;
  if (rest.rfind("http://", 0) == 0) rest = rest.substr(7);
  size_t slash = rest.find('/');
  if (slash != std::string::npos) rest = rest.substr(0, slash);
  if (rest.empty()) {
    error = "empty server URL";
    return false;
  }
  size_t colon = rest.rfind(':');
  if (colon != std::string::npos) {
    host = rest.substr(0, colon);
    port = atoi(rest.substr(colon + 1).c_str());
    if (port < 1 || port > 65535) {
      error = "bad port in server URL";
      return false;
    }
  } else {
    host = rest;
    port = 80;
  }
  if (host.empty()) {
    error = "empty host in server URL";
    return false;
  }
  return true;
}

static std::string envOr(const char* key, const std::string& dflt) {
  const char* v = getenv(key);
  return (v && *v) ? std::string(v) : dflt;
}

static void loadDotEnvFile(const std::string& path,
                           std::map<std::string, std::string>& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return;
  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    if (i >= s.size() || s[i] == '#') continue;
    if (s.compare(i, 7, "export ") == 0) i += 7;
    size_t eq = s.find('=', i);
    if (eq == std::string::npos) continue;
    std::string key = s.substr(i, eq - i);
    while (!key.empty() && isspace((unsigned char)key.back())) key.pop_back();
    std::string val = s.substr(eq + 1);
    size_t a = 0;
    while (a < val.size() && isspace((unsigned char)val[a])) a++;
    size_t b = val.size();
    while (b > a && isspace((unsigned char)val[b - 1])) b--;
    val = val.substr(a, b > a ? b - a : 0);
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
      val = val.substr(1, val.size() - 2);
    else if (val.size() >= 2 && val.front() == '\'' && val.back() == '\'')
      val = val.substr(1, val.size() - 2);
    if (!key.empty()) out[key] = val;
  }
  fclose(f);
}

static void loadRelayConfigFromEnv() {
  const char* explicitEnv = getenv("MORSE_ENV_FILE");
  std::map<std::string, std::string> file;
  if (explicitEnv && *explicitEnv) {
    loadDotEnvFile(explicitEnv, file);
  } else {
    loadDotEnvFile(".env", file);
    if (file.empty()) loadDotEnvFile("../.env", file);
    if (file.empty()) loadDotEnvFile("../../.env", file);
  }
  auto pick = [&](const char* key, const std::string& dflt) {
    std::string v = envOr(key, "");
    if (!v.empty()) return v;
    auto it = file.find(key);
    if (it != file.end()) return it->second;
    return dflt;
  };
  g_serverUrl = pick("MORSE_SERVER_URL", g_serverUrl);
  std::string dad = pick("MORSE_TOKEN_DAD", "");
  std::string emma = pick("MORSE_TOKEN_EMMA", "");
  if (!dad.empty()) {
    std::lock_guard<std::mutex> lk(g_dad.mtx);
    g_dad.token = dad;
  }
  if (!emma.empty()) {
    std::lock_guard<std::mutex> lk(g_emma.mtx);
    g_emma.token = emma;
  }
}

// -----------------------------------------------------------------------------
// Minimal HTTP client for the relay (background thread only)
// -----------------------------------------------------------------------------
static bool httpDo(const std::string& host, int port, const std::string& method,
                   const std::string& path, const std::string& token,
                   const std::string& body, int& status, std::string& respBody,
                   int recvTimeoutS = 4) {
  status = 0;
  respBody.clear();
  char portStr[16];
  snprintf(portStr, sizeof(portStr), "%d", port);
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* ai = nullptr;
  if (getaddrinfo(host.c_str(), portStr, &hints, &ai) != 0 || !ai) return false;

  int fd = -1;
  for (addrinfo* p = ai; p; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, p->ai_addr, p->ai_addrlen);
    if (rc < 0 && errno != EINPROGRESS) {
      close(fd);
      fd = -1;
      continue;
    }
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(fd, &wf);
    timeval tv{3, 0};
    rc = select(fd + 1, nullptr, &wf, nullptr, &tv);
    if (rc <= 0) {
      close(fd);
      fd = -1;
      continue;
    }
    int err = 0;
    socklen_t elen = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
    if (err != 0) {
      close(fd);
      fd = -1;
      continue;
    }
    if (flags >= 0) fcntl(fd, F_SETFL, flags);
    break;
  }
  freeaddrinfo(ai);
  if (fd < 0) return false;

  timeval tv{recvTimeoutS, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  std::string req =
      method + " " + path + " HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n";
  if (!token.empty()) req += "Authorization: Bearer " + token + "\r\n";
  if (!body.empty() || method == "POST")
    req += "Content-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n";
  req += "\r\n" + body;

  size_t sent = 0;
  while (sent < req.size()) {
    ssize_t w = send(fd, req.data() + sent, req.size() - sent, 0);
    if (w <= 0) {
      close(fd);
      return false;
    }
    sent += (size_t)w;
  }
  std::string resp;
  char buf[4096];
  ssize_t n;
  while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) resp.append(buf, (size_t)n);
  close(fd);
  if (resp.empty()) return false;
  int code = 0;
  if (sscanf(resp.c_str(), "HTTP/%*d.%*d %d", &code) != 1) return false;
  status = code;
  size_t h = resp.find("\r\n\r\n");
  respBody = (h == std::string::npos) ? "" : resp.substr(h + 4);
  return true;
}

// -----------------------------------------------------------------------------
// Tiny JSON readers (relay responses are small and ASCII)
// -----------------------------------------------------------------------------
static bool jsonUnescapeInto(const char* p, const char* end, std::string& out) {
  out.clear();
  while (p < end) {
    char c = *p++;
    if (c != '\\') {
      out += c;
      continue;
    }
    if (p >= end) return false;
    char e = *p++;
    switch (e) {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'u': {
        if (end - p < 4) return false;
        unsigned v = 0;
        for (int i = 0; i < 4; i++) {
          char h = *p++;
          v <<= 4;
          if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
          else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
          else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
          else return false;
        }
        if (v < 0x80) {
          out += (char)v;
        } else if (v < 0x800) {
          out += (char)(0xC0 | (v >> 6));
          out += (char)(0x80 | (v & 0x3F));
        } else {
          out += (char)(0xE0 | (v >> 12));
          out += (char)(0x80 | ((v >> 6) & 0x3F));
          out += (char)(0x80 | (v & 0x3F));
        }
        break;
      }
      default: return false;
    }
  }
  return true;
}

// Find "key" : "string" (first occurrence at depth >= 0). Returns false if missing.
static bool jsonStringVal(const std::string& body, const char* key, std::string& out) {
  std::string pat = std::string("\"") + key + "\"";
  size_t k = body.find(pat);
  if (k == std::string::npos) return false;
  size_t c = body.find(':', k + pat.size());
  if (c == std::string::npos) return false;
  size_t q1 = body.find('"', c + 1);
  if (q1 == std::string::npos) return false;
  // Walk to the closing unescaped quote.
  size_t i = q1 + 1;
  while (i < body.size()) {
    if (body[i] == '\\') {
      i += 2;
      continue;
    }
    if (body[i] == '"') break;
    i++;
  }
  if (i >= body.size()) return false;
  return jsonUnescapeInto(body.data() + q1 + 1, body.data() + i, out);
}

static bool jsonIntVal(const std::string& body, const char* key, long long& out) {
  std::string pat = std::string("\"") + key + "\"";
  size_t k = body.find(pat);
  if (k == std::string::npos) return false;
  size_t c = body.find(':', k + pat.size());
  if (c == std::string::npos) return false;
  size_t i = c + 1;
  while (i < body.size() && isspace((unsigned char)body[i])) i++;
  bool neg = false;
  if (i < body.size() && (body[i] == '-' || body[i] == '+')) {
    neg = body[i] == '-';
    i++;
  }
  if (i >= body.size() || !isdigit((unsigned char)body[i])) return false;
  long long v = 0;
  while (i < body.size() && isdigit((unsigned char)body[i])) {
    v = v * 10 + (body[i] - '0');
    i++;
  }
  out = neg ? -v : v;
  return true;
}

static bool jsonBoolVal(const std::string& body, const char* key, bool& out) {
  std::string pat = std::string("\"") + key + "\"";
  size_t k = body.find(pat);
  if (k == std::string::npos) return false;
  size_t c = body.find(':', k + pat.size());
  if (c == std::string::npos) return false;
  size_t i = c + 1;
  while (i < body.size() && isspace((unsigned char)body[i])) i++;
  if (body.compare(i, 4, "true") == 0) {
    out = true;
    return true;
  }
  if (body.compare(i, 5, "false") == 0) {
    out = false;
    return true;
  }
  return false;
}

// Extract the messages[] array: every {"seq":N,...,"text":"..."} object.
static std::vector<InboundItem> parseRelayMessages(const std::string& body) {
  std::vector<InboundItem> items;
  size_t arr = body.find("\"messages\"");
  if (arr == std::string::npos) return items;
  size_t b = body.find('[', arr);
  if (b == std::string::npos) return items;
  size_t i = b + 1;
  while (i < body.size()) {
    size_t o = body.find('{', i);
    size_t close = body.find(']', b);
    if (o == std::string::npos || (close != std::string::npos && o > close)) break;
    // Find matching close brace (no nesting inside message objects).
    size_t e = body.find('}', o);
    if (e == std::string::npos) break;
    std::string obj = body.substr(o, e - o + 1);
    long long seq = 0;
    // A message object without seq is not ours (e.g. an error body).
    size_t ks = obj.find("\"seq\"");
    if (ks != std::string::npos) {
      size_t c = obj.find(':', ks);
      if (c != std::string::npos) {
        size_t k = c + 1;
        while (k < obj.size() && isspace((unsigned char)obj[k])) k++;
        while (k < obj.size() && isdigit((unsigned char)obj[k])) {
          seq = seq * 10 + (obj[k] - '0');
          k++;
        }
      }
      if (seq > 0) {
        InboundItem it;
        it.seq = seq;
        std::string from, text;
        if (jsonStringVal(obj, "from", from)) it.from = from;
        if (jsonStringVal(obj, "text", text)) {
          it.text = text;
          items.push_back(it);
        }
      }
    }
    i = e + 1;
  }
  return items;
}

static std::string jsonEscapeStr(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c >= 32 && c < 127) {
      out += c;
    } else {
      char tmp[8];
      snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned char)c);
      out += tmp;
    }
  }
  return out;
}

// -----------------------------------------------------------------------------
// Relay poll thread: one cycle polls dad then emma (TX flush + RX poll + ack)
// -----------------------------------------------------------------------------
static void setLinkLocked(DeviceSim& d, uint8_t link, const std::string& err) {
  d.link = link;
  d.relayError = err;
}

// -----------------------------------------------------------------------------
// Relay poll threads: one per device (TX flush + RX poll slices + ack)
// -----------------------------------------------------------------------------
// One thread per device. The old single thread polled dad then emma in
// sequence, so with a 20 s wait each device only polled every ~40 s —
// inside the 45 s presence window, so peer_online flapped in the sim.
// Separate threads keep each device on its own ~20 s cycle.

// Flush one queued outbound message (Morse SEND or web send button).
// Returns false when the send failed (link already updated, caller
// returns); true when nothing was queued or the POST went out (200/201).
static bool tryFlushSend(DeviceSim& d, const std::string& host, int port) {
  std::string token;
  std::string toSend;
  uint64_t counter = 0;
  {
    std::lock_guard<std::mutex> lk(d.mtx);
    token = d.token;
    if (!d.pendingSend.empty()) {
      toSend = d.pendingSend;
      counter = d.sendCounter;
      d.pendingSend.clear();
    }
  }
  if (toSend.empty()) return true;

  char cid[64];
  // Per-process boot id, like the firmware's <device>-<bootId>-<counter>:
  // without it a sim restart reuses dad-sim-1..N and the relay answers
  // 200 duplicate for the previous run's messages instead of storing the
  // new text.
  static const uint32_t bootId = [] {
    uint32_t b = (uint32_t)std::chrono::steady_clock::now()
                     .time_since_epoch()
                     .count();
    return b != 0 ? b : 1u;
  }();
  snprintf(cid, sizeof(cid), "%s-sim-%04x-%llu", d.id.c_str(),
           (unsigned)bootId, (unsigned long long)counter);
  std::string payload = std::string("{\"client_msg_id\":\"") + cid +
                        "\",\"to\":\"family\",\"text\":\"" + jsonEscapeStr(toSend) + "\"}";
  int st = 0;
  std::string resp;
  if (!httpDo(host, port, "POST", "/api/v1/messages", token, payload, st, resp)) {
    std::lock_guard<std::mutex> lk(d.mtx);
    // Requeue once so a momentary drop does not eat the message.
    if (d.pendingSend.empty()) {
      d.pendingSend = toSend;
    }
    setLinkLocked(d, LINK_WIFI, "relay unreachable (send)");
    return false;
  }
  if (st == 401) {
    std::lock_guard<std::mutex> lk(d.mtx);
    setLinkLocked(d, LINK_WIFI, "auth rejected (401): token mismatch with relay .env?");
    return false;
  }
  if (st != 200 && st != 201) {
    std::lock_guard<std::mutex> lk(d.mtx);
    setLinkLocked(d, LINK_WIFI, "send rejected: HTTP " + std::to_string(st));
    return false;
  }
  // Do NOT advance lastSeq from the POST response. That was the firmware
  // 0.2.0 TX bug (fixed there by MORSE_PROTO_GEN=3): a send assigned seq N
  // would make the next poll since=N skip a peer message with seq < N that
  // had not been fetched yet. The RX poll below advances past our own
  // sends via latest_seq instead.
  return true;
}

// One long-poll slice (waitS seconds) + ack. Returns false on transport /
// auth / HTTP failure (link already updated, caller returns).
static bool pollSlice(DeviceSim& d, const std::string& host, int port, int waitS) {
  std::string token;
  long long since = 0;
  {
    std::lock_guard<std::mutex> lk(d.mtx);
    token = d.token;
    since = d.lastSeq;
  }

  std::string path = "/api/v1/messages?since=" + std::to_string(since) +
                     "&wait=" + std::to_string(waitS) + "&limit=20";
  int st = 0;
  std::string resp;
  // The recv timeout must clear the wait (plus margin) or every poll dies
  // as unreachable.
  if (!httpDo(host, port, "GET", path, token, "", st, resp, waitS + 10)) {
    std::lock_guard<std::mutex> lk(d.mtx);
    // Keep a previously-good green until proven otherwise? No: the dot must
    // reflect reality, so a failed poll means orange (wifi ok, relay down).
    setLinkLocked(d, LINK_WIFI, "relay unreachable: " + g_serverUrl);
    return false;
  }
  if (st == 401) {
    std::lock_guard<std::mutex> lk(d.mtx);
    setLinkLocked(d, LINK_WIFI, "auth rejected (401): token mismatch with relay .env?");
    return false;
  }
  if (st != 200) {
    std::lock_guard<std::mutex> lk(d.mtx);
    setLinkLocked(d, LINK_WIFI, "poll HTTP " + std::to_string(st));
    return false;
  }

  long long latest = since;
  jsonIntVal(resp, "latest_seq", latest);
  std::vector<InboundItem> msgs = parseRelayMessages(resp);
  {
    std::lock_guard<std::mutex> lk(d.mtx);
    for (const auto& m : msgs) {
      if (m.seq > d.lastSeq && !m.text.empty()) {
        InboundItem it = m;
        if (it.text.size() > MSG_TEXT_MAX) it.text.resize(MSG_TEXT_MAX);
        d.inbound.push_back(it);
      }
    }
    if (latest > d.lastSeq) d.lastSeq = latest;
    // Backstop: if the server returned messages newer than latest_seq for
    // any reason, still advance past them so they are not requeued.
    for (const auto& m : msgs) {
      if (m.seq > d.lastSeq) d.lastSeq = m.seq;
    }
    setLinkLocked(d, LINK_OK, "");
  }

  // Ack what we have so the relay's unread count tracks the sim.
  long long upto = 0;
  {
    std::lock_guard<std::mutex> lk(d.mtx);
    upto = d.lastSeq;
    if (upto <= d.ackedSeq) return true;
  }
  int ast = 0;
  std::string aresp;
  std::string apayload = "{\"upto_seq\":" + std::to_string(upto) + "}";
  if (httpDo(host, port, "POST", "/api/v1/ack", token, apayload, ast, aresp) && ast == 200) {
    std::lock_guard<std::mutex> lk(d.mtx);
    if (upto > d.ackedSeq) d.ackedSeq = upto;
  }
  return true;
}

// One relay cycle for one device: flush queued sends, then poll in 2 s
// slices up to a 20 s cycle budget. The slices are the sim's answer to the
// firmware's single-task SEND delay (see config.h): a send queued
// mid-poll is flushed before the next slice, so it goes out within ~2 s
// instead of waiting out a full 20 s long-poll. The local dev server has
// no request budget, so the extra requests cost nothing; the wire
// protocol is unchanged (the DO still sees wait<=25 polls).
static void relayCycleDevice(DeviceSim& d, const std::string& serverUrl, bool enabled) {
  bool wifiOn = true;
  std::string token;
  {
    std::lock_guard<std::mutex> lk(d.mtx);
    token = d.token;
    wifiOn = d.wifiOn;
  }

  if (!wifiOn) {
    std::lock_guard<std::mutex> lk(d.mtx);
    setLinkLocked(d, LINK_OFF, "wifi off (sim toggle)");
    return;
  }
  if (!enabled) {
    std::lock_guard<std::mutex> lk(d.mtx);
    setLinkLocked(d, LINK_WIFI, "relay disabled in sim");
    return;
  }
  std::string host;
  int port = 80;
  std::string urlError;
  if (!parseServerUrl(serverUrl, host, port, urlError)) {
    std::lock_guard<std::mutex> lk(d.mtx);
    setLinkLocked(d, LINK_WIFI, urlError);
    return;
  }
  if (token.empty()) {
    std::lock_guard<std::mutex> lk(d.mtx);
    setLinkLocked(d, LINK_WIFI, "no token: set MORSE_TOKEN_" + d.id + " or use the relay bar");
    return;
  }

  const auto cycleStart = std::chrono::steady_clock::now();
  for (;;) {
    if (!tryFlushSend(d, host, port)) return;
    if (!pollSlice(d, host, port, 2)) return;
    if (!g_running) return;
    // Loop: a send queued during the slice is flushed before the next one.
    if (std::chrono::steady_clock::now() - cycleStart >= std::chrono::seconds(20)) break;
  }
}

static void relayDeviceThread(DeviceSim& d, int startDelayMs) {
  // Stagger so dad and emma do not poll in lockstep.
  std::this_thread::sleep_for(std::chrono::milliseconds(startDelayMs));
  while (g_running) {
    const auto t0 = std::chrono::steady_clock::now();
    relayCycleDevice(d, g_serverUrl, g_relayEnabled.load());
    if (!g_running) break;
    // A failed cycle returns at once; back off instead of spinning.
    if (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
      std::this_thread::sleep_for(std::chrono::seconds(2));
  }
}

// -----------------------------------------------------------------------------
// Virtual device state machine (mirrors firmware/src/main.cpp)
// -----------------------------------------------------------------------------
static uint32_t vnow();

static void appendChar(DeviceSim& d, char c) {
  if (d.st.len >= TEXT_MAX) {
    memmove(d.st.text, d.st.text + 1, TEXT_MAX - 1);
    d.st.len = TEXT_MAX - 1;
  }
  d.st.text[d.st.len++] = c;
  d.st.text[d.st.len] = '\0';
}

static void resetPath(DeviceSim& d) {
  d.st.node = MORSE_ROOT_INDEX;
  d.st.depth = 0;
  d.st.bits = 0;
}

static char commitLetter(DeviceSim& d, uint32_t now) {
  if (d.st.depth == 0) return 0;
  char c = MORSE_NODES[d.st.node].letter;
  if (c != '\0') {
    appendChar(d, c);
    d.st.lastCharAt = now;
    d.st.charFlashActive = true;
  }
  d.st.burstNode = d.st.node;
  d.st.burstUntil = now + BURST_MS;
  d.st.burstActive = true;
  d.st.burstError = false;
  resetPath(d);
  d.st.lastCommitAt = now;
  return c;
}

static void addSymbol(DeviceSim& d, bool dash, uint32_t now) {
  int8_t child = morseChild(d.st.node, dash);
  if (child < 0) {
    commitLetter(d, now);
    child = morseChild(MORSE_ROOT_INDEX, dash);
  }
  if (child < 0) return;
  d.st.node = (uint8_t)child;
  if (dash)
    d.st.bits |= (uint8_t)(1u << d.st.depth);
  else
    d.st.bits &= (uint8_t)~(1u << d.st.depth);
  d.st.depth++;
  d.st.lastSymbolAt = now;
  d.st.burstNode = d.st.node;
  d.st.burstUntil = now + BURST_MS;
  d.st.burstActive = true;
  d.st.burstError = false;
}

static void redrawCompose(DeviceSim& d, uint32_t now) {
  d.appMode = MODE_COMPOSE;
  d.msgMenuOpen = false;
  selectDevice(d, deviceLink(d));
  uiFull(d.st, now);
  uiBadge(d.inboxCount, now);
}

static void rebuildActions(DeviceSim& d) {
  d.actionCount = 0;
  if (d.st.len > 0) {
    snprintf(d.actionBuf[d.actionCount], sizeof(d.actionBuf[0]), "SEND");
    d.actionPtrs[d.actionCount] = d.actionBuf[d.actionCount];
    d.actionCount++;
    snprintf(d.actionBuf[d.actionCount], sizeof(d.actionBuf[0]), "CLEAR");
    d.actionPtrs[d.actionCount] = d.actionBuf[d.actionCount];
    d.actionCount++;
  }
  if (d.inboxCount > 0) {
    snprintf(d.actionBuf[d.actionCount], sizeof(d.actionBuf[0]), "READ %u",
             (unsigned)d.inboxCount);
    d.actionPtrs[d.actionCount] = d.actionBuf[d.actionCount];
    d.actionCount++;
  }
  snprintf(d.actionBuf[d.actionCount], sizeof(d.actionBuf[0]), "CLOSE");
  d.actionPtrs[d.actionCount] = d.actionBuf[d.actionCount];
  d.actionCount++;
  if (d.actionSel >= d.actionCount) d.actionSel = 0;
}

static void enterActions(DeviceSim& d, uint32_t now) {
  d.appMode = MODE_ACTIONS;
  d.actionSel = 0;
  rebuildActions(d);
  selectDevice(d, deviceLink(d));
  uiFull(d.st, now);
  uiBadge(d.inboxCount, now);
  uiActionSheet(d.actionPtrs, d.actionCount, d.actionSel, now);
}

static void enterMessage(DeviceSim& d, uint32_t now, uint8_t idx) {
  if (d.inboxCount == 0) {
    redrawCompose(d, now);
    return;
  }
  if (idx >= d.inboxCount) idx = 0;
  d.appMode = MODE_MESSAGE;
  d.msgIdx = idx;
  d.msgMenuOpen = false;
  d.msgMenuSel = 0;
  selectDevice(d, deviceLink(d));
  uiMessageCard(d.inbox[d.msgIdx].id, d.inbox[d.msgIdx].text, (uint8_t)(d.msgIdx + 1),
                d.inboxCount, false, 0, now);
}

static void redrawMessage(DeviceSim& d, uint32_t now) {
  if (d.inboxCount == 0) {
    redrawCompose(d, now);
    return;
  }
  if (d.msgIdx >= d.inboxCount) d.msgIdx = 0;
  selectDevice(d, deviceLink(d));
  uiMessageCard(d.inbox[d.msgIdx].id, d.inbox[d.msgIdx].text, (uint8_t)(d.msgIdx + 1),
                d.inboxCount, d.msgMenuOpen, d.msgMenuSel, now);
}

static void inboxRemoveAt(DeviceSim& d, uint8_t idx) {
  if (idx >= d.inboxCount) return;
  for (uint8_t i = idx; i + 1 < d.inboxCount; i++) d.inbox[i] = d.inbox[i + 1];
  d.inboxCount--;
}

// Newest-first insert; drops the oldest when full.
static void inboxPush(DeviceSim& d, const char* text, uint32_t now) {
  if (!text || !*text) return;
  InboxMsg m;
  m.id = d.nextMsgId++;
  size_t n = strlen(text);
  if (n > MSG_TEXT_MAX) n = MSG_TEXT_MAX;
  memcpy(m.text, text, n);
  m.text[n] = '\0';
  if (d.inboxCount >= INBOX_MAX) d.inboxCount = INBOX_MAX - 1;
  for (uint8_t i = d.inboxCount; i > 0; i--) d.inbox[i] = d.inbox[i - 1];
  d.inbox[0] = m;
  d.inboxCount++;
  selectDevice(d, deviceLink(d));
  if (d.appMode == MODE_COMPOSE) {
    uiBadge(d.inboxCount, now);
  } else if (d.appMode == MODE_ACTIONS) {
    rebuildActions(d);
    uiFull(d.st, now);
    uiBadge(d.inboxCount, now);
    uiActionSheet(d.actionPtrs, d.actionCount, d.actionSel, now);
  } else {
    redrawMessage(d, now);
  }
}

static void messageNext(DeviceSim& d, uint32_t now) {
  inboxRemoveAt(d, d.msgIdx);
  if (d.inboxCount == 0) {
    redrawCompose(d, now);
    return;
  }
  if (d.msgIdx >= d.inboxCount) d.msgIdx = 0;
  d.msgMenuOpen = false;
  d.msgMenuSel = 0;
  redrawMessage(d, now);
}

static void messageClose(DeviceSim& d, uint32_t now) {
  inboxRemoveAt(d, d.msgIdx);
  redrawCompose(d, now);
}

// Queue outbound text for the relay poll thread (real POST). The compose
// buffer clears immediately, exactly like the firmware's outbox handoff.
static void queueSend(DeviceSim& d, const char* text) {
  if (!text || !*text) return;
  std::string t(text);
  if (t.size() > MSG_TEXT_MAX) t.resize(MSG_TEXT_MAX);
  std::lock_guard<std::mutex> lk(d.mtx);
  d.pendingSend = t;
  d.sendCounter++;
}

static void activateAction(DeviceSim& d, uint32_t now) {
  const char* sel = d.actionSel < d.actionCount ? d.actionPtrs[d.actionSel] : "CLOSE";
  if (!strncmp(sel, "SEND", 4)) {
    queueSend(d, d.st.text);
    d.st.len = 0;
    d.st.text[0] = '\0';
    resetPath(d);
    d.st.lastCommitAt = now;
    d.st.lastSymbolAt = now;
    redrawCompose(d, now);
  } else if (!strncmp(sel, "CLEAR", 5)) {
    d.st.len = 0;
    d.st.text[0] = '\0';
    resetPath(d);
    d.st.lastCommitAt = now;
    d.st.lastSymbolAt = now;
    redrawCompose(d, now);
  } else if (!strncmp(sel, "READ", 4)) {
    enterMessage(d, now, 0);
  } else {
    redrawCompose(d, now);
  }
}

static void clearTextSim(DeviceSim& d, uint32_t now) {
  d.st.len = 0;
  d.st.text[0] = '\0';
  resetPath(d);
  d.st.lastCommitAt = now;
  d.st.lastSymbolAt = now;
  d.st.charFlashActive = false;
  d.st.burstNode = MORSE_ROOT_INDEX;
  d.st.burstUntil = now + BURST_MS;
  d.st.burstActive = true;
  d.st.burstError = true;
  redrawCompose(d, now);
}

static void onPressDown(DeviceSim& d, uint32_t now) {
  (void)d;
  (void)now;
  // Intentionally empty: the gap timer (maybeCommit) is frozen while the
  // button is physically held, so a press whose raw edge landed before
  // LETTER_GAP_MS keeps the pending letter alive for the whole hold.
}

static void onRelease(DeviceSim& d, uint32_t now, uint32_t durationMs) {
  if (d.longFired) {
    d.longFired = false;
    return;
  }
  if (d.appMode == MODE_ACTIONS) {
    if (durationMs >= ACTION_HOLD_MS) {
      activateAction(d, now);
    } else if (d.actionCount > 0) {
      d.actionSel = (uint8_t)((d.actionSel + 1) % d.actionCount);
      selectDevice(d, deviceLink(d));
      uiActionSheet(d.actionPtrs, d.actionCount, d.actionSel, now);
    }
    return;
  }
  if (d.appMode == MODE_MESSAGE) {
    if (!d.msgMenuOpen) {
      if (durationMs >= ACTION_HOLD_MS) {
        d.msgMenuOpen = true;
        d.msgMenuSel = 0;
        redrawMessage(d, now);
      } else {
        messageNext(d, now);
      }
    } else {
      if (durationMs >= ACTION_HOLD_MS) {
        if (d.msgMenuSel == 0)
          messageNext(d, now);
        else
          messageClose(d, now);
      } else {
        d.msgMenuSel = (uint8_t)((d.msgMenuSel + 1) % 2);
        redrawMessage(d, now);
      }
    }
    return;
  }
  if (durationMs >= CLEAR_HOLD_MS) {
    if (d.st.depth > 0) commitLetter(d, now);
    enterActions(d, now);
    return;
  }
  bool dash = durationMs >= DOT_MAX_MS;
  // No gap check here: if the letter were done, maybeCommit would have fixed
  // it during idle (holds freeze that timer). A still-pending letter extends.
  if (d.st.depth > 0 && morseChild(d.st.node, dash) < 0) {
    commitLetter(d, now);
  }
  addSymbol(d, dash, now);
  selectDevice(d, deviceLink(d));
  uiFull(d.st, now);
  uiBadge(d.inboxCount, now);
}

static void maybeCommit(DeviceSim& d, uint32_t now, bool downRaw) {
  if (d.appMode != MODE_COMPOSE) return;
  if (d.st.depth == 0) return;
  // Press cycle in flight freezes the timer: raw level covers the hold,
  // inPressed covers the debounce tail after release (mirrors firmware).
  if (downRaw || d.inPressed) return;
  if (now - d.st.lastSymbolAt < LETTER_GAP_MS) return;
  commitLetter(d, now);
  selectDevice(d, deviceLink(d));
  uiFull(d.st, now);
  uiBadge(d.inboxCount, now);
}

static void maybeWordSpace(DeviceSim& d, uint32_t now, bool downRaw) {
  if (d.appMode != MODE_COMPOSE) return;
  if (d.st.depth != 0) return;
  if (d.st.len == 0) return;
  if (d.st.text[d.st.len - 1] == ' ') return;
  if (downRaw || d.inPressed) return;
  if (now - d.st.lastCommitAt < WORD_GAP_MS) return;
  appendChar(d, ' ');
  d.st.lastCommitAt = now;
  selectDevice(d, deviceLink(d));
  uiResult(d.st, now);
}

static void pollInput(DeviceSim& d, uint32_t now) {
  bool raw = d.virtDown;
  if (raw != d.inRaw) {
    d.inRaw = raw;
    d.inRawChanged = now;
  }
  if (now - d.inRawChanged >= DEBOUNCE_MS && raw != d.inPressed) {
    d.inPressed = raw;
    if (d.inPressed) {
      d.inPressedAt = now;
      d.previewState = PREVIEW_DOT;
      d.longFired = false;
      onPressDown(d, now);
      if (d.appMode == MODE_COMPOSE) {
        selectDevice(d, deviceLink(d));
        uiPattern(d.st, d.previewState, now);
        d.lastPreviewDraw = now;
      }
    } else {
      uint32_t duration = now - d.inPressedAt;
      d.previewState = PREVIEW_NONE;
      onRelease(d, now, duration);
    }
  }
  if (d.inPressed && !d.longFired) {
    uint32_t held = now - d.inPressedAt;
    if (d.appMode == MODE_COMPOSE && held >= CLEAR_HOLD_MS) {
      d.longFired = true;
      if (d.st.depth > 0) commitLetter(d, now);
      enterActions(d, now);
    } else if (d.appMode == MODE_ACTIONS && held >= ACTION_HOLD_MS) {
      d.longFired = true;
      activateAction(d, now);
    } else if (d.appMode == MODE_MESSAGE && held >= ACTION_HOLD_MS) {
      d.longFired = true;
      if (!d.msgMenuOpen) {
        d.msgMenuOpen = true;
        d.msgMenuSel = 0;
        redrawMessage(d, now);
      } else {
        if (d.msgMenuSel == 0) messageNext(d, now);
        else messageClose(d, now);
      }
    }
  }
  if (d.inPressed && !d.longFired && d.appMode == MODE_COMPOSE) {
    uint32_t held = now - d.inPressedAt;
    uint8_t p;
    if (held >= CLEAR_HOLD_MS)
      p = PREVIEW_ACTIONS;
    else if (held >= DOT_MAX_MS)
      p = PREVIEW_DASH;
    else
      p = PREVIEW_DOT;
    if (p != d.previewState) {
      d.previewState = p;
      selectDevice(d, deviceLink(d));
      uiPattern(d.st, d.previewState, now);
      d.lastPreviewDraw = now;
    } else if (now - d.lastPreviewDraw >= PREVIEW_PULSE_MS) {
      selectDevice(d, deviceLink(d));
      uiPattern(d.st, d.previewState, now);
      d.lastPreviewDraw = now;
    }
  }
}

// Drain relay inbox into the on-screen inbox (main thread: owns all ui calls).
static void drainInbound(DeviceSim& d, uint32_t now) {
  std::vector<InboundItem> items;
  uint8_t link;
  {
    std::lock_guard<std::mutex> lk(d.mtx);
    items.swap(d.inbound);
    link = d.wifiOn ? d.link : (uint8_t)LINK_OFF;
  }
  bool linkChanged = (link != d.linkCache);
  d.linkCache = link;
  if (!items.empty()) {
    for (const auto& it : items) inboxPush(d, it.text.c_str(), now);
  } else if (linkChanged) {
    // Repaint just the top strip so the dot tracks the relay live.
    selectDevice(d, link);
    if (d.appMode == MODE_COMPOSE) {
      uiBadge(d.inboxCount, now);
    } else if (d.appMode == MODE_MESSAGE) {
      redrawMessage(d, now);
    } else {
      rebuildActions(d);
      uiFull(d.st, now);
      uiBadge(d.inboxCount, now);
      uiActionSheet(d.actionPtrs, d.actionCount, d.actionSel, now);
    }
  }
}

static void tickDevice(DeviceSim& d, uint32_t now) {
  drainInbound(d, now);
  maybeCommit(d, now, d.virtDown);
  maybeWordSpace(d, now, d.virtDown);
  pollInput(d, now);
  now = vnow();
  if (d.appMode == MODE_COMPOSE && d.st.burstActive &&
      (int32_t)(now - d.st.burstUntil) >= 0) {
    d.st.burstActive = false;
    d.st.burstError = false;
    selectDevice(d, deviceLink(d));
    uiFull(d.st, now);
    uiBadge(d.inboxCount, now);
  }
  if (d.appMode == MODE_COMPOSE && d.st.charFlashActive &&
      (int32_t)(now - (d.st.lastCharAt + RESULT_FLASH_MS)) >= 0) {
    d.st.charFlashActive = false;
    selectDevice(d, deviceLink(d));
    uiResult(d.st, now);
  }
  if (d.appMode == MODE_COMPOSE && (d.st.depth > 0 || d.inPressed)) {
    if (now - d.lastProgressDraw >= PROGRESS_REDRAW_MS) {
      d.lastProgressDraw = now;
      selectDevice(d, deviceLink(d));
      uiProgress(d.st, d.inPressed, now);
    }
  } else if (d.lastProgressDraw != 0) {
    selectDevice(d, deviceLink(d));
    uiProgress(d.st, false, now);
    d.lastProgressDraw = 0;
  }
}

static void resetSim(DeviceSim& d, uint32_t now) {
  memset(&d.st, 0, sizeof(d.st));
  d.st.node = MORSE_ROOT_INDEX;
  d.st.text[0] = '\0';
  d.st.lastCommitAt = now;
  d.st.lastSymbolAt = now;
  d.inRaw = d.virtDown;
  d.inRawChanged = now;
  d.inPressed = false;
  d.previewState = PREVIEW_NONE;
  d.lastPreviewDraw = 0;
  d.lastProgressDraw = 0;
  d.longFired = false;
  d.appMode = MODE_COMPOSE;
  d.inboxCount = 0;
  d.nextMsgId = 1;
  d.actionCount = 0;
  d.actionSel = 0;
  d.msgIdx = 0;
  d.msgMenuOpen = false;
  d.msgMenuSel = 0;
  redrawCompose(d, now);
}

// Wall-clock boot time; virtual millis() like Arduino.
static std::chrono::steady_clock::time_point bootTp;
static uint32_t vnow() {
  using namespace std::chrono;
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - bootTp)
      .count();
}

// -----------------------------------------------------------------------------
// Scenes (same set as tools/preview/main.cpp)
// -----------------------------------------------------------------------------
static void setTextState(MorseState& s, const char* t) {
  size_t n = strlen(t);
  if (n > TEXT_MAX) n = TEXT_MAX;
  memcpy(s.text, t, n);
  s.text[n] = '\0';
  s.len = (uint8_t)n;
}

static void applyScene(DeviceSim& d, const std::string& name) {
  uint32_t now = vnow();
  resetSim(d, now);
  d.virtDown = false;
  // Demo every link color across the dump: idle is offline (red), the armed
  // hold is wifi-without-relay (orange), everything else is online (green).
  uint8_t link = LINK_OK;
  if (name == "root") link = LINK_OFF;
  if (name == "hold-menu") link = LINK_WIFI;
  selectDevice(d, link);
  if (name == "root") {
    // idle
  } else if (name == "e-selected") {
    d.st.node = 1;
    d.st.depth = 1;
    d.st.bits = 0;
    d.st.lastSymbolAt = now - 120;
    d.st.burstNode = 1;
    d.st.burstUntil = now + BURST_MS / 2;
    d.st.burstActive = true;
  } else if (name == "hello") {
    setTextState(d.st, "HELLO");
    d.st.lastCharAt = now - 120;
    d.st.charFlashActive = true;
    d.st.lastCommitAt = now - 4000;
  } else if (name == "sos-help") {
    setTextState(d.st, "SOS HELP");
    d.st.lastCommitAt = now - 3000;
  } else if (name == "mid-letter-s") {
    d.st.node = 7;
    d.st.depth = 3;
    d.st.bits = 0;
    d.st.lastSymbolAt = now - 300;
  } else if (name == "wrapped") {
    setTextState(d.st, "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG");
    d.st.lastCommitAt = now - 5000;
  } else if (name == "hold-dash") {
    d.st.node = 5;
    d.st.depth = 1;
    d.st.bits = 1;
    d.st.lastSymbolAt = now - 80;
    d.st.burstNode = 5;
    d.st.burstUntil = now + BURST_MS / 2;
    d.st.burstActive = true;
    // show held-dash preview + pressed countdown
    selectDevice(d, link);
    uiFull(d.st, now);
    uiPattern(d.st, PREVIEW_DASH, now);
    uiProgress(d.st, true, now);
    return;
  } else if (name == "hold-menu") {
    setTextState(d.st, "SOS HELP");
    selectDevice(d, link);
    uiFull(d.st, now);
    uiBadge(2, now);
    uiPattern(d.st, PREVIEW_ACTIONS, now);
    return;
  } else if (name == "menu") {
    setTextState(d.st, "HELLO");
    selectDevice(d, link);
    uiFull(d.st, now);
    uiBadge(2, now);
    inboxPush(d, "ARE YOU OK", now);
    inboxPush(d, "MEET AT SIX", now);
    // inboxPush paints badges; repaint compose then open the sheet
    selectDevice(d, link);
    uiFull(d.st, now);
    uiBadge(d.inboxCount, now);
    d.appMode = MODE_ACTIONS;
    d.actionSel = 0;
    rebuildActions(d);
    uiActionSheet(d.actionPtrs, d.actionCount, d.actionSel, now);
    return;
  } else if (name == "badge") {
    setTextState(d.st, "HELLO");
    selectDevice(d, link);
    uiFull(d.st, now);
    uiBadge(3, now);
    return;
  } else if (name == "message") {
    inboxPush(d, "HELLO ARE YOU OK MEET ME AT THE PARK AT SIX", now);
    selectDevice(d, link);
    uiMessageCard(d.inbox[0].id, d.inbox[0].text, 1, d.inboxCount, false, 0, now);
    return;
  } else if (name == "message-menu") {
    inboxPush(d, "FIRST MESSAGE", now);
    inboxPush(d, "SECOND MESSAGE", now);
    selectDevice(d, link);
    d.appMode = MODE_MESSAGE;
    d.msgIdx = 0;
    d.msgMenuOpen = true;
    d.msgMenuSel = 0;
    uiMessageCard(d.inbox[d.msgIdx].id, d.inbox[d.msgIdx].text,
                  (uint8_t)(d.msgIdx + 1), d.inboxCount, true, 0, now);
    return;
  }
  selectDevice(d, link);
  uiFull(d.st, now);
  uiBadge(d.inboxCount, now);
}

// -----------------------------------------------------------------------------
// Framebuffer encoding
// -----------------------------------------------------------------------------
static void framebufferRGB888(DeviceSim& d, std::vector<uint8_t>& out) {
  const auto& fb = d.display.framebuffer();
  out.resize((size_t)SCREEN_W * SCREEN_H * 3);
  for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) {
    uint16_t c = fb[i];
    out[3 * i + 0] = (uint8_t)(((c >> 11) & 0x1F) << 3 | ((c >> 11) & 0x1F) >> 2);
    out[3 * i + 1] = (uint8_t)(((c >> 5) & 0x3F) << 2 | ((c >> 5) & 0x3F) >> 4);
    out[3 * i + 2] = (uint8_t)((c & 0x1F) << 3 | (c & 0x1F) >> 2);
  }
}

static void writePPM(DeviceSim& d, const std::string& path) {
  std::vector<uint8_t> rgb;
  framebufferRGB888(d, rgb);
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
  fwrite(rgb.data(), 1, rgb.size(), f);
  fclose(f);
}

// BMP (24-bit, bottom-up) so screenshots open everywhere.
static std::vector<uint8_t> encodeBMP(DeviceSim& d) {
  std::vector<uint8_t> rgb;
  framebufferRGB888(d, rgb);
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


static const char* linkName(uint8_t link) {
  if (link == LINK_OK) return "online";
  if (link == LINK_WIFI) return "wifi-only";
  return "offline";
}

static std::string stateJSON(DeviceSim& d) {
  uint32_t now = vnow();
  std::string esc = jsonEscapeStr(d.st.text);
  char letter = MORSE_NODES[d.st.node].letter ? MORSE_NODES[d.st.node].letter : '.';
  const char* modeStr = d.appMode == MODE_COMPOSE ? "compose"
                        : d.appMode == MODE_ACTIONS ? "actions"
                                                   : "message";
  uint8_t link;
  std::string relayError;
  std::string token;
  long long lastSeq = 0;
  bool wifiOn = true;
  {
    std::lock_guard<std::mutex> lk(d.mtx);
    link = d.wifiOn ? d.link : (uint8_t)LINK_OFF;
    relayError = d.relayError;
    token = d.token;
    lastSeq = d.lastSeq;
    wifiOn = d.wifiOn;
  }
  char buf[4096];
  snprintf(buf, sizeof(buf),
           "{\"id\":\"%s\",\"now\":%u,\"node\":%u,\"nodeLetter\":\"%c\",\"depth\":%u,"
           "\"text\":\"%s\",\"pressed\":%s,\"preview\":%u,"
           "\"mode\":\"%s\",\"unread\":%u,\"nextId\":%u,"
           "\"msgIdx\":%u,\"msgMenu\":%s,\"msgMenuSel\":%u,"
           "\"actionSel\":%u,\"link\":%u,\"linkName\":\"%s\","
           "\"wifi\":%s,\"hasToken\":%s,\"lastSeq\":%lld,"
           "\"relayUrl\":\"%s\","
           "\"relayError\":\"%s\"}",
           d.id.c_str(), now, d.st.node, letter, d.st.depth, esc.c_str(),
           (d.virtDown || d.inPressed) ? "true" : "false", d.previewState, modeStr,
           (unsigned)d.inboxCount, (unsigned)d.nextMsgId, (unsigned)d.msgIdx,
           d.msgMenuOpen ? "true" : "false", (unsigned)d.msgMenuSel,
           (unsigned)d.actionSel, (unsigned)link, linkName(link),
           wifiOn ? "true" : "false", token.empty() ? "false" : "true",
           (long long)lastSeq, jsonEscapeStr(g_serverUrl.c_str()).c_str(),
           jsonEscapeStr(relayError.c_str()).c_str());
  std::string out(buf);
  // inbox: newest-first [{id,text}]
  out.pop_back(); // remove }
  out += ",\"inbox\":[";
  for (uint8_t i = 0; i < d.inboxCount; i++) {
    if (i) out += ",";
    char ib[64];
    snprintf(ib, sizeof(ib), "{\"id\":%u,\"text\":\"",
             (unsigned)d.inbox[i].id);
    out += ib;
    out += jsonEscapeStr(d.inbox[i].text);
    out += "\"}";
  }
  out += "],\"actions\":[";
  for (uint8_t i = 0; i < d.actionCount; i++) {
    if (i) out += ",";
    out += "\"";
    out += jsonEscapeStr(d.actionPtrs[i] ? d.actionPtrs[i] : "");
    out += "\"";
  }
  out += "]";
  if (d.appMode == MODE_MESSAGE && d.inboxCount > 0 && d.msgIdx < d.inboxCount) {
    char mb[64];
    snprintf(mb, sizeof(mb), ",\"msgId\":%u,", (unsigned)d.inbox[d.msgIdx].id);
    out += mb;
    out += "\"msgText\":\"";
    out += jsonEscapeStr(d.inbox[d.msgIdx].text);
    out += "\"";
  }
  out += "}";
  return out;
}

static std::string relayJSON() {
  auto one = [](DeviceSim& d) {
    std::lock_guard<std::mutex> lk(d.mtx);
    uint8_t link = d.wifiOn ? d.link : (uint8_t)LINK_OFF;
    char b[1024];
    snprintf(b, sizeof(b),
             "{\"id\":\"%s\",\"wifi\":%s,\"link\":%u,\"linkName\":\"%s\","
             "\"hasToken\":%s,\"lastSeq\":%lld,\"error\":\"%s\"}",
             d.id.c_str(), d.wifiOn ? "true" : "false", (unsigned)link,
             linkName(link), d.token.empty() ? "false" : "true",
             (long long)d.lastSeq, jsonEscapeStr(d.relayError.c_str()).c_str());
    return std::string(b);
  };
  char head[512];
  snprintf(head, sizeof(head),
           "{\"url\":\"%s\",\"enabled\":%s,\"dad\":",
           jsonEscapeStr(g_serverUrl.c_str()).c_str(),
           g_relayEnabled.load() ? "true" : "false");
  return std::string(head) + one(g_dad) + ",\"emma\":" + one(g_emma) + "}";
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

static void sendJSON(int fd, const std::string& payload, int code = 200) {
  const char* reason = code == 200 ? "OK" : (code == 400 ? "Bad Request" : "Not Found");
  std::string head = "HTTP/1.0 " + std::to_string(code) + " " + reason +
                     "\r\nContent-Type: application/json\r\nContent-Length: " +
                     std::to_string(payload.size()) +
                     "\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
  sendAll(fd, head.c_str(), head.size());
  sendAll(fd, payload.c_str(), payload.size());
}

static void handleDeviceClient(int fd, DeviceSim& d, const std::string& sub,
                               const std::string& method, const std::string& body) {
  uint32_t now = vnow();
  tickDevice(g_dad, now);
  tickDevice(g_emma, now);
  now = vnow();

  if (sub == "state" || sub == "messages") {
    sendJSON(fd, stateJSON(d));
  } else if (sub == "frame.raw") {
    std::vector<uint8_t> bin;
    framebufferRGB888(d, bin);
    std::string head = "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n"
                       "Content-Length: " +
                       std::to_string(bin.size()) +
                       "\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    sendAll(fd, head.c_str(), head.size());
    sendAll(fd, (const char*)bin.data(), bin.size());
  } else if (sub == "frame.bmp") {
    std::vector<uint8_t> bin = encodeBMP(d);
    std::string head = "HTTP/1.0 200 OK\r\nContent-Type: image/bmp\r\nContent-Length: " +
                       std::to_string(bin.size()) +
                       "\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    sendAll(fd, head.c_str(), head.size());
    sendAll(fd, (const char*)bin.data(), bin.size());
  } else if (sub == "press" && method == "POST") {
    if (body.find("down") != std::string::npos)
      d.virtDown = true;
    else if (body.find("up") != std::string::npos)
      d.virtDown = false;
    else if (body.find("true") != std::string::npos)
      d.virtDown = true;
    else if (body.find("false") != std::string::npos)
      d.virtDown = false;
    sendJSON(fd, stateJSON(d));
  } else if (sub == "scene" && method == "POST") {
    std::string name = "root";
    std::string v;
    if (jsonStringVal(body, "scene", v) && !v.empty()) {
      name = v;
    } else if (!body.empty()) {
      std::string t;
      for (char ch : body)
        if (ch != '"' && ch != '\'' && ch != ' ' && ch != '\n' && ch != '\r')
          t += ch;
      size_t b = t.find(':');
      if (b != std::string::npos) t = t.substr(b + 1);
      size_t a = t.find('{');
      size_t z = t.find('}');
      if (a != std::string::npos && z != std::string::npos && z > a)
        t = t.substr(a + 1, z - a - 1);
      if (!t.empty()) name = t;
    }
    applyScene(d, name);
    sendJSON(fd, stateJSON(d));
  } else if (sub == "text" && method == "POST") {
    std::string t;
    jsonStringVal(body, "text", t);
    uint32_t n2 = vnow();
    resetSim(d, n2);
    setTextState(d.st, t.c_str());
    redrawCompose(d, n2);
    sendJSON(fd, stateJSON(d));
  } else if (sub == "receive" && method == "POST") {
    // Local inject (offline-style). Empty body cycles through samples.
    std::string t;
    jsonStringVal(body, "text", t);
    if (t.empty() && !body.empty() && body.find('{') == std::string::npos) {
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
    if (t.size() > MSG_TEXT_MAX) t.resize(MSG_TEXT_MAX);
    inboxPush(d, t.c_str(), vnow());
    sendJSON(fd, stateJSON(d));
  } else if (sub == "send" && method == "POST") {
    // Queue text for delivery via the REAL relay (same path as Morse SEND).
    std::string t;
    jsonStringVal(body, "text", t);
    while (!t.empty() && isspace((unsigned char)t.front())) t.erase(t.begin());
    while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back();
    if (t.empty()) {
      sendJSON(fd, "{\"error\":\"empty text\"}", 400);
      return;
    }
    if (t.size() > MSG_TEXT_MAX) t.resize(MSG_TEXT_MAX);
    queueSend(d, t.c_str());
    sendJSON(fd, "{\"queued\":true}");
  } else if (sub == "wifi" && method == "POST") {
    bool on = true;
    bool v = true;
    if (jsonBoolVal(body, "on", v)) {
      on = v;
    } else if (body.find("off") != std::string::npos) {
      on = false;
    } else if (body.find("false") != std::string::npos) {
      on = false;
    }
    {
      std::lock_guard<std::mutex> lk(d.mtx);
      d.wifiOn = on;
      if (!on) setLinkLocked(d, LINK_OFF, "wifi off (sim toggle)");
    }
    sendJSON(fd, stateJSON(d));
  } else if (sub == "clear" && method == "POST") {
    clearTextSim(d, vnow());
    sendJSON(fd, stateJSON(d));
  } else {
    sendJSON(fd, "{\"error\":\"not found\"}", 404);
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
  size_t clPos = r.find("Content-Length:");
  if (clPos != std::string::npos && hdrEnd != std::string::npos) {
    int cl = atoi(r.c_str() + clPos + 15);
    while ((int)body.size() < cl) {
      ssize_t m = recv(fd, req, sizeof(req), 0);
      if (m <= 0) break;
      body.append(req, (size_t)m);
    }
  }

  std::string p(path);
  // Strip query string for routing.
  size_t q = p.find('?');
  if (q != std::string::npos) p = p.substr(0, q);

  if (p == "/" || p == "/index.html") {
    std::string payload = readFile(g_indexPath);
    if (payload.empty()) payload = "<h1>index.html missing</h1>";
    std::string head = "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
                       std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n";
    sendAll(fd, head.c_str(), head.size());
    sendAll(fd, payload.c_str(), payload.size());
    return;
  }

  if (p == "/api/relay" && std::string(method) == "GET") {
    tickDevice(g_dad, vnow());
    tickDevice(g_emma, vnow());
    sendJSON(fd, relayJSON());
    return;
  }

  if (p == "/api/relay" && std::string(method) == "POST") {
    std::string url, dadToken, emmaToken;
    bool enabled = true;
    bool hasEnabled = jsonBoolVal(body, "enabled", enabled);
    // One URL field (the relay bar); "serverUrl" is accepted as an alias.
    if (jsonStringVal(body, "url", url) && !url.empty()) g_serverUrl = url;
    else if (jsonStringVal(body, "serverUrl", url) && !url.empty()) g_serverUrl = url;
    if (hasEnabled) g_relayEnabled = enabled;
    if (jsonStringVal(body, "dadToken", dadToken)) {
      std::lock_guard<std::mutex> lk(g_dad.mtx);
      g_dad.token = dadToken;
    }
    if (jsonStringVal(body, "emmaToken", emmaToken)) {
      std::lock_guard<std::mutex> lk(g_emma.mtx);
      g_emma.token = emmaToken;
    }
    // A token change may unstick auth errors: reset the error text so the
    // next poll cycle reports fresh state instead of a stale 401.
    {
      std::lock_guard<std::mutex> lk(g_dad.mtx);
      std::lock_guard<std::mutex> lk2(g_emma.mtx);
      if (g_dad.relayError.find("auth") != std::string::npos)
        g_dad.relayError = "retrying";
      if (g_emma.relayError.find("auth") != std::string::npos)
        g_emma.relayError = "retrying";
    }
    sendJSON(fd, relayJSON());
    return;
  }

  // /api/<dev>/<sub> (new) or legacy /api/<sub> (dad).
  const std::string prefix = "/api/";
  if (p.compare(0, prefix.size(), prefix) == 0) {
    std::string rest = p.substr(prefix.size());
    size_t slash = rest.find('/');
    if (slash != std::string::npos) {
      std::string maybeDev = rest.substr(0, slash);
      if (DeviceSim* dev = devById(maybeDev)) {
        handleDeviceClient(fd, *dev, rest.substr(slash + 1), method, body);
        return;
      }
      // Unknown /api/<x>/... — 404 rather than guessing dad.
      sendJSON(fd, "{\"error\":\"unknown device (use dad or emma)\"}", 404);
      return;
    }
    // Legacy single-device paths map to dad.
    handleDeviceClient(fd, g_dad, rest, method, body);
    return;
  }

  // Legacy frame alias.
  if (p == "/frame.bmp") {
    std::vector<uint8_t> bin = encodeBMP(g_dad);
    std::string head = "HTTP/1.0 200 OK\r\nContent-Type: image/bmp\r\nContent-Length: " +
                       std::to_string(bin.size()) +
                       "\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    sendAll(fd, head.c_str(), head.size());
    sendAll(fd, (const char*)bin.data(), bin.size());
    return;
  }

  sendJSON(fd, "{\"error\":\"not found\"}", 404);
}

int main(int argc, char** argv) {
  int port = 8077;
  std::string dumpDir;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
    if (!strncmp(argv[i], "--port=", 7)) port = atoi(argv[i] + 7);
    if (!strcmp(argv[i], "--dump") && i + 1 < argc) dumpDir = argv[++i];
    if (!strcmp(argv[i], "--index") && i + 1 < argc) g_indexPath = argv[++i];
    if (!strncmp(argv[i], "--relay-url=", 12)) {
      g_serverUrl = argv[i] + 12;
    }
    if (!strncmp(argv[i], "--server-url=", 13)) {
      g_serverUrl = argv[i] + 13;
    }
    if (!strncmp(argv[i], "--dad-token=", 12)) {
      std::lock_guard<std::mutex> lk(g_dad.mtx);
      g_dad.token = argv[i] + 12;
    }
    if (!strncmp(argv[i], "--emma-token=", 13)) {
      std::lock_guard<std::mutex> lk(g_emma.mtx);
      g_emma.token = argv[i] + 13;
    }
    if (!strcmp(argv[i], "--no-relay")) g_relayEnabled = false;
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

  loadRelayConfigFromEnv();

  bootTp = std::chrono::steady_clock::now();
  g_dad.display.init(SCREEN_W, SCREEN_H);
  g_dad.display.setRotation(DISPLAY_ROTATION);
  g_emma.display.init(SCREEN_W, SCREEN_H);
  g_emma.display.setRotation(DISPLAY_ROTATION);
  uiBegin(&g_dad.display);
  resetSim(g_dad, vnow());
  uiBegin(&g_emma.display);
  resetSim(g_emma, vnow());

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
      applyScene(g_dad, scenes[i]);
      std::string p = dumpDir + "/" + fnames[i] + ".ppm";
      writePPM(g_dad, p);
      printf("  %s\n", p.c_str());
    }
    return 0;
  }

  std::thread relayDad(relayDeviceThread, std::ref(g_dad), 0);
  std::thread relayEmma(relayDeviceThread, std::ref(g_emma), 500);

  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) {
    perror("socket");
    g_running = false;
    relayDad.join();
    relayEmma.join();
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
    g_running = false;
    relayDad.join();
    relayEmma.join();
    return 1;
  }
  if (listen(srv, 8) < 0) {
    perror("listen");
    g_running = false;
    relayDad.join();
    relayEmma.join();
    return 1;
  }
  printf("morse-code sim: http://localhost:%d/  (index: %s)\n", port,
         g_indexPath.c_str());
  printf("relay: %s dad-token:%s emma-token:%s\n", g_serverUrl.c_str(),
         g_dad.token.empty() ? "MISSING" : "set",
         g_emma.token.empty() ? "MISSING" : "set");
  printf("same renderer as the panel: uiFull/uiPattern/uiResult/uiProgress\n");
  fflush(stdout);

  while (g_running) {
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
    uint32_t now = vnow();
    tickDevice(g_dad, now);
    tickDevice(g_emma, now);
  }

  close(srv);
  relayDad.join();
  relayEmma.join();
  return 0;
}
