#pragma once
#include <Arduino.h>

// Keeps a local wall clock (TIME_ZONE in config.h) synced from a time API.
// timeUpdate() re-syncs every TIME_SYNC_INTERVAL_MIN minutes and retries every
// TIME_RETRY_SEC seconds after a failure. Between syncs the clock runs on
// millis(), so callers can read the time as often as they like with no network
// traffic.

// One HTTPS call to the time API. Returns true and updates the clock on success.
bool timeSyncNow();

// Call every loop(). Does a sync when one is due; that call can block for a
// few seconds (TLS handshake + request).
void timeUpdate();

// True once at least one sync has succeeded.
bool timeIsValid();

// Seconds since local midnight (0..86399). Returns false if never synced.
bool timeSecondsOfDay(int *secondsOfDay);

// "HH:MM:SS" for logs, or "--:--:--" before the first sync.
String timeString();
