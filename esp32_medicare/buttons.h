#pragma once
#include <Arduino.h>

// Debounced button helper for active-LOW buttons wired to GND with
// INPUT_PULLUP. Supports three gestures so three physical buttons can drive
// the whole menu:
//   clicked()     - short press, fires once on release
//   longPressed() - fires once when held past LONG_PRESS_MS (used as "back")
//   repeated()    - fires once immediately, then auto-repeats while held
//                   (used for scrolling lists and adjusting counts)
class Button {
public:
  // `label` is only used in debug logging ("UP", "PTT", ...).
  void begin(uint8_t pin, const char *label = "?");
  void update();                 // call every loop()
  void flush();                  // drop any pending click/long/repeat events

  bool clicked();                // consumes the event
  bool longPressed();            // consumes the event
  bool repeated();               // consumes the event
  bool isDown() const { return stableLevel == LOW; }

private:
  static const uint16_t DEBOUNCE_MS     = 30;
  static const uint16_t LONG_PRESS_MS   = 800;
  static const uint16_t REPEAT_DELAY_MS = 450;   // wait before auto-repeat starts
  static const uint16_t REPEAT_RATE_MS  = 150;   // then one step every this long

  const char *name       = "?";
  uint8_t  pinNum        = 255;
  int      stableLevel   = HIGH;
  int      lastRawLevel  = HIGH;
  unsigned long lastChangeMs = 0;
  unsigned long pressStartMs = 0;
  unsigned long lastRepeatMs = 0;

  bool longFired    = false;   // long press already reported for this hold
  bool repeatPrimed = false;   // initial repeat already fired for this hold

  bool clickEvent   = false;
  bool longEvent    = false;
  bool repeatEvent  = false;
};
