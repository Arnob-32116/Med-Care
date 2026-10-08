#include "gemini_client.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <base64.h>   // arduino-base64 library (Densaugeo/base64), exposes base64::encode

const char* SYSTEM_INSTRUCTION =
  "You are the command interpreter for MediCare Companion, a pill dispenser. "
  "You receive a voice command audio clip plus recent context (active compartments, "
  "last 5 voice commands). Decide exactly ONE action: "
  "fetch_prescription (the patient wants pills dispensed now; ALWAYS return "
  "compartment_ids using the compartment_id values from the context for the "
  "requested medicine, and medicine_name), "
  "update_schedule (change a medicine's time_of_day or meal_relation), "
  "view_pill_count (show how many pills are left), "
  "low_stock_today (asks which pills will run out, finish, or are not enough for today), "
  "meal_pills (the speaker says they ate, or are about to eat, breakfast, lunch or dinner and asks "
  "which pills to take: return meal, and meal_state = after for 'I ate' or before for 'I am about to eat'), "
  "add_stock (a caregiver adds pills to a compartment: return quantity, and the compartment_ids or "
  "medicine_name the speaker named; if the medicine is in more than one compartment and the speaker "
  "did not say which, return none with reason 'ambiguous'), "
  "or none. Only reference medicines/compartments "
  "present in the context. If a medicine's current_count is 0, never return "
  "fetch_prescription for it — return none with reason 'out of stock'. "
  "oled_message must be under 40 characters, no punctuation.";

const char* RESPONSE_SCHEMA_JSON = R"({
  "type": "OBJECT",
  "properties": {
    "parsed_text": { "type": "STRING" },
    "action": { "type": "STRING", "enum": ["fetch_prescription","update_schedule","view_pill_count","low_stock_today","meal_pills","add_stock","none"] },
    "action_details": {
      "type": "OBJECT",
      "properties": {
        "medicine_name": { "type": "STRING" },
        "compartment_ids": { "type": "ARRAY", "items": { "type": "INTEGER" } },
        "field": { "type": "STRING", "enum": ["time_of_day","meal_relation"] },
        "new_value": { "type": "STRING" },
        "quantity": { "type": "INTEGER" },
        "meal": { "type": "STRING", "enum": ["breakfast","lunch","dinner"] },
        "meal_state": { "type": "STRING", "enum": ["before","after"] },
        "reason": { "type": "STRING" }
      }
    },
    "oled_message": { "type": "STRING" }
  },
  "required": ["parsed_text","action","oled_message"]
})";

GeminiResult callGemini(const uint8_t *wavData, size_t wavSize, const String &contextJson) {
  GeminiResult result;
  result.success = false;

  if (!wavData || wavSize == 0) {
    DBG("GEMINI", "no audio data (ptr=%p, size=%u)", wavData, (unsigned)wavSize);
    result.oledMessage = "NO AUDIO DATA";
    return result;
  }

  size_t base64Size = ((wavSize + 2) / 3) * 4;  // exact base64 output length

  // Build the JSON prefix/suffix around the base64 audio data
  String prefix =
    "{\"system_instruction\":{\"parts\":[{\"text\":" + jsonEscape(SYSTEM_INSTRUCTION) + "}]},"
    "\"contents\":[{\"parts\":["
    "{\"text\":" + jsonEscape("Context:\n" + contextJson) + "},"
    "{\"text\":\"New voice command audio follows.\"},"
    "{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"";

  String suffix =
    "\"}}]}],"
    "\"generationConfig\":{\"responseMimeType\":\"application/json\",\"responseSchema\":" +
    String(RESPONSE_SCHEMA_JSON) + "}}";

  size_t contentLength = prefix.length() + base64Size + suffix.length();
  DBG("GEMINI", "request: wav=%u B, base64=%u B, total body=%u B, heap=%lu",
      (unsigned)wavSize, (unsigned)base64Size, (unsigned)contentLength, (unsigned long)ESP.getFreeHeap());

  WiFiClientSecure client;
  client.setInsecure();   // class project — swap for setCACert() with Google's root CA for production
  client.setTimeout(15000);

  if (!client.connect(GEMINI_HOST, 443)) {
    DBG("GEMINI", "TLS connect FAILED (wifi=%d)", (int)WiFi.status());
    result.oledMessage = "GEMINI CONN FAIL";
    return result;
  }
  DBG("GEMINI", "connected, uploading...");

  String path = String("/v1beta/models/") + GEMINI_MODEL + ":generateContent?key=" + GEMINI_API_KEY;

  client.print(String("POST ") + path + " HTTP/1.1\r\n");
  client.print(String("Host: ") + GEMINI_HOST + "\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Content-Length: " + String(contentLength) + "\r\n");
  client.print("Connection: close\r\n\r\n");

  // Body: prefix, then streamed base64 audio, then suffix.
  // Streamed straight out of the PSRAM buffer in chunks rather than base64-
  // encoding the whole ~1.28MB clip into one String, which would risk heap
  // fragmentation/failure on top of everything else already in RAM.
  client.print(prefix);

  const size_t CHUNK = 3072;  // multiple of 3 for clean base64 grouping
  size_t offset = 0;
  while (offset < wavSize) {
    size_t n = min(CHUNK, wavSize - offset);
    String encoded = base64::encode(wavData + offset, n);
    client.print(encoded);
    offset += n;
  }

  client.print(suffix);
  DBG("GEMINI", "upload done, waiting for response...");

  // --- Read response ---
  unsigned long start = millis();
  while (client.connected() && !client.available()) {
    if (millis() - start > 15000) {
      DBG("GEMINI", "TIMEOUT waiting for response");
      client.stop();
      result.oledMessage = "GEMINI TIMEOUT";
      return result;
    }
  }

  // Skip HTTP headers, but note whether the body is chunked-encoded —
  // Google's API sends Transfer-Encoding: chunked, and a body read straight
  // off the socket after the headers would have hex chunk-size markers
  // embedded in it, which breaks JSON parsing (this was the PARSE ERROR).
  bool chunked = false;
  bool firstLine = true;
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (firstLine) {
      String s = line;
      s.trim();
      DBG("GEMINI", "HTTP status: %s", s.c_str());   // 404/400 -> check GEMINI_MODEL in config.h
      firstLine = false;
    }
    if (line.indexOf("Transfer-Encoding:") >= 0 && line.indexOf("chunked") >= 0) chunked = true;
    if (line == "\r") break;
  }

  String responseBody;
  if (chunked) {
    // De-chunk: each chunk is preceded by its size in hex on its own line,
    // followed by exactly that many bytes, then a trailing CRLF, repeating
    // until a zero-size chunk marks the end of the body.
    while (true) {
      String sizeLine = client.readStringUntil('\n');
      sizeLine.trim();
      if (sizeLine.length() == 0) continue;  // stray blank line between chunks
      long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
      if (chunkSize <= 0) break;  // 0-length chunk = end of body

      size_t received = 0;
      char readBuf[512];
      while (received < (size_t)chunkSize) {
        size_t toRead = min(sizeof(readBuf), (size_t)chunkSize - received);
        int n = client.readBytes(readBuf, toRead);
        if (n <= 0) break;  // connection dropped mid-chunk
        responseBody.concat(readBuf, n);
        received += n;
      }
      client.readStringUntil('\n');  // consume the CRLF that follows chunk data
    }
  } else {
    responseBody = client.readString();
  }
  client.stop();

  DBG("GEMINI", "response body: %u bytes (chunked=%d)", (unsigned)responseBody.length(), (int)chunked);

  // Gemini wraps the actual JSON text inside candidates[0].content.parts[0].text.
  JsonDocument outerDoc;
  DeserializationError err = deserializeJson(outerDoc, responseBody);
  if (err) {
    DBG("GEMINI", "outer JSON parse error: %s", err.c_str());
    DBG("GEMINI", "raw body: %s", responseBody.substring(0, 800).c_str());
    result.oledMessage = "PARSE ERROR";
    return result;
  }

  const char* innerText = outerDoc["candidates"][0]["content"]["parts"][0]["text"];
  if (!innerText) {
    DBG("GEMINI", "no candidates in reply. API said: %s", responseBody.substring(0, 800).c_str());
    result.oledMessage = "NO RESPONSE";
    return result;
  }
  DBG("GEMINI", "inner text: %s", innerText);

  JsonDocument innerDoc;
  err = deserializeJson(innerDoc, innerText);
  if (err) {
    DBG("GEMINI", "schema JSON parse error: %s", err.c_str());
    result.oledMessage = "SCHEMA PARSE ERR";
    return result;
  }

  result.success = true;
  result.parsedText = innerDoc["parsed_text"].as<String>();
  result.action = innerDoc["action"].as<String>();
  result.oledMessage = innerDoc["oled_message"].as<String>();

  String detailsOut;
  serializeJson(innerDoc["action_details"], detailsOut);
  result.actionDetailsJson = detailsOut;

  return result;
}

// Minimal JSON string escaper for embedding raw text into hand-built JSON
String jsonEscape(const String &input) {
  String out = "\"";
  for (size_t i = 0; i < input.length(); i++) {
    char c = input.charAt(i);
    if (c == '"' || c == '\\') out += '\\';
    if (c == '\n') { out += "\\n"; continue; }
    out += c;
  }
  out += "\"";
  return out;
}
