#pragma once
#include <Arduino.h>

struct GeminiResult {
  bool success;
  String parsedText;         // Gemini's transcription of the voice command
  String action;              // fetch_prescription | update_schedule | view_pill_count | low_stock_today | meal_pills | add_stock | none
  String actionDetailsJson;   // raw JSON string, passed straight to Supabase insert / UART
  String oledMessage;
};

// wavData/wavSize: the in-RAM WAV clip from audio_capture (getRecordingBuffer()/
// getRecordingSize()) - no SD, no Supabase Storage, sent straight to Gemini.
GeminiResult callGemini(const uint8_t *wavData, size_t wavSize, const String &contextJson);

// Minimal JSON string escaper for embedding raw text into hand-built JSON.
// Declared here (not just file-local in the .cpp) so it's usable elsewhere if needed.
String jsonEscape(const String &input);
