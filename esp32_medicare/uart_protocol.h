#pragma once
#include <Arduino.h>
#include "gemini_client.h"

void initUartToArduino();

// Forwards only actions the Arduino needs to physically act on
// (FETCH_PRESCRIPTION -> dispense, UPDATE_SCHEDULE -> update its RTC table).
// view_pill_count is display-only and handled entirely on the ESP32 (see
// oled_display.h) since the OLED lives here, not on the Arduino — don't
// call this for that action.
void sendUartResult(const GeminiResult &result);

// Tells the Arduino to play one DFPlayer Mini track: "PLAY|<label>|<track>\n".
// `label` is only for the Arduino's debug log; `track` is the file number on its SD card.
void sendUartPlayTrack(int track, const String &label);