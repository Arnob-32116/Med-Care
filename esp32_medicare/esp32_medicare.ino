#include "config.h"
#include "audio_capture.h"
#include "gemini_client.h"
#include "supabase_client.h"
#include "uart_protocol.h"
#include "oled_display.h"
#include "buttons.h"
#include "menu.h"
#include "pill_logic.h"
#include "time_client.h"
#include "meal_reminder.h"
#include <WiFi.h>
#include <ArduinoJson.h>

// Ignore clips shorter than this - a bounce or accidental tap produces a
// near-empty buffer that Gemini can't do anything useful with, and there's
// no point burning an API call on it.
#define MIN_RECORDING_MS 400

// ---- How long voice results stay on the OLED ------------------------------
// Lists (pill counts, low stock, meal pills) stay up until this many ms after
// your LAST UP/DOWN press; every press restarts the timer. OK closes early.
// >>> Raise this to keep results on screen longer. <<<
#define RESULT_HOLD_MS  10000
// One-shot messages ("Added 5", "Stock OK", errors).
#define NOTICE_HOLD_MS  3000

enum SystemState { STATE_MENU, STATE_RECORDING, STATE_PROCESSING };
SystemState currentState = STATE_MENU;

Button btnPtt, btnUp, btnDown, btnOk;
unsigned long recordingStartMs = 0;

void processVoiceCommand();

void setup() {
  Serial.begin(115200);
  delay(2000);
  DBG("BOOT", "=== MediCare Companion starting ===");
  DBG("BOOT", "heap=%lu  psram=%lu", (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());

  initUartToArduino();
  DBG("BOOT", "UART1 ready (TX only): %d baud, TX=%d", UART_BAUD, UART_TX_PIN);

  btnPtt.begin(PTT_BUTTON_PIN, "PTT");
  btnUp.begin(BTN_UP_PIN, "UP");
  btnDown.begin(BTN_DOWN_PIN, "DOWN");
  btnOk.begin(BTN_OK_PIN, "OK");

  bool oled = initOledDisplay();
  DBG("BOOT", "OLED %s", oled ? "ready" : "NOT ready (headless)");
  showNotice("Booting...", "OLED ok");
  delay(500);

  showNotice("WiFi...", WIFI_SSID);
  DBG("BOOT", "connecting to WiFi '%s'", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(500);
    // 1=SSID not found (5GHz-only / out of range), 4=connect failed, 6=wrong password/disconnected
    DBG("BOOT", "  waiting for WiFi, status=%d", (int)WiFi.status());
  }
  if (WiFi.status() == WL_CONNECTED) {
    DBG("BOOT", "WiFi connected, IP=%s, RSSI=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    DBG("BOOT", "WiFi FAILED - ESP32 is 2.4 GHz only. Continuing offline so the menu still shows.");
    showNotice("WiFi FAILED", "need 2.4GHz");
    delay(1500);
  }

  showNotice("Time sync...", "");
  if (WiFi.status() == WL_CONNECTED) timeUpdate();   // first sync; later ones run from loop()

  showNotice("Audio...", "");
  initAudioBuffer();
  initI2SMic();
  DBG("BOOT", "audio ready, psram free=%lu", (unsigned long)ESP.getFreePsram());

  menuInit(&btnUp, &btnDown, &btnOk);
  DBG("BOOT", "READY - hold PTT to speak, UP/DOWN/OK for menu");
}

// Back to the menu after the voice pipeline (or a rejected clip) took over
// the screen. Flushes button events so nothing pressed meanwhile fires late.
static void returnToMenu() {
  btnUp.flush(); btnDown.flush(); btnOk.flush();
  menuMarkDirty();
  currentState = STATE_MENU;
  DBG("STATE", "-> MENU");
}

void loop() {
  btnPtt.update();
  btnUp.update();
  btnDown.update();
  btnOk.update();

  // Heartbeat: if this stops printing, loop() is blocked somewhere.
  static unsigned long lastBeat = 0;
  if (millis() - lastBeat > 5000) {
    lastBeat = millis();
    DBG("LOOP", "alive | state=%d | heap=%lu | wifi=%d",
        (int)currentState, (unsigned long)ESP.getFreeHeap(), (int)WiFi.status());
  }

  switch (currentState) {
    case STATE_MENU:
      // PTT takes priority over whatever the menu is showing.
      if (btnPtt.isDown()) {
        DBG("STATE", "MENU -> RECORDING (PTT held)");
        startRecording();
        recordingStartMs = millis();
        showOledMessage("REC...");
        currentState = STATE_RECORDING;
        break;
      }
      timeUpdate();            // re-syncs the clock every TIME_SYNC_INTERVAL_MIN minutes
      mealReminderUpdate();    // plays meal reminders and the 30-minute follow-ups
      menuUpdate();
      break;

    case STATE_RECORDING:
      recordAudioChunk();
      if (!btnPtt.isDown()) {
        stopRecording();
        unsigned long held = millis() - recordingStartMs;
        DBG("STATE", "PTT released after %lu ms, clip = %u bytes", held, (unsigned)getRecordingSize());

        if (held < MIN_RECORDING_MS || getRecordingSize() <= 44) {
          DBG("STATE", "clip too short - ignoring");
          showNotice("Too short", "");
          delay(800);
          returnToMenu();
          break;
        }

        showOledMessage("THINKING");
        DBG("STATE", "RECORDING -> PROCESSING");
        currentState = STATE_PROCESSING;
      }
      break;

    case STATE_PROCESSING:
      processVoiceCommand();
      returnToMenu();
      break;
  }
}

// ===========================================================================
// Voice results shown on the OLED
// ===========================================================================

// Shows a scrollable list and waits: UP/DOWN scroll (and restart the timer),
// OK closes it, and it closes by itself RESULT_HOLD_MS after the last press.
static void showResultScreen(const String &title, const String rows[], int n) {
  DBG("RESULT", "'%s' (%d rows):", title.c_str(), n);
  for (int i = 0; i < n; i++) DBG("RESULT", "  %s", rows[i].c_str());

  if (!oledReady()) return;   // headless: nothing to wait for

  btnUp.flush(); btnDown.flush(); btnOk.flush();
  int top = 0;
  bool redraw = true;
  unsigned long lastActivity = millis();

  while (millis() - lastActivity < RESULT_HOLD_MS) {
    btnUp.update(); btnDown.update(); btnOk.update();

    if (btnUp.repeated())   { if (top > 0) top--;                                  redraw = true; lastActivity = millis(); }
    if (btnDown.repeated()) { if (top + OLED_VISIBLE_ROWS < n) top++;              redraw = true; lastActivity = millis(); }

    if (btnOk.clicked() || btnOk.longPressed()) {
      DBG("RESULT", "closed by OK");
      return;
    }

    if (redraw) {
      showScrollList(title, rows, n, top, -1);   // -1 = no cursor
      redraw = false;
    }
    delay(5);
  }
  DBG("RESULT", "closed after %d ms idle", RESULT_HOLD_MS);
}

// Renders the compartment list straight from the Gemini context blob, used
// for the view_pill_count action (the menu has its own richer version).
static void showPillCountsFromContext(const String &contextJson) {
  JsonDocument doc;
  if (deserializeJson(doc, contextJson)) {
    DBG("VOICE", "pill counts: context JSON unreadable");
    showNotice("Read error", "");
    delay(NOTICE_HOLD_MS);
    return;
  }

  JsonArray compartments = doc["active_compartments"];
  if (compartments.isNull() || compartments.size() == 0) {
    DBG("VOICE", "pill counts: no compartments");
    showNotice("No compartments", "");
    delay(NOTICE_HOLD_MS);
    return;
  }

  String rows[MAX_COMPARTMENTS];
  int n = 0;
  for (JsonObject c : compartments) {
    if (n >= MAX_COMPARTMENTS) break;
    rows[n++] = String(c["compartment_id"].as<int>()) + " " +
                c["medicine_name"].as<String>() + " x" +
                String(c["current_count"].as<int>());
  }
  showResultScreen("Pill counts", rows, n);
}

// ===========================================================================
// Voice actions handled entirely on the ESP32 (never sent to the Arduino)
// ===========================================================================

static const char *MEAL_NAMES[] = { "breakfast", "lunch", "dinner" };   // = morning / noon / night slot

static int mealSlotFromName(const char *name) {
  String m = name;
  m.toLowerCase();
  for (int i = 0; i < 3; i++) if (m == MEAL_NAMES[i]) return i;
  return -1;
}

// "Which pills won't last through today?" Whole-day maths, no time of day asked.
static void handleLowStockToday() {
  Compartment comps[MAX_COMPARTMENTS];
  int n = fetchCompartments(comps, MAX_COMPARTMENTS);
  if (n < 0) {
    DBG("VOICE", "low stock: DB fetch failed");
    showNotice("DB error", "check wifi");
    delay(NOTICE_HOLD_MS);
    return;
  }

  int idx[MAX_COMPARTMENTS];
  int m = findLowStockToday(comps, n, idx);
  DBG("VOICE", "low stock today: %d of %d compartments", m, n);

  // Voice answer first, so it starts while the OLED screen is still up.
  // The Arduino's DFPlayer plays whichever track number it is sent.
  int answerTrack = (m == 0) ? AUDIO_STOCK_OK_TRACK : AUDIO_BUY_PILLS_TRACK;
  sendUartPlayTrack(answerTrack, m == 0 ? "stock ok" : "buy pills today");

  if (m == 0) {
    showNotice("Stock OK", "Enough for today");
    delay(NOTICE_HOLD_MS);
    return;
  }

  String rows[MAX_COMPARTMENTS * 2];
  int r = 0;
  for (int k = 0; k < m; k++) {
    const Compartment &c = comps[idx[k]];
    DailyDoses d = parseDailyDoses(c.timeOfDay);
    rows[r++] = String(c.id) + " " + c.name;
    rows[r++] = "  have " + String(c.count) + " need " + formatQty(d.total);
  }
  showResultScreen("Finishing today", rows, r);
}

// "I ate lunch / dinner, what pills should I take?"
static void handleMealPills(const String &detailsJson) {
  JsonDocument d;
  if (deserializeJson(d, detailsJson)) {
    DBG("VOICE", "meal pills: bad details JSON");
    showNotice("Not understood", "try again");
    delay(NOTICE_HOLD_MS);
    return;
  }

  const char *mealStr  = d["meal"] | "";
  const char *stateStr = d["meal_state"] | "after";
  int slot = mealSlotFromName(mealStr);
  bool after = String(stateStr) != "before";
  DBG("VOICE", "meal pills: meal='%s' (slot %d) state='%s'", mealStr, slot, stateStr);

  if (slot < 0) {
    showNotice("Which meal?", "breakfast/lunch/dinner");
    delay(NOTICE_HOLD_MS);
    return;
  }

  Compartment comps[MAX_COMPARTMENTS];
  int n = fetchCompartments(comps, MAX_COMPARTMENTS);
  if (n < 0) {
    DBG("VOICE", "meal pills: DB fetch failed");
    showNotice("DB error", "check wifi");
    delay(NOTICE_HOLD_MS);
    return;
  }

  int idx[MAX_COMPARTMENTS];
  int skipped = 0;
  int m = findMealPills(comps, n, slot, after, idx, &skipped);
  DBG("VOICE", "meal pills: %d to take, %d skipped (opposite meal relation)", m, skipped);

  if (m == 0 && skipped == 0) {
    showNotice("Nothing due", String("for ") + MEAL_NAMES[slot]);
    delay(NOTICE_HOLD_MS);
    return;
  }

  String rows[MAX_COMPARTMENTS * 2 + 2];
  int r = 0;
  if (m == 0) rows[r++] = "None to take now";
  for (int k = 0; k < m; k++) {
    const Compartment &c = comps[idx[k]];
    DailyDoses dd = parseDailyDoses(c.timeOfDay);
    rows[r++] = String(c.id) + " " + c.name;
    rows[r++] = "  take " + formatQty(dd.slot[slot]) + (c.count <= 0 ? " OUT!" : "");
  }
  if (skipped > 0) rows[r++] = "+" + String(skipped) + (after ? " before-meal" : " after-meal");

  String title = String(after ? "After " : "Before ") + MEAL_NAMES[slot];
  showResultScreen(title, rows, r);
}

// "Add 5 pills to <medicine> / compartment 3"
static void handleAddStock(const String &detailsJson) {
  JsonDocument d;
  if (deserializeJson(d, detailsJson)) {
    DBG("VOICE", "add stock: bad details JSON");
    showNotice("Not understood", "try again");
    delay(NOTICE_HOLD_MS);
    return;
  }

  int qty = d["quantity"] | 0;
  const char *medName = d["medicine_name"] | "";
  JsonArray ids = d["compartment_ids"];
  DBG("VOICE", "add stock: qty=%d medicine='%s' ids=%u", qty, medName, (unsigned)ids.size());

  if (qty <= 0) {
    showNotice("How many pills?", "say a number");
    delay(NOTICE_HOLD_MS);
    return;
  }

  Compartment comps[MAX_COMPARTMENTS];
  int n = fetchCompartments(comps, MAX_COMPARTMENTS);
  if (n < 0) {
    DBG("VOICE", "add stock: DB fetch failed");
    showNotice("DB error", "check wifi");
    delay(NOTICE_HOLD_MS);
    return;
  }

  // Which compartment(s) did they mean? Slot numbers first, medicine name as fallback.
  bool hit[MAX_COMPARTMENTS] = { false };
  for (JsonVariant v : ids) {
    int id = v.as<int>();
    for (int i = 0; i < n; i++) if (comps[i].id == id) hit[i] = true;
  }

  String want = medName;
  want.toLowerCase();
  bool anyHit = false;
  for (int i = 0; i < n; i++) if (hit[i]) anyHit = true;

  if (!anyHit && want.length() > 0) {
    for (int i = 0; i < n; i++) {                       // exact name match
      String have = comps[i].name;
      have.toLowerCase();
      if (have == want) { hit[i] = true; anyHit = true; }
    }
    if (!anyHit) {                                      // loose match (either contains the other)
      for (int i = 0; i < n; i++) {
        String have = comps[i].name;
        have.toLowerCase();
        if (have.length() > 0 && (have.indexOf(want) >= 0 || want.indexOf(have) >= 0)) { hit[i] = true; anyHit = true; }
      }
    }
  }

  int hits = 0, target = -1;
  for (int i = 0; i < n; i++) if (hit[i]) { hits++; target = i; }
  DBG("VOICE", "add stock: %d matching compartment(s)", hits);

  if (hits == 0) {
    showNotice("No such pill", want.length() ? want : String("say name or slot"));
    delay(NOTICE_HOLD_MS);
    return;
  }
  if (hits > 1) {
    // Same medicine in several slots (or several ids returned) - don't guess which to top up.
    showNotice("Which slot?", "say slot number");
    delay(NOTICE_HOLD_MS);
    return;
  }

  int newCount = comps[target].count + qty;
  if (newCount > 999) newCount = 999;
  DBG("VOICE", "add stock: slot %d (%s) %d -> %d", comps[target].id, comps[target].name.c_str(),
      comps[target].count, newCount);

  if (updateCompartmentCount(comps[target].id, newCount)) {
    showNotice(comps[target].name + " +" + String(qty), "now " + String(newCount) + " pills");
  } else {
    showNotice("Save FAILED", "check wifi/db");
  }
  delay(NOTICE_HOLD_MS);
}

// ===========================================================================
// Voice pipeline
// ===========================================================================

void processVoiceCommand() {
  unsigned long t0 = millis();

  DBG("VOICE", "1/4 fetching Supabase context...");
  String contextJson = fetchSupabaseContext();
  DBG("VOICE", "    context: %u bytes (%lu ms)", (unsigned)contextJson.length(), millis() - t0);
  DBG("VOICE", "    %s", contextJson.c_str());

  DBG("VOICE", "2/4 calling Gemini with %u byte clip...", (unsigned)getRecordingSize());
  unsigned long t1 = millis();
  GeminiResult result = callGemini(getRecordingBuffer(), getRecordingSize(), contextJson);
  DBG("VOICE", "    Gemini returned in %lu ms", millis() - t1);
  DBG("VOICE", "    success=%d text='%s' action=%s", result.success, result.parsedText.c_str(), result.action.c_str());
  DBG("VOICE", "    details=%s oled='%s'", result.actionDetailsJson.c_str(), result.oledMessage.c_str());

  if (!result.success) {
    // Network/parse failure: nothing worth logging to the DB, but the Arduino
    // still gets a NONE line so it lands in a known-safe state.
    DBG("VOICE", "FAILED (%s) - sending NONE to Arduino", result.oledMessage.c_str());
    showOledMessage(result.oledMessage);
    sendUartResult(result);
    delay(2000);
    return;
  }

  DBG("VOICE", "3/4 logging to voice_commands...");
  bool logged = insertVoiceCommandRow(result.parsedText, result.action, result.actionDetailsJson);
  DBG("VOICE", "    insert %s", logged ? "OK" : "FAILED (continuing anyway)");

  String actionUpper = result.action;
  actionUpper.toUpperCase();

  DBG("VOICE", "4/4 routing action '%s'", result.action.c_str());
  if (actionUpper == "VIEW_PILL_COUNT") {
    // Display-only - the OLED lives on the ESP32, so this never reaches the Arduino.
    DBG("VOICE", "    -> pill counts on OLED (not sent to Arduino)");
    showPillCountsFromContext(contextJson);   // stays up per RESULT_HOLD_MS
  } else if (actionUpper == "LOW_STOCK_TODAY") {
    DBG("VOICE", "    -> low-stock-today list on OLED + voice answer via PLAY to Arduino");
    handleLowStockToday();
  } else if (actionUpper == "MEAL_PILLS") {
    DBG("VOICE", "    -> meal pills on OLED (not sent to Arduino)");
    handleMealPills(result.actionDetailsJson);
  } else if (actionUpper == "ADD_STOCK") {
    DBG("VOICE", "    -> add stock in Supabase (not sent to Arduino)");
    handleAddStock(result.actionDetailsJson);
  } else {
    // FETCH_PRESCRIPTION -> dispense, UPDATE_SCHEDULE -> RTC table update,
    // NONE -> no-op. All three are the Arduino's business.
    DBG("VOICE", "    -> OLED message + UART to Arduino");
    showOledMessage(result.oledMessage);
    sendUartResult(result);
    delay(2000);
  }

  // A dispense, schedule change or restock makes the cached menu data stale.
  if (actionUpper == "FETCH_PRESCRIPTION" || actionUpper == "UPDATE_SCHEDULE" || actionUpper == "ADD_STOCK") {
    DBG("VOICE", "    menu data marked stale, will refetch");
    menuRefreshData();
  }
  DBG("VOICE", "done in %lu ms total", millis() - t0);
}
