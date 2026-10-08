#pragma once
#include <Arduino.h>

// SH1106 at text size 1: 6px per char wide, 8px per row tall.
// 128/6 = 21 chars per line, 64/8 = 8 rows (row 0 is the title bar).
#define OLED_CHARS_PER_LINE 21
#define OLED_VISIBLE_ROWS   7

// Initializes I2C on SDA_PIN/SCL_PIN and the SH1106 panel. Call once from setup().
// Returns false if the panel isn't found instead of halting, so the firmware
// still runs (Serial-only) when the OLED isn't plugged in.
bool initOledDisplay();

// True once initOledDisplay() has succeeded. Every draw call below is a no-op
// when this is false, so callers don't need to guard each one.
bool oledReady();

// Clears the screen and shows a short status line in large text (used for
// READY, ANALYZING..., and Gemini's oled_message after a voice command).
void showOledMessage(const String &message);

// Draws a title bar plus a scrollable list. `selected` is the index into
// `items` that gets the ">" cursor; `topIndex` is the first visible row.
void showScrollList(const String &title, const String items[], int itemCount,
                    int topIndex, int selected);

// Full-screen editor for a single numeric value (the refill screen).
void showValueEditor(const String &title, const String &label, int value);

// Two-line centered notice (used for saving/errors mid-action).
void showNotice(const String &line1, const String &line2);
