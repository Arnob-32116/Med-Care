#pragma once
#include <Arduino.h>
#include "supabase_client.h"   // Compartment, MAX_COMPARTMENTS

// Daily doses parsed from compartments.time_of_day, stored as
// "morning+noon+night" - e.g. "1+0+1", "0+1+1", "0+0+1/2".
struct DailyDoses {
  bool  valid;      // false if the string isn't in the a+b+c format
  float slot[3];    // [0]=morning (breakfast), [1]=noon (lunch), [2]=night (dinner)
  float total;      // slot[0] + slot[1] + slot[2] = pills needed for a whole day
};

DailyDoses parseDailyDoses(const String &timeOfDay);

enum MealRelation { REL_UNSPECIFIED, REL_BEFORE, REL_AFTER };
MealRelation parseMealRelation(const String &mealRelation);

// 1 -> "1", 0.5 -> "1/2", 1.5 -> "1 1/2", anything else -> decimal.
String formatQty(float q);

// Compartments whose stock can't get through a whole day: current_count <= the
// day's total need (so "3 pills, need 0+1+1=2" is fine, "2 pills, need 2" or
// fewer is listed). Compartments with no valid schedule are skipped.
// Writes indices into c[] to outIdx (room for MAX_COMPARTMENTS), returns how many.
int findLowStockToday(const Compartment *c, int n, int *outIdx);

// Pills to take around a meal. slot: 0=breakfast, 1=lunch, 2=dinner.
//   after=true  -> "I ate": after_meal + unspecified pills with a dose in that slot
//   after=false -> "I'm about to eat": before_meal + unspecified pills
// *skippedOpposite counts pills due in that slot but with the opposite relation.
int findMealPills(const Compartment *c, int n, int slot, bool after,
                  int *outIdx, int *skippedOpposite);
