#pragma once
#include <Arduino.h>
#include "buttons.h"

// The OLED menu. Owns its own screen state and the cached compartment list.
// Call menuInit() once from setup(), then menuUpdate() every loop() while the
// system isn't busy recording or talking to Gemini.
void menuInit(Button *up, Button *down, Button *ok);

// Handles button input and redraws when something changed. Does nothing
// (beyond one debug line) if the OLED failed to initialise.
void menuUpdate();

// Forces a redraw on the next menuUpdate() - call after the voice pipeline
// has taken over the screen, so the menu repaints itself.
void menuMarkDirty();

// Re-fetches compartments from Supabase on the next menuUpdate().
void menuRefreshData();
