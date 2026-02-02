#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <Adafruit_Protomatter.h>
#include <Fonts/nycta_r464pt7b.h>

#if defined(ARDUINO_ADAFRUIT_MATRIXPORTAL_ESP32S3)  // MatrixPortal ESP32-S3
uint8_t rgbPins[] = { 42, 41, 40, 38, 39, 37 };
uint8_t addrPins[] = { 45, 36, 48, 35, 21 };
uint8_t clockPin = 2;
uint8_t latchPin = 47;
uint8_t oePin = 14;
#endif

Adafruit_Protomatter matrix(
  64,                         // Matrix width in pixels
  6,                          // Bit depth -- 6 here provides maximum color options
  1, rgbPins,                 // # of matrix chains, array of 6 RGB pins for each
  5, addrPins,                // # of address pins (height is inferred), array of pins. 5 for 64
  clockPin, latchPin, oePin,  // Other matrix control pins
  true);                      // HERE IS THE MAGIC FOR DOUBLE-BUFFERING!

int16_t textX;        // Current text position (X)
int16_t textY;        // Current text position (Y)
int16_t textMin;      // Text pos. (X) when scrolled off left edge
char matrixStr[256];  // Buffer to hold scrolling message text

// nanopb
extern "C" {
#include "pb.h"
#include "pb_decode.h"
#include "gtfs-realtime.pb.h"
}

// ---------------- USER CONFIG ----------------
static const char* WIFI_SSID = "Gardsnas-II";
static const char* WIFI_PASS = "cloudest-COMPLY-primer";

static const char* MTA_API_KEY = "";  // optional

static const char* HOST = "api-endpoint.mta.info";
static const uint16_t PORT = 443;
static const char* PATH = "/Dataservice/mtagtfsfeeds/nyct%2Fgtfs-ace";

static const char* TARGET_ROUTE = "A";
static const char* TARGET_STOP = "A12N";  // 145 St uptown platform

static const uint32_t POLL_MS = 30000;

// Hard cap to prevent runaway allocations in INTERNAL heap
static const size_t MAX_BODY_BYTES = 180 * 1024;  // 180 KB

// Read chunk size from socket into internal RAM buffer
static const size_t READ_CHUNK = 2048;

// TLS: easiest path is insecure. For production, use CA/cert pinning.
static const bool TLS_INSECURE = true;
// ---------------------------------------------

// ---------- Types ----------
struct Arrival {
  uint32_t t;
  char trip_id[64];
};
// -------------------------

// ----------------- CALLBACK DECODE (requires FT_CALLBACK options) -----------------
// Streams FeedMessage.entity and TripUpdate.stop_time_update so we don't allocate huge arrays.

static const int MAX_ARRIVALS = 32;

struct ArrivalsAcc {
  Arrival items[MAX_ARRIVALS];
  int count;
};

static inline void acc_add(ArrivalsAcc* acc, uint32_t t, const char* trip_id) {
  if (!acc || acc->count >= MAX_ARRIVALS) return;
  acc->items[acc->count].t = t;
  if (trip_id) {
    strncpy(acc->items[acc->count].trip_id, trip_id, sizeof(acc->items[acc->count].trip_id) - 1);
    acc->items[acc->count].trip_id[sizeof(acc->items[acc->count].trip_id) - 1] = '\0';
  } else {
    acc->items[acc->count].trip_id[0] = '\0';
  }
  acc->count++;
}

struct EntityTmp {
  uint32_t times[16];
  int count;
};

struct StopCbArg {
  const char* stop_id;  // e.g. "A12N"
  EntityTmp* tmp;
  uint32_t* cb_calls;
};

struct FeedCbArg {
  const char* route_id;  // e.g. "A"
  const char* stop_id;   // e.g. "A12N"
  ArrivalsAcc* out;
  uint32_t cb_calls;
};

// StopTimeUpdate callback: decode directly from *stream*
static bool stop_time_update_cb(pb_istream_t* stream, const pb_field_t* field, void** arg) {
  (void)field;
  StopCbArg* a = (StopCbArg*)(*arg);
  if (!a || !a->tmp || !a->stop_id) return false;

  transit_realtime_TripUpdate_StopTimeUpdate stu = transit_realtime_TripUpdate_StopTimeUpdate_init_zero;

  // IMPORTANT: decode directly, no pb_make_string_substream
  bool ok = pb_decode(stream, transit_realtime_TripUpdate_StopTimeUpdate_fields, &stu);
  if (!ok) {
    Serial.printf("STU decode failed: %s\n", PB_GET_ERROR(stream));
    return false;
  }

  if (stu.has_stop_id && strcmp(stu.stop_id, a->stop_id) == 0) {
    uint32_t best = 0;
    if (stu.has_arrival && stu.arrival.has_time) best = (uint32_t)stu.arrival.time;
    else if (stu.has_departure && stu.departure.has_time) best = (uint32_t)stu.departure.time;

    if (best && a->tmp->count < (int)(sizeof(a->tmp->times) / sizeof(a->tmp->times[0]))) {
      a->tmp->times[a->tmp->count++] = best;
    }
  }

  if (a->cb_calls) {
    (*a->cb_calls)++;
    if (((*a->cb_calls) & 0x3F) == 0) yield();
  }
  return true;
}

// FeedEntity callback: decode directly from *stream*
static bool feed_entity_cb(pb_istream_t* stream, const pb_field_t* field, void** arg) {
  (void)field;
  FeedCbArg* g = (FeedCbArg*)(*arg);
  if (!g || !g->out) return false;

  transit_realtime_FeedEntity ent = transit_realtime_FeedEntity_init_zero;

  EntityTmp tmp = {};
  StopCbArg sarg = { g->stop_id, &tmp, &g->cb_calls };

  // Attach nested callback BEFORE decoding entity
  ent.trip_update.stop_time_update.funcs.decode = &stop_time_update_cb;
  ent.trip_update.stop_time_update.arg = &sarg;

  bool ok = pb_decode(stream, transit_realtime_FeedEntity_fields, &ent);
  if (!ok) {
    Serial.printf("Entity decode failed: %s\n", PB_GET_ERROR(stream));
    return false;
  }

  bool route_ok = false;
  const char* trip_id = nullptr;
  if (ent.has_trip_update) {
    const auto& trip = ent.trip_update.trip;
    if (trip.has_route_id && strcmp(trip.route_id, g->route_id) == 0) {
      route_ok = true;
      if (trip.has_trip_id) trip_id = trip.trip_id;
    }
  }

  if (route_ok) {
    for (int i = 0; i < tmp.count; i++) {
      acc_add(g->out, tmp.times[i], trip_id);
    }
  }

  g->cb_calls++;
  if ((g->cb_calls & 0x7F) == 0) yield();
  return true;
}
// -------------------------------------------------------------------------------

static void printHeapStats(const char* tag) {
  Serial.printf("[%s] Free heap=%u | Min free heap=%u\n", tag, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
}

static void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Syncing NTP");
  time_t now = 0;
  int tries = 0;
  while (now < 1700000000 && tries < 60) {
    delay(250);
    Serial.print(".");
    time(&now);
    tries++;
  }
  Serial.println();
  Serial.print("Unix time: ");
  Serial.println((unsigned long)now);
}

// Read CRLF line into buf (without CRLF)
static bool readLine(WiFiClientSecure& c, char* out, size_t outMax, uint32_t timeoutMs) {
  size_t n = 0;
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    while (c.available()) {
      char ch = (char)c.read();
      if (ch == '\r') {
        // consume '\n' if present
        uint32_t t0 = millis();
        while (!c.available() && millis() - t0 < timeoutMs) delay(1);
        if (c.available() && (char)c.peek() == '\n') c.read();
        out[n] = '\0';
        return true;
      }
      if (ch == '\n') {
        out[n] = '\0';
        return true;
      }
      if (n + 1 < outMax) out[n++] = ch;
    }
    if (!c.connected()) break;
    delay(1);
  }
  return false;
}

static bool startsWithNoCase(const char* s, const char* prefix) {
  while (*prefix) {
    char a = *s++;
    char b = *prefix++;
    if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
    if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
    if (a != b) return false;
  }
  return true;
}

static bool containsNoCase(const char* s, const char* needle) {
  for (size_t i = 0; s[i]; i++) {
    size_t j = 0;
    while (needle[j]) {
      char a = s[i + j];
      if (!a) break;
      char b = needle[j];
      if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
      if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
      if (a != b) break;
      j++;
    }
    if (!needle[j]) return true;
  }
  return false;
}

static int cmpArrival(const void* a, const void* b) {
  const Arrival* A = (const Arrival*)a;
  const Arrival* B = (const Arrival*)b;
  if (A->t < B->t) return -1;
  if (A->t > B->t) return 1;
  return 0;
}

static void printArrivals(const Arrival* arr, int n) {
  time_t now;
  time(&now);

  matrix.fillScreen(0);  // Fill background black
  matrix.setCursor(textX, textY);

  Serial.println("\n--- A arrivals @ 145 St uptown (A12N) ---");
  sprintf(matrixStr, " A Uptown\n 145th St.\n NEXT TRAINS\n\n");  //1) 3:15 min\n2) 5:45 min\n3) 12:30 min");


  if (n <= 0) {
    Serial.println("No matching arrivals found in this snapshot.");
    sprintf(matrixStr, "%sNo Trains found\n",matrixStr);
    return;
  }
  for (int i = 0; i < 3; i++) { // was i < n
    double fmins = ((double)arr[i].t - (double)now) / 60.0;
    int mins = (int)trunc(fmins);
    int secs = (int)trunc((fmins - (double)mins) * 60);

    //Serial.printf("%2d) in %5d:%2d min:sec | unix=%u | trip_id=%s\n", i + 1, mins, secs, (unsigned)arr[i].t, arr[i].trip_id);
    //Serial.printf("%2d) in %7.2f min | unix=%u | trip_id=%s\n", i + 1, fmins, (unsigned)arr[i].t, arr[i].trip_id);

    sprintf(matrixStr, "%s %d) %5d:%2d\n",matrixStr,i+1,mins,secs);
  }
  matrix.print(matrixStr);
  matrix.show();
}

// Debug helpers (kept lightweight)
static void printTaskStats(const char* tag) {
  UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
  Serial.printf("[%s] stack high watermark (words)=%u (~%u bytes)\n",
                tag, (unsigned)watermark, (unsigned)watermark * 4);
}

static void heapCheck(const char* tag) {
  bool ok = heap_caps_check_integrity_all(true);
  Serial.printf("[%s] heap integrity: %s\n", tag, ok ? "OK" : "BROKEN");
}

/*
  Raw HTTPS GET. Reads body into INTERNAL HEAP (malloc).

  Returns true with (*out_buf, *out_len) set. Caller frees *out_buf.
  Expects Content-Length (MTA provides it).
*/
static bool httpsGetBodyInternal(uint8_t** out_buf, size_t* out_len) {
  *out_buf = nullptr;
  *out_len = 0;

  // printTaskStats("pre-client");
  // heapCheck("pre-client");

  WiFiClientSecure client;
  client.setTimeout(20);  // seconds
  if (TLS_INSECURE) client.setInsecure();

  Serial.printf("Connecting to %s:%u...\n", HOST, PORT);
  if (!client.connect(HOST, PORT)) {
    Serial.println("❌ TLS connect failed");
    return false;
  }

  // Send request
  client.printf("GET %s HTTP/1.1\r\n", PATH);
  client.printf("Host: %s\r\n", HOST);
  client.print("User-Agent: esp32-gtfsrt/1.0\r\n");
  client.print("Accept: application/x-protobuf\r\n");
  // Avoid gzip so body is raw protobuf.
  client.print("Accept-Encoding: identity\r\n");
  // Simpler: close after response.
  client.print("Connection: close\r\n");
  if (MTA_API_KEY && strlen(MTA_API_KEY) > 0) {
    client.print("x-api-key: ");
    client.print(MTA_API_KEY);
    client.print("\r\n");
  }
  client.print("\r\n");

  // Status line
  char line[192];
  if (!readLine(client, line, sizeof(line), 8000)) {
    Serial.println("❌ Failed to read status line");
    client.stop();
    return false;
  }
  Serial.print("Status: ");
  Serial.println(line);
  if (!containsNoCase(line, "200")) {
    Serial.println("❌ Non-200 status");
    client.stop();
    return false;
  }

  // Headers
  long contentLen = -1;
  bool chunked = false;
  bool gz = false;

  while (true) {
    if (!readLine(client, line, sizeof(line), 8000)) {
      Serial.println("❌ Header read timeout");
      client.stop();
      return false;
    }
    if (line[0] == '\0') break;

    if (startsWithNoCase(line, "Content-Length:")) {
      const char* p = line + strlen("Content-Length:");
      while (*p == ' ' || *p == '\t') p++;
      contentLen = strtol(p, nullptr, 10);
    }
    if (startsWithNoCase(line, "Transfer-Encoding:") && containsNoCase(line, "chunked")) {
      chunked = true;
    }

    // inside header loop:
    if (startsWithNoCase(line, "Content-Encoding:") && containsNoCase(line, "gzip")) gz = true;
  }

  Serial.printf("Headers parsed: contentLen=%ld chunked=%s ", contentLen, chunked ? "yes" : "no");

  // printTaskStats("post-headers");
  // heapCheck("post-headers");

  if (chunked) {
    Serial.println("❌ This version expects Content-Length, but got chunked transfer.");
    client.stop();
    return false;
  }
  if (contentLen <= 0) {
    Serial.println("❌ Missing/invalid Content-Length");
    client.stop();
    return false;
  }
  if ((size_t)contentLen > MAX_BODY_BYTES) {
    Serial.printf("❌ Body too large for internal heap cap (%u bytes)\n", (unsigned)MAX_BODY_BYTES);
    client.stop();
    return false;
  }

  uint8_t* buf = (uint8_t*)malloc((size_t)contentLen);
  if (!buf) {
    Serial.println("❌ malloc failed (internal heap) for body");
    client.stop();
    return false;
  }

  uint8_t* tmp = (uint8_t*)malloc(READ_CHUNK);
  if (!tmp) {
    Serial.println("❌ malloc failed for tmp buffer");
    free(buf);
    client.stop();
    return false;
  }

  // Serial.printf("Content-Encoding gzip? %s\n", gz ? "YES" : "NO");
  // if (gz) {
  //   Serial.println("❌ Got gzip body; nanopb can't decode compressed bytes.");
  //   // bail out
  // }


  // printTaskStats("post-malloc");
  // heapCheck("post-malloc");

  size_t got = 0;
  uint32_t lastPrint = millis();

  const uint32_t OVERALL_TIMEOUT_MS = 60000;
  const uint32_t IDLE_TIMEOUT_MS = 15000;

  uint32_t startMs = millis();
  uint32_t lastProgressMs = millis();

  while (got < (size_t)contentLen) {
    if (millis() - startMs > OVERALL_TIMEOUT_MS) {
      Serial.printf("❌ overall timeout at got=%u / %ld\n", (unsigned)got, contentLen);
      free(tmp);
      free(buf);
      client.stop();
      return false;
    }

    int avail = client.available();
    if (avail <= 0) {
      if (millis() - lastProgressMs > IDLE_TIMEOUT_MS) {
        Serial.printf("❌ idle timeout at got=%u / %ld\n", (unsigned)got, contentLen);
        heapCheck("idle-timeout");
        free(tmp);
        free(buf);
        client.stop();
        return false;
      }
      delay(10);
      continue;
    }

    size_t remaining = (size_t)contentLen - got;
    size_t want = (size_t)avail;
    if (want > remaining) want = remaining;
    if (want > READ_CHUNK) want = READ_CHUNK;

    int r = client.read(tmp, (int)want);
    if (r > 0) {
      memcpy(buf + got, tmp, (size_t)r);
      got += (size_t)r;
      lastProgressMs = millis();
    } else {
      delay(10);
    }

    if ((got % 8192) == 0 || (millis() - lastPrint) > 2000) {
      Serial.printf("...downloaded %u / %ld bytes\n", (unsigned)got, contentLen);
      lastPrint = millis();
      delay(1);
    }
  }

  client.stop();
  free(tmp);

  Serial.printf("Body read complete: got=%u expected=%ld\n", (unsigned)got, contentLen);

  // heapCheck("post-body");
  // printTaskStats("post-body");

  *out_buf = buf;
  *out_len = got;
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(3000);
  Serial.println(__FILE__);
  delay(2000);

  // Initialize matrix...
  ProtomatterStatus status = matrix.begin();
  Serial.print("Protomatter begin() status: ");
  Serial.println((int)status);
  if (status != PROTOMATTER_OK) {
    Serial.println("not ok");
    for (;;)
      ;
  }

  //sprintf(matrixStr, "A Uptown\n145th St.\nNEXT TRAINS\n1) 3:15 min\n2) 5:45 min\n3) 12:30 min");
  matrix.setFont(&nycta_r464pt7b);
  matrix.setTextWrap(false);    // Allow text off edge
  matrix.setTextColor(0x003f);  // Blue

  int16_t x1, y1;
  uint16_t w, h;
  matrix.getTextBounds(matrixStr, 0, 0, &x1, &y1, &w, &h);  // How big is it?
  textMin = -w;                                             // All text is off left edge when it reaches this point
  textX = 1;                                                //matrix.width(); // Start off right edge
  textY = 6;                                                //matrix.height() / 2 - (y1 + h / 2); // Center text vertically
  //Serial.printf("string:\n%s\nx1:%d y1:%d w:%d h:%d textMin:%d textX:%d textY:%d\nmatrix.height %d\n",str,x1,y1,w,h,textX,textY,matrix.height());

  Serial.println("\Starting wifi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected: ");
  Serial.println(WiFi.localIP());

  // Critical for avoiding mid-transfer stalls
  WiFi.setSleep(false);

  delay(1000);
  syncTime();
  //printHeapStats("after ntp");

  UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
  Serial.printf("[loop entry] stack high watermark (words)=%u (~%u bytes)\n",
                (unsigned)hw, (unsigned)hw * 4);
  delay(10);
}

void loop() {
  Serial.println("\nFetching feed...");
  //printHeapStats("pre-fetch");

  uint8_t* raw = nullptr;
  size_t rawLen = 0;

  if (!httpsGetBodyInternal(&raw, &rawLen)) {
    Serial.println("❌ Failed to fetch body");
    delay(POLL_MS);
    return;
  }

  //printHeapStats("post-fetch");

  // ===== CALLBACK DECODE (stream entities + stop_time_update) =====
  transit_realtime_FeedMessage feed = transit_realtime_FeedMessage_init_zero;
  //Serial.printf("sizeof(feed) = %u bytes\n", (unsigned)sizeof(feed));  //transit_realtime_FeedMessage));

  ArrivalsAcc acc = {};

  FeedCbArg farg = {};
  farg.route_id = TARGET_ROUTE;
  farg.stop_id = TARGET_STOP;
  farg.out = &acc;
  farg.cb_calls = 0;

  // These fields exist ONLY if gtfs-realtime.pb.h was generated with:
  //   transit_realtime.FeedMessage.entity type:FT_CALLBACK
  //   transit_realtime.TripUpdate.stop_time_update type:FT_CALLBACK
  feed.entity.funcs.decode = &feed_entity_cb;
  feed.entity.arg = &farg;

  pb_istream_t istream = pb_istream_from_buffer(raw, rawLen);

  uint32_t t0 = millis();
  bool ok = pb_decode(&istream, transit_realtime_FeedMessage_fields, &feed);
  uint32_t tDecode = millis() - t0;

  free(raw);

  Serial.printf("pb_decode time ms=%u\n", (unsigned)tDecode);

  if (!ok) {
    Serial.printf("❌ Decode failed: %s\n", PB_GET_ERROR(&istream));
    printHeapStats("post-decode");
    delay(POLL_MS);
    return;
  }

  // Sort by time
  if (acc.count > 1) qsort(acc.items, acc.count, sizeof(Arrival), cmpArrival);

  // Keep only future arrivals and show first 10
  time_t now;
  time(&now);
  Arrival future[10];
  int nf = 0;
  for (int i = 0; i < acc.count && nf < 10; i++) {
    if (acc.items[i].t >= (uint32_t)now) future[nf++] = acc.items[i];
  }

  printArrivals(future, nf);

  delay(POLL_MS);
}
