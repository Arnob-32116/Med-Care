#pragma once
#include <Arduino.h>

#define MAX_COMPARTMENTS 8

struct Compartment {
  int    id;
  String name;
  int    count;
  String dosage;
  String timeOfDay;
  String mealRelation;
};

// Fetches active_compartments + last 5 voice_commands, returns as one JSON
// string. Used as the context blob for the Gemini call.
String fetchSupabaseContext();

// Fetches the full compartment rows (including dosage/time/meal fields the
// Gemini context doesn't carry) into `out`. Returns how many were written,
// or -1 on failure. Used by the OLED menu screens.
int fetchCompartments(Compartment *out, int maxItems);

// Writes a new pill count for one compartment (the refill workflow).
// See the note in the README about this being an ESP32-side write.
bool updateCompartmentCount(int compartmentId, int newCount);

// Inserts a new voice_commands row after Gemini responds
bool insertVoiceCommandRow(const String &parsedText, const String &action, const String &actionDetailsJson);
