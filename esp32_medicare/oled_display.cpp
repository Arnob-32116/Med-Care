#include "oled_display.h"
#include "config.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

static Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
static bool displayOk = false;

static uint8_t scanI2C() {
  uint8_t oledAddr = 0;
  int found = 0;
  DBG("OLED", "scanning I2C (SDA=%d, SCL=%d)...", SDA_PIN, SCL_PIN);
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      DBG("OLED", "  device found at 0x%02X", addr);
      found++;
      if (!oledAddr && (addr == 0x3C || addr == 0x3D)) oledAddr = addr;
    }
  }
  if (found == 0) DBG("OLED", "  NOTHING on the bus - check wiring/power and SDA_PIN/SCL_PIN in config.h");
  return oledAddr;
}

bool initOledDisplay() {
  Wire.begin(SDA_PIN, SCL_PIN);

  uint8_t addr = scanI2C();
  if (!addr) {
    DBG("OLED", "no display at 0x3C/0x3D - running headless");
    displayOk = false;
    return false;
  }

  if (!display.begin(addr, true)) {
    DBG("OLED", "display.begin(0x%02X) FAILED - running headless", addr);
    displayOk = false;
    return false;
  }

  displayOk = true;
  display.setContrast(255);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.print("OLED OK 0x");
  display.print(addr, HEX);
  display.display();
  DBG("OLED", "init OK at 0x%02X", addr);
  return true;
}

bool oledReady() { return displayOk; }

void showOledMessage(const String &message) {
  if (!displayOk) return;
  DBG("OLED", "message: %s", message.c_str());

  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SH110X_WHITE);

  // Word-wrap so a long oled_message doesn't run off the edge.
  // At text size 2 this screen fits ~10 characters per line.
  const int charsPerLine = 10;
  String remaining = message;
  int row = 0;
  while (remaining.length() > 0 && row < 4) {
    String line = remaining.substring(0, min((int)remaining.length(), charsPerLine));
    display.setCursor(0, row * 16);
    display.println(line);
    remaining = remaining.length() > charsPerLine ? remaining.substring(charsPerLine) : "";
    row++;
  }

  display.display();
}

static void drawTitleBar(const String &title) {
  // Inverted title bar so the current screen is obvious at a glance.
  display.fillRect(0, 0, SCREEN_WIDTH, 8, SH110X_WHITE);
  display.setTextSize(1);
  display.setTextColor(SH110X_BLACK);
  display.setCursor(1, 0);
  display.print(title.substring(0, OLED_CHARS_PER_LINE));
  display.setTextColor(SH110X_WHITE);
}

void showScrollList(const String &title, const String items[], int itemCount,
                    int topIndex, int selected) {
  if (!displayOk) return;
  DBG("OLED", "list '%s' (%d items, top=%d, sel=%d)", title.c_str(), itemCount, topIndex, selected);

  display.clearDisplay();
  drawTitleBar(title);

  if (itemCount == 0) {
    display.setCursor(0, 16);
    display.print("(empty)");
    display.display();
    return;
  }

  for (int row = 0; row < OLED_VISIBLE_ROWS; row++) {
    int idx = topIndex + row;
    if (idx >= itemCount) break;

    int y = 8 + row * 8;
    display.setCursor(0, y);
    display.print(idx == selected ? ">" : " ");
    // -2 leaves room for the cursor column and avoids wrapping onto the next row
    display.print(items[idx].substring(0, OLED_CHARS_PER_LINE - 2));
  }

  // Scroll indicators so it's clear there's more above/below.
  if (topIndex > 0) {
    display.setCursor(SCREEN_WIDTH - 6, 8);
    display.print("^");
  }
  if (topIndex + OLED_VISIBLE_ROWS < itemCount) {
    display.setCursor(SCREEN_WIDTH - 6, SCREEN_HEIGHT - 8);
    display.print("v");
  }

  display.display();
}

void showValueEditor(const String &title, const String &label, int value) {
  if (!displayOk) return;
  DBG("OLED", "editor '%s' %s = %d", title.c_str(), label.c_str(), value);

  display.clearDisplay();
  drawTitleBar(title);

  display.setTextSize(1);
  display.setCursor(0, 14);
  display.print(label.substring(0, OLED_CHARS_PER_LINE));

  display.setTextSize(3);
  display.setCursor(40, 28);
  display.print(value);

  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print("UP/DN  OK=save");

  display.display();
}

void showNotice(const String &line1, const String &line2) {
  if (!displayOk) return;
  DBG("OLED", "notice: '%s' / '%s'", line1.c_str(), line2.c_str());

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 24);
  display.println(line1.substring(0, OLED_CHARS_PER_LINE));
  if (line2.length()) {
    display.setCursor(0, 36);
    display.println(line2.substring(0, OLED_CHARS_PER_LINE));
  }
  display.display();
}
