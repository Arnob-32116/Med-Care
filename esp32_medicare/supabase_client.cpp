#include "supabase_client.h"
#include "config.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

String httpGet(const String &url) {
  WiFiClientSecure client;
  client.setInsecure();   // fine for a class project; use a root cert for production

  HTTPClient http;
  http.begin(client, url);
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", "Bearer " SUPABASE_ANON_KEY);

  unsigned long t = millis();
  int code = http.GET();
  const char *path = url.c_str() + strlen(SUPABASE_URL);   // log the path only, not the host
  String payload = "";
  if (code == 200) {
    payload = http.getString();
    DBG("SUPA", "GET %s -> 200 (%u B, %lu ms)", path, (unsigned)payload.length(), millis() - t);
  } else {
    DBG("SUPA", "GET %s FAILED: %d %s (%lu ms)", path, code,
        code < 0 ? HTTPClient::errorToString(code).c_str() : http.getString().c_str(), millis() - t);
  }
  http.end();
  return payload;
}

String fetchSupabaseContext() {
  // active_compartments: medicine_name not null. current_count is read-only
  // in the voice path — the Arduino decrements it when a dose is dispensed,
  // and the menu's refill screen is the only place the ESP32 writes it.
  String compartmentsUrl = String(SUPABASE_URL) +
    "/rest/v1/compartments?select=compartment_id,medicine_name,current_count,updated_at&medicine_name=not.is.null";
  String compartmentsJson = httpGet(compartmentsUrl);

  // last 5 voice_commands, most recent first
  String voiceUrl = String(SUPABASE_URL) +
    "/rest/v1/voice_commands?select=parsed_text,action,action_details,created_at&order=created_at.desc&limit=5";
  String voiceJson = httpGet(voiceUrl);

  // Parse each response into its own document first, then assign into the
  // combined one. (ArduinoJson v7 removed the v6 shortcut of deserializing
  // straight into doc["key"].to<JsonArray>() as the destination.)
  JsonDocument compartmentsDoc;
  JsonDocument voiceDoc;
  DeserializationError e1 = deserializeJson(compartmentsDoc, compartmentsJson);
  DeserializationError e2 = deserializeJson(voiceDoc, voiceJson);

  if (e1 || e2) {
    DBG("SUPA", "context parse failed (compartments: %s, voice: %s)", e1.c_str(), e2.c_str());
    return "{}";
  }

  JsonDocument doc;
  doc["active_compartments"] = compartmentsDoc;
  doc["recent_commands"] = voiceDoc;

  String output;
  serializeJson(doc, output);
  return output;
}

int fetchCompartments(Compartment *out, int maxItems) {
  // Ordered by compartment_id so the menu list is stable between refreshes -
  // without this, rows can come back in a different order and the item you
  // had highlighted would shift under you.
  String url = String(SUPABASE_URL) +
    "/rest/v1/compartments?select=compartment_id,medicine_name,current_count,dosage,time_of_day,meal_relation"
    "&medicine_name=not.is.null&order=compartment_id.asc";

  String json = httpGet(url);
  if (json.length() == 0) return -1;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    DBG("SUPA", "compartments parse failed: %s", err.c_str());
    return -1;
  }

  JsonArray arr = doc.as<JsonArray>();
  int n = 0;
  for (JsonObject o : arr) {
    if (n >= maxItems) break;
    out[n].id           = o["compartment_id"] | 0;
    out[n].name         = o["medicine_name"].isNull()  ? String("?") : o["medicine_name"].as<String>();
    out[n].count        = o["current_count"] | 0;
    out[n].dosage       = o["dosage"].isNull()         ? String("")  : o["dosage"].as<String>();
    out[n].timeOfDay    = o["time_of_day"].isNull()    ? String("")  : o["time_of_day"].as<String>();
    out[n].mealRelation = o["meal_relation"].isNull()  ? String("")  : o["meal_relation"].as<String>();
    n++;
  }
  return n;
}

bool updateCompartmentCount(int compartmentId, int newCount) {
  JsonDocument doc;
  doc["current_count"] = newCount;
  String body;
  serializeJson(doc, body);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = String(SUPABASE_URL) +
    "/rest/v1/compartments?compartment_id=eq." + String(compartmentId);

  http.begin(client, url);
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", "Bearer " SUPABASE_ANON_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");

  // HTTPClient has no PATCH() convenience method - sendRequest is the way.
  int code = http.sendRequest("PATCH", body);
  DBG("SUPA", "PATCH compartment %d -> count %d : HTTP %d", compartmentId, newCount, code);
  http.end();

  // PostgREST returns 204 No Content for a successful return=minimal update.
  return (code == 204 || code == 200);
}

bool insertVoiceCommandRow(const String &parsedText, const String &action, const String &actionDetailsJson) {
  JsonDocument doc;
  doc["parsed_text"] = parsedText;
  doc["action"] = action;

  // action_details is jsonb — parse it back into an object so it isn't double-encoded as a string
  JsonDocument detailsDoc;
  if (actionDetailsJson.length() > 0) {
    deserializeJson(detailsDoc, actionDetailsJson);
    doc["action_details"] = detailsDoc.as<JsonObject>();
  }

  String body;
  serializeJson(doc, body);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/voice_commands";
  http.begin(client, url);
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", "Bearer " SUPABASE_ANON_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");

  int code = http.POST(body);
  DBG("SUPA", "POST voice_commands -> HTTP %d %s", code, code != 201 ? http.getString().c_str() : "");
  http.end();

  return code == 201;
}
