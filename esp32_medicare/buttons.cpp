#include "buttons.h"
#include "config.h"

void Button::begin(uint8_t pin, const char *label) {
  pinNum = pin;
  name = label;
  pinMode(pinNum, INPUT_PULLUP);
  stableLevel  = digitalRead(pinNum);
  lastRawLevel = stableLevel;
  lastChangeMs = millis();
  DBG("BTN", "%s on GPIO%d, idle level = %s", name, (int)pin,
      stableLevel == HIGH ? "HIGH (ok)" : "LOW (STUCK - check wiring/pin!)");
}

void Button::flush() {
  clickEvent = longEvent = repeatEvent = false;
}

void Button::update() {
  if (pinNum == 255) return;

  int raw = digitalRead(pinNum);
  unsigned long now = millis();

  if (raw != lastRawLevel) {
    lastRawLevel = raw;
    lastChangeMs = now;
  }

  if (now - lastChangeMs < DEBOUNCE_MS) return;   // still settling
  if (raw == stableLevel) {
    // Level unchanged - but if it's being held, service long-press/repeat.
    if (stableLevel == LOW) {
      if (!longFired && now - pressStartMs >= LONG_PRESS_MS) {
        longFired = true;
        longEvent = true;
        DBG("BTN", "%s LONG PRESS", name);
      }
      if (!repeatPrimed && now - pressStartMs >= REPEAT_DELAY_MS) {
        repeatPrimed = true;
        lastRepeatMs = now;
        repeatEvent  = true;
      } else if (repeatPrimed && now - lastRepeatMs >= REPEAT_RATE_MS) {
        lastRepeatMs = now;
        repeatEvent  = true;
      }
    }
    return;
  }

  // Confirmed edge.
  stableLevel = raw;

  if (stableLevel == LOW) {          // pressed
    pressStartMs = now;
    longFired    = false;
    repeatPrimed = false;
    repeatEvent  = true;             // first step lands immediately
    DBG("BTN", "%s PRESSED", name);
  } else {                           // released
    DBG("BTN", "%s RELEASED (held %lu ms)", name, now - pressStartMs);
    // A release only counts as a click if it wasn't already a long press.
    if (!longFired) {
      clickEvent = true;
      DBG("BTN", "%s CLICK", name);
    }
    longFired    = false;
    repeatPrimed = false;
  }
}

bool Button::clicked()     { bool e = clickEvent;  clickEvent  = false; return e; }
bool Button::longPressed() { bool e = longEvent;   longEvent   = false; return e; }
bool Button::repeated()    { bool e = repeatEvent; repeatEvent = false; return e; }
