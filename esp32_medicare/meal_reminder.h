#pragma once
#include <Arduino.h>

// Meal-time voice reminders. Six events a day (all times and track numbers
// are in config.h):
//   breakfast / lunch / dinner reminder           at the meal time
//   breakfast / lunch / dinner follow-up reminder MEAL_FOLLOWUP_DELAY_MIN later
// Each one sends "PLAY|label|track" to the Arduino, shows a notice on the OLED,
// and fires once per day.
//
// Call every loop() while the system is idle (STATE_MENU). It reads the local
// clock from time_client, so it costs nothing until an event is due.
void mealReminderUpdate();
