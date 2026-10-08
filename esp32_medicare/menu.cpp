#include "menu.h"
#include "config.h"
#include "oled_display.h"
#include "supabase_client.h"
#include <WiFi.h>

enum MenuScreen {
  SCREEN_MAIN,
  SCREEN_PILLS,
  SCREEN_SCHEDULE,
  SCREEN_REFILL_PICK,
  SCREEN_REFILL_EDIT,
  SCREEN_STATUS
};

static const char *MAIN_ITEMS[] = {
  "View pills",
  "Schedule",
  "Refill",
  "Status",
  "Refresh data"
};
static const int MAIN_ITEM_COUNT = sizeof(MAIN_ITEMS) / sizeof(MAIN_ITEMS[0]);

static Button *btnUp = nullptr;
static Button *btnDown = nullptr;
static Button *btnOk = nullptr;

static MenuScreen screen = SCREEN_MAIN;
static int selected = 0;      // highlighted index on the current screen
static int topIndex = 0;      // first visible row on the current screen
static bool dirty = true;     // needs redraw
static bool needsFetch = true;

static Compartment comps[MAX_COMPARTMENTS];
static int compCount = 0;
static bool fetchFailed = false;

// Refill editor state
static int refillIndex = 0;   // which compartment is being edited
static int refillValue = 0;

void menuInit(Button *up, Button *down, Button *ok) {
  btnUp = up;
  btnDown = down;
  btnOk = ok;
  screen = SCREEN_MAIN;
  selected = 0;
  topIndex = 0;
  dirty = true;
  needsFetch = true;
  DBG("MENU", "init");
}

void menuMarkDirty() { dirty = true; }
void menuRefreshData() { needsFetch = true; }

// Keeps `selected` on screen by sliding the visible window as needed.
static void clampScroll(int itemCount) {
  if (itemCount <= 0) { selected = 0; topIndex = 0; return; }
  if (selected < 0) selected = itemCount - 1;             // wrap to bottom
  if (selected >= itemCount) selected = 0;                // wrap to top
  if (selected < topIndex) topIndex = selected;
  if (selected >= topIndex + OLED_VISIBLE_ROWS) topIndex = selected - OLED_VISIBLE_ROWS + 1;
  if (topIndex < 0) topIndex = 0;
}

static const char *screenName(MenuScreen s) {
  switch (s) {
    case SCREEN_MAIN:        return "MAIN";
    case SCREEN_PILLS:       return "PILLS";
    case SCREEN_SCHEDULE:    return "SCHEDULE";
    case SCREEN_REFILL_PICK: return "REFILL_PICK";
    case SCREEN_REFILL_EDIT: return "REFILL_EDIT";
    case SCREEN_STATUS:      return "STATUS";
  }
  return "?";
}

static void goTo(MenuScreen s) {
  DBG("MENU", "screen %s -> %s", screenName(screen), screenName(s));
  screen = s;
  selected = 0;
  topIndex = 0;
  dirty = true;
  // Drop leftover button events so a stale long-press/click from the old
  // screen can't instantly fire on the new one.
  btnUp->flush(); btnDown->flush(); btnOk->flush();
}

static void doFetch() {
  DBG("MENU", "fetching compartments from Supabase...");
  showNotice("Loading...", "");
  unsigned long t = millis();
  compCount = fetchCompartments(comps, MAX_COMPARTMENTS);
  if (compCount < 0) {
    compCount = 0;
    fetchFailed = true;
    DBG("MENU", "fetch FAILED (%lu ms)", millis() - t);
  } else {
    fetchFailed = false;
    DBG("MENU", "fetch OK: %d compartments (%lu ms)", compCount, millis() - t);
    for (int i = 0; i < compCount; i++)
      DBG("MENU", "  slot %d: %s x%d", comps[i].id, comps[i].name.c_str(), comps[i].count);
  }
  needsFetch = false;
  dirty = true;
}

// ---- Row builders -------------------------------------------------------

static int buildPillRows(String *rows, int maxRows) {
  int n = 0;
  for (int i = 0; i < compCount && n < maxRows - 1; i++) {
    rows[n++] = String(comps[i].id) + " " + comps[i].name + " x" + String(comps[i].count);
  }
  rows[n++] = "< Back";
  return n;
}

static int buildScheduleRows(String *rows, int maxRows) {
  int n = 0;
  for (int i = 0; i < compCount && n < maxRows - 1; i++) {
    String when = comps[i].timeOfDay.length() ? comps[i].timeOfDay : String("--");
    String meal = comps[i].mealRelation.length() ? comps[i].mealRelation : String("");
    String line = comps[i].name + " " + when;
    if (meal.length()) line += "/" + meal;
    rows[n++] = line;
  }
  rows[n++] = "< Back";
  return n;
}

static int buildRefillRows(String *rows, int maxRows) {
  int n = 0;
  for (int i = 0; i < compCount && n < maxRows - 1; i++) {
    rows[n++] = String(comps[i].id) + " " + comps[i].name + " x" + String(comps[i].count);
  }
  rows[n++] = "< Back";
  return n;
}

static int buildStatusRows(String *rows, int maxRows) {
  int n = 0;
  rows[n++] = WiFi.status() == WL_CONNECTED ? "WiFi: OK" : "WiFi: DOWN";
  rows[n++] = "IP " + WiFi.localIP().toString();
  rows[n++] = "Slots: " + String(compCount);
  rows[n++] = fetchFailed ? "DB: error" : "DB: OK";
  rows[n++] = "Free: " + String(ESP.getFreeHeap() / 1024) + "KB";
  rows[n++] = "< Back";
  return n;
}

// ---- Screen handlers ----------------------------------------------------

static void handleMain() {
  if (btnUp->repeated())   { selected--; clampScroll(MAIN_ITEM_COUNT); dirty = true;
                             DBG("MENU", "main: UP -> '%s'", MAIN_ITEMS[selected]); }
  if (btnDown->repeated()) { selected++; clampScroll(MAIN_ITEM_COUNT); dirty = true;
                             DBG("MENU", "main: DOWN -> '%s'", MAIN_ITEMS[selected]); }

  if (btnOk->clicked()) {
    DBG("MENU", "main: OK on '%s'", MAIN_ITEMS[selected]);
    switch (selected) {
      case 0: goTo(SCREEN_PILLS); break;
      case 1: goTo(SCREEN_SCHEDULE); break;
      case 2: goTo(SCREEN_REFILL_PICK); break;
      case 3: goTo(SCREEN_STATUS); break;
      case 4: needsFetch = true; dirty = true; break;
    }
    return;
  }

  if (dirty) {
    DBG("MENU", "redraw main (sel=%d, top=%d)", selected, topIndex);
    String rows[MAIN_ITEM_COUNT];
    for (int i = 0; i < MAIN_ITEM_COUNT; i++) rows[i] = MAIN_ITEMS[i];
    showScrollList("MediCare", rows, MAIN_ITEM_COUNT, topIndex, selected);
    dirty = false;
  }
}

// Shared behaviour for the read-only list screens: scroll, and treat
// both the "< Back" row and a long-press of OK as "return to main".
static void handleList(const String &title, int (*builder)(String*, int)) {
  String rows[MAX_COMPARTMENTS + 6];
  int count = builder(rows, MAX_COMPARTMENTS + 6);

  if (btnUp->repeated())   { selected--; clampScroll(count); dirty = true;
                             DBG("MENU", "%s: UP -> row %d '%s'", title.c_str(), selected, rows[selected].c_str()); }
  if (btnDown->repeated()) { selected++; clampScroll(count); dirty = true;
                             DBG("MENU", "%s: DOWN -> row %d '%s'", title.c_str(), selected, rows[selected].c_str()); }

  if (btnOk->longPressed()) {
    DBG("MENU", "%s: OK long-press -> back to main", title.c_str());
    goTo(SCREEN_MAIN);
    return;
  }

  if (btnOk->clicked()) {
    DBG("MENU", "%s: OK on row %d '%s'", title.c_str(), selected, rows[selected].c_str());
    if (selected == count - 1) { goTo(SCREEN_MAIN); return; }   // "< Back"

    if (screen == SCREEN_REFILL_PICK && selected < compCount) {
      refillIndex = selected;
      refillValue = comps[selected].count;
      DBG("MENU", "refill: editing slot %d (%s), current=%d",
          comps[selected].id, comps[selected].name.c_str(), refillValue);
      goTo(SCREEN_REFILL_EDIT);
      return;
    }
  }

  if (dirty) {
    DBG("MENU", "redraw '%s' (%d rows, sel=%d, top=%d)", title.c_str(), count, selected, topIndex);
    showScrollList(title, rows, count, topIndex, selected);
    dirty = false;
  }
}

static void handleRefillEdit() {
  if (btnUp->repeated())   { refillValue++; dirty = true; }
  if (btnDown->repeated()) { refillValue--; dirty = true; }
  if (refillValue < 0)   refillValue = 0;
  if (refillValue > 999) refillValue = 999;

  if (btnOk->longPressed()) {
    DBG("MENU", "refill: cancelled");
    goTo(SCREEN_REFILL_PICK);   // cancel
    return;
  }

  if (btnOk->clicked()) {
    DBG("MENU", "refill: saving slot %d -> %d", comps[refillIndex].id, refillValue);
    showNotice("Saving...", comps[refillIndex].name);
    bool ok = updateCompartmentCount(comps[refillIndex].id, refillValue);
    DBG("MENU", "refill: save %s", ok ? "OK" : "FAILED");
    if (ok) {
      comps[refillIndex].count = refillValue;   // keep the cache in step
      showNotice("Saved", comps[refillIndex].name + " x" + String(refillValue));
    } else {
      showNotice("Save FAILED", "check wifi/db");
    }
    delay(1200);
    goTo(SCREEN_REFILL_PICK);
    return;
  }

  if (dirty) {
    DBG("MENU", "refill: value = %d", refillValue);
    showValueEditor("Refill", comps[refillIndex].name, refillValue);
    dirty = false;
  }
}

// ---- Entry point --------------------------------------------------------

void menuUpdate() {
  if (!oledReady()) {   // headless: nothing to drive
    static bool warned = false;
    if (!warned) { DBG("MENU", "menuUpdate() skipped - OLED not initialised (headless)"); warned = true; }
    return;
  }

  if (needsFetch) {
    doFetch();
    // A refresh can shrink the list out from under the cursor.
    selected = 0;
    topIndex = 0;
  }

  switch (screen) {
    case SCREEN_MAIN:         handleMain(); break;
    case SCREEN_PILLS:        handleList("Pill counts", buildPillRows); break;
    case SCREEN_SCHEDULE:     handleList("Schedule", buildScheduleRows); break;
    case SCREEN_REFILL_PICK:  handleList("Refill: pick", buildRefillRows); break;
    case SCREEN_REFILL_EDIT:  handleRefillEdit(); break;
    case SCREEN_STATUS:       handleList("Status", buildStatusRows); break;
  }
}
