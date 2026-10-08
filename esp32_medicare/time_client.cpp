#include "time_client.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static bool synced = false;
static uint32_t baseSod = 0;          // seconds since local midnight at the last sync
static unsigned long baseMs = 0;      // millis() at the last sync

static bool attempted = false;        // has any sync been tried yet
static bool lastFailed = false;       // did the most recent attempt fail
static unsigned long lastAttemptMs = 0;

bool timeSyncNow() {
  if (WiFi.status() != WL_CONNECTED) {
    DBG("TIME", "sync skipped - WiFi not connected");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();   // class project; use a root cert for production

  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  String url = String("https://") + TIME_API_HOST +
               "/api/time/current/zone?timeZone=" + TIME_ZONE;

  if (!http.begin(client, url)) {
    DBG("TIME", "http.begin failed");
    return false;
  }
  http.addHeader("Accept", "application/json");

  unsigned long t = millis();
  int code = http.GET();
  if (code != 200) {
    DBG("TIME", "GET failed: %d %s (%lu ms)", code,
        code < 0 ? HTTPClient::errorToString(code).c_str() : "", millis() - t);
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    DBG("TIME", "JSON parse error: %s", err.c_str());
    return false;
  }

  // timeapi.io returns the local time already split into fields.
  int year   = doc["year"]    | -1;
  int month  = doc["month"]   | -1;
  int day    = doc["day"]     | -1;
  int hour   = doc["hour"]    | -1;
  int minute = doc["minute"]  | -1;
  int second = doc["seconds"] | -1;

  if (second < 0) second = doc["second"] | -1;   // tolerate a differently named seconds field

  // Fallback: parse "YYYY-MM-DDTHH:MM:SS..." if the split fields are missing.
  if (hour < 0 || minute < 0 || second < 0) {
    const char *dt = doc["dateTime"] | "";
    int y2, mo2, d2, h2, mi2, s2;
    if (sscanf(dt, "%d-%d-%dT%d:%d:%d", &y2, &mo2, &d2, &h2, &mi2, &s2) == 6) {
      year = y2; month = mo2; day = d2; hour = h2; minute = mi2; second = s2;
    }
  }

  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
    DBG("TIME", "unexpected reply: %s", payload.substring(0, 200).c_str());
    return false;
  }

  baseSod = (uint32_t)hour * 3600UL + (uint32_t)minute * 60UL + (uint32_t)second;
  baseMs  = millis();   // accuracy ~1 s, plenty for meal reminders
  synced  = true;

  DBG("TIME", "synced: %04d-%02d-%02d %02d:%02d:%02d %s (%lu ms)",
      year, month, day, hour, minute, second, TIME_ZONE, millis() - t);
  return true;
}

void timeUpdate() {
  unsigned long interval = lastFailed
      ? (unsigned long)TIME_RETRY_SEC * 1000UL
      : (unsigned long)TIME_SYNC_INTERVAL_MIN * 60UL * 1000UL;

  if (attempted && millis() - lastAttemptMs < interval) return;

  attempted = true;
  lastAttemptMs = millis();
  lastFailed = !timeSyncNow();
}

bool timeIsValid() { return synced; }

bool timeSecondsOfDay(int *secondsOfDay) {
  if (!synced) return false;
  uint32_t elapsed = (millis() - baseMs) / 1000UL;   // unsigned math survives millis() rollover
  *secondsOfDay = (int)((baseSod + elapsed) % 86400UL);
  return true;
}

String timeString() {
  int sod;
  if (!timeSecondsOfDay(&sod)) return String("--:--:--");
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", sod / 3600, (sod / 60) % 60, sod % 60);
  return String(buf);
}