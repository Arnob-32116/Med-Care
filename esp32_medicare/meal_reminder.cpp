#include "meal_reminder.h"
#include "config.h"
#include "time_client.h"
#include "uart_protocol.h"
#include "oled_display.h"
#include "menu.h"

static_assert(MEAL_GRACE_MIN <= MEAL_FOLLOWUP_DELAY_MIN,
              "MEAL_GRACE_MIN must be <= MEAL_FOLLOWUP_DELAY_MIN (config.h) or the windows overlap");

#define MIN_OF(h, m)  ((h) * 60 + (m))
#define NOTICE_MS     2500

struct MealEvent {
  const char *label;    // sent to the Arduino (debug log only)
  const char *line1;    // OLED line 1
  const char *line2;    // OLED line 2
  int  minuteOfDay;     // when it triggers
  int  track;           // DFPlayer track number (config.h)
  bool fired;           // already played today
};

static MealEvent events[] = {
  { "breakfast",          "Breakfast time",  "Take your pills",
    MIN_OF(MEAL_BREAKFAST_HOUR, MEAL_BREAKFAST_MINUTE), AUDIO_BREAKFAST_TRACK, false },
  { "lunch",              "Lunch time",      "Take your pills",
    MIN_OF(MEAL_LUNCH_HOUR, MEAL_LUNCH_MINUTE),         AUDIO_LUNCH_TRACK,     false },
  { "dinner",             "Dinner time",     "Take your pills",
    MIN_OF(MEAL_DINNER_HOUR, MEAL_DINNER_MINUTE),       AUDIO_DINNER_TRACK,    false },

  { "breakfast-followup", "Breakfast pills", "Did you take them?",
    MIN_OF(MEAL_BREAKFAST_HOUR, MEAL_BREAKFAST_MINUTE) + MEAL_FOLLOWUP_DELAY_MIN, AUDIO_BREAKFAST_FOLLOWUP_TRACK, false },
  { "lunch-followup",     "Lunch pills",     "Did you take them?",
    MIN_OF(MEAL_LUNCH_HOUR, MEAL_LUNCH_MINUTE) + MEAL_FOLLOWUP_DELAY_MIN,         AUDIO_LUNCH_FOLLOWUP_TRACK,     false },
  { "dinner-followup",    "Dinner pills",    "Did you take them?",
    MIN_OF(MEAL_DINNER_HOUR, MEAL_DINNER_MINUTE) + MEAL_FOLLOWUP_DELAY_MIN,       AUDIO_DINNER_FOLLOWUP_TRACK,    false },
};
static const int EVENT_COUNT = sizeof(events) / sizeof(events[0]);

static void resetFlags() {
  for (int i = 0; i < EVENT_COUNT; i++) events[i].fired = false;
  DBG("MEAL", "new day - reminders re-armed");
}

void mealReminderUpdate() {
  // The clock only needs checking about once a second.
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 1000) return;
  lastCheck = millis();

  int sod;
  if (!timeSecondsOfDay(&sod)) return;   // clock not synced yet

  // Midnight rollover: time of day jumps from ~86399 back to ~0. A small
  // backwards step from a re-sync (a second or two) must NOT count as a new day.
  static int lastSod = -1;
  if (lastSod >= 0 && lastSod - sod > 12 * 3600) resetFlags();
  lastSod = sod;

  int nowMin = sod / 60;

  for (int i = 0; i < EVENT_COUNT; i++) {
    MealEvent &e = events[i];
    if (e.fired) continue;

    // "Around" the event time: from the exact minute until MEAL_GRACE_MIN later,
    // so a reminder is not lost if loop() was busy or the board booted late.
    if (nowMin >= e.minuteOfDay && nowMin < e.minuteOfDay + MEAL_GRACE_MIN) {
      e.fired = true;
      DBG("MEAL", "%s due (clock %s) -> track %d", e.label, timeString().c_str(), e.track);

      sendUartPlayTrack(e.track, e.label);

      showNotice(e.line1, e.line2);
      delay(NOTICE_MS);
      menuMarkDirty();
      return;   // one event per check
    }
  }
}
