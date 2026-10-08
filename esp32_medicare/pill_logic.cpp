#include "pill_logic.h"
#include <math.h>

// "1" -> 1, "1/2" -> 0.5, "1 1/2" -> 1.5, "" -> 0
static float parseQty(String s) {
  s.trim();
  if (s.length() == 0) return 0;
  int slash = s.indexOf('/');
  if (slash < 0) return s.toFloat();

  int sp = s.indexOf(' ');
  if (sp > 0 && sp < slash) return s.substring(0, sp).toFloat() + parseQty(s.substring(sp + 1));

  float num = s.substring(0, slash).toFloat();
  float den = s.substring(slash + 1).toFloat();
  return den > 0 ? num / den : 0;
}

DailyDoses parseDailyDoses(const String &timeOfDay) {
  DailyDoses d;
  d.valid = false;
  d.slot[0] = d.slot[1] = d.slot[2] = 0;
  d.total = 0;

  int p1 = timeOfDay.indexOf('+');
  int p2 = (p1 < 0) ? -1 : timeOfDay.indexOf('+', p1 + 1);
  if (p1 < 0 || p2 < 0) return d;

  d.slot[0] = parseQty(timeOfDay.substring(0, p1));
  d.slot[1] = parseQty(timeOfDay.substring(p1 + 1, p2));
  d.slot[2] = parseQty(timeOfDay.substring(p2 + 1));
  for (int i = 0; i < 3; i++) if (d.slot[i] < 0) d.slot[i] = 0;

  d.total = d.slot[0] + d.slot[1] + d.slot[2];
  d.valid = true;
  return d;
}

MealRelation parseMealRelation(const String &mealRelation) {
  String r = mealRelation;
  r.toLowerCase();
  if (r.indexOf("before") >= 0) return REL_BEFORE;
  if (r.indexOf("after") >= 0)  return REL_AFTER;
  return REL_UNSPECIFIED;
}

String formatQty(float q) {
  int whole = (int)q;
  float frac = q - whole;
  if (frac < 0.01f) return String(whole);
  if (fabsf(frac - 0.5f) < 0.01f) return whole > 0 ? String(whole) + " 1/2" : String("1/2");
  return String(q, 2);
}

int findLowStockToday(const Compartment *c, int n, int *outIdx) {
  int m = 0;
  for (int i = 0; i < n; i++) {
    DailyDoses d = parseDailyDoses(c[i].timeOfDay);
    if (!d.valid || d.total <= 0) continue;
    // <= : stock exactly equal to the day's need is finished by tonight.
    // Change to < if you only want to list pills that fall short.
    if ((float)c[i].count <= d.total + 0.001f) outIdx[m++] = i;
  }
  return m;
}

int findMealPills(const Compartment *c, int n, int slot, bool after,
                  int *outIdx, int *skippedOpposite) {
  int m = 0;
  int skipped = 0;
  if (slot < 0 || slot > 2) { if (skippedOpposite) *skippedOpposite = 0; return 0; }

  for (int i = 0; i < n; i++) {
    DailyDoses d = parseDailyDoses(c[i].timeOfDay);
    if (!d.valid || d.slot[slot] <= 0) continue;

    MealRelation rel = parseMealRelation(c[i].mealRelation);
    bool wanted = (rel == REL_UNSPECIFIED) || (after ? rel == REL_AFTER : rel == REL_BEFORE);
    if (wanted) outIdx[m++] = i;
    else        skipped++;
  }
  if (skippedOpposite) *skippedOpposite = skipped;
  return m;
}
