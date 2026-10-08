#include "uart_protocol.h"
#include "config.h"
#include <ArduinoJson.h>

// Serial1 is used for Arduino communication, kept separate from Serial (USB debug).
// TX only (RX pin = -1): the link is one-way, ESP32 -> Arduino.
void initUartToArduino() {
  Serial1.begin(UART_BAUD, SERIAL_8N1, -1, UART_TX_PIN);
}

// Escapes pipe characters so they don't break the ACTION|MSG|EXTRA framing
static String sanitizeField(const String &input) {
  String out = input;
  out.replace("|", "/");
  out.replace("\n", " ");
  out.replace("\r", " ");
  return out;
}

// Sends one framed line to the Arduino and logs it (minus the trailing \n,
// since DBG adds its own).
static void txLine(const String &line) {
  Serial1.print(line);
  String shown = line;
  shown.trim();
  DBG("UART", "TX: %s", shown.c_str());
}

static void sendNone(const String &message) {
  txLine("NONE|" + sanitizeField(message) + "|\n");
}

// Meal reminders: one PLAY line, the Arduino plays that DFPlayer track.
void sendUartPlayTrack(int track, const String &label) {
  txLine("PLAY|" + sanitizeField(label) + "|" + String(track) + "\n");
}

// The MSG field is carried through for the Arduino's own debug logging only —
// the ESP32 is the one that actually displays it (see oled_display.h), since
// that's where the OLED physically lives.
void sendUartResult(const GeminiResult &result) {
  DBG("UART", "sendUartResult: success=%d action=%s", result.success, result.action.c_str());

  if (!result.success) {
    // Any Gemini/network failure is treated as a NONE action so Arduino has
    // a consistent, always-safe fallback state instead of a custom error protocol
    DBG("UART", "Gemini failed -> NONE");
    sendNone(result.oledMessage);
    return;
  }

  String actionUpper = result.action;
  actionUpper.toUpperCase();

  if (actionUpper == "FETCH_PRESCRIPTION") {
    // Send one FETCH line per compartment — dispensing logic on Arduino
    // handles one compartment per FETCH line, so a medicine spanning
    // multiple compartments gets multiple lines.
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, result.actionDetailsJson);
    if (err || !doc["compartment_ids"].is<JsonArray>()) {
      DBG("UART", "FETCH has no usable compartment_ids (%s) -> NONE", err ? err.c_str() : "missing");
      sendNone(result.oledMessage);
      return;
    }

    JsonArray ids = doc["compartment_ids"];

    // Guard: Gemini can return fetch_prescription with an empty id list
    // (e.g. it recognized the intent but matched no compartment). Without
    // this, the loop below sends nothing at all and the Arduino is left
    // hanging with no line for a command it should respond to.
    if (ids.size() == 0) {
      DBG("UART", "FETCH with empty compartment_ids -> NONE");
      sendNone(result.oledMessage);
      return;
    }

    for (JsonVariant v : ids) {
      int compartmentId = v.as<int>();
      txLine("FETCH|" + sanitizeField(result.oledMessage) + "|" + String(compartmentId) + "\n");
      delay(50);  // small gap so Arduino's serial buffer isn't overwhelmed by back-to-back lines
    }
  }
  else if (actionUpper == "UPDATE_SCHEDULE") {
    // The Arduino needs to know WHAT to update, not just that something
    // changed. Pack medicine/field/new_value into EXTRA as
    // semicolon-separated key=value pairs (no pipes, so framing stays intact).
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, result.actionDetailsJson);

    String extra = "";
    if (!err) {
      String medicine = doc["medicine_name"].as<String>();
      String field    = doc["field"].as<String>();
      String newValue = doc["new_value"].as<String>();

      if (medicine.length() && medicine != "null") extra += "medicine=" + sanitizeField(medicine);
      if (field.length()    && field    != "null") {
        if (extra.length()) extra += ";";
        extra += "field=" + sanitizeField(field);
      }
      if (newValue.length() && newValue != "null") {
        if (extra.length()) extra += ";";
        extra += "value=" + sanitizeField(newValue);
      }
    }

    // Nothing usable to act on — fall back to NONE rather than sending a
    // bare UPDATE the Arduino can't interpret.
    if (extra.length() == 0) {
      DBG("UART", "UPDATE has nothing actionable -> NONE");
      sendNone(result.oledMessage);
      return;
    }

    txLine("UPDATE|" + sanitizeField(result.oledMessage) + "|" + extra + "\n");
  }
  else {
    // NONE, or view_pill_count (display-only, handled locally on the ESP32 —
    // see esp32_medicare.ino, which calls showPillCountsFromContext()
    // instead of this function for that action)
    sendNone(result.oledMessage);
  }
}