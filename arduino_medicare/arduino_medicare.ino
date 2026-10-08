#include <Servo.h>
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>   // Library Manager: "DFRobotDFPlayerMini"

// ---------- Wiring ----------
// ESP32 GPIO17 (TX) -> Uno pin 0 (RX), common GND.
//   Unplug the wire on pin 0 while uploading.
//   One-way link: Uno TX (pin 1) is NOT connected to the ESP32.
//
// DFPlayer Mini (same wiring as the DFPlayer test sketch):
//   Uno pin 5 (TX) -> 1k resistor -> DFPlayer RX
//   Uno pin 4 (RX) <- DFPlayer TX     (needed: the library reads the DFPlayer's replies)
//   DFPlayer VCC -> 5V, GND -> GND (common with everything else)
//   Speaker on SPK1/SPK2 (or DAC_R/DAC_L -> amplifier)
//   SD card (FAT32): voice files in a folder named MP3 as 0001.mp3, 0002.mp3 ...
//   The numbers must match the AUDIO_*_TRACK values in the ESP32 config.h.
const uint8_t DF_RX_PIN = 4;     // Arduino receives from DFPlayer TX
const uint8_t DF_TX_PIN = 5;     // Arduino sends to DFPlayer RX
const uint8_t DF_VOLUME = 30;    // 0..30
SoftwareSerial dfSerial(DF_RX_PIN, DF_TX_PIN);
DFRobotDFPlayerMini dfPlayer;
bool dfReady = false;            // true once dfPlayer.begin() has succeeded

// ---------- Slots ----------
const uint8_t NUM_SLOTS = 2;
const uint8_t SLOT_IDS[NUM_SLOTS]   = {1, 2};    // compartment_id values from Supabase
const uint8_t SERVO_PINS[NUM_SLOTS] = {9, 10};   // one trapdoor servo per slot

// ---------- Servo timing (from your code) ----------
const int MIN_US = 544;
const int MAX_US = 2400;
// Per-slot angles. Slot 2 (pin 10) is mounted mirrored, so its angles are reversed.
//                                  slot 1 (pin 9), slot 2 (pin 10)
const int CLOSED_ANGLES[NUM_SLOTS] = {0, 90};
const int OPEN_ANGLES[NUM_SLOTS]   = {90, 0};

const unsigned long MOVE_MS = 6000;
const unsigned long HOLD_MS = 5000;
const unsigned long UPDATE_MS = 20;
const bool USE_EASING = true;

Servo trapdoors[NUM_SLOTS];
int currentUs[NUM_SLOTS];

// ---------- Pending dispense queue ----------
const uint8_t QUEUE_SIZE = 4;
int8_t queueSlots[QUEUE_SIZE];
uint8_t qHead = 0, qCount = 0;

// ---------- Pending audio queue ----------
// Tracks are played from loop(), between servo moves, so the DFPlayer
// commands never disturb a servo that is mid-motion.
const uint8_t AUDIO_QUEUE_SIZE = 4;
uint16_t audioQueue[AUDIO_QUEUE_SIZE];
uint8_t aHead = 0, aCount = 0;

// ---------- Serial line buffer ----------
char lineBuf[64];
uint8_t lineLen = 0;

int angleToUs(int angle) {
  return MIN_US + (long)angle * (MAX_US - MIN_US) / 180;
}

int slotIndexFromId(int id) {
  for (uint8_t i = 0; i < NUM_SLOTS; i++) {
    if (SLOT_IDS[i] == id) return i;
  }
  return -1;
}

void enqueue(int idx) {
  if (qCount >= QUEUE_SIZE) {
    Serial.println(F("Queue full, dropping request"));
    return;
  }
  queueSlots[(qHead + qCount) % QUEUE_SIZE] = idx;
  qCount++;
}

void enqueueAudio(uint16_t track) {
  if (aCount >= AUDIO_QUEUE_SIZE) {
    Serial.println(F("Audio queue full, dropping request"));
    return;
  }
  audioQueue[(aHead + aCount) % AUDIO_QUEUE_SIZE] = track;
  aCount++;
}

// ---------- DFPlayer Mini (DFRobotDFPlayerMini library, same calls as the test sketch) ----------
bool dfInit() {
  Serial.println(F("DFPlayer: connecting..."));
  if (!dfPlayer.begin(dfSerial)) {
    Serial.println(F("DFPlayer: NOT responding. Check 5V, GND, pin 5 -> 1k -> RX, pin 4 <- TX, FAT32 SD card with MP3 files."));
    return false;
  }
  dfPlayer.volume(DF_VOLUME);
  Serial.print(F("DFPlayer: OK, volume "));
  Serial.println(DF_VOLUME);
  return true;
}

// Prints DFPlayer errors and finished tracks. Called from loop() only,
// never while a servo is moving.
void dfEvents() {
  if (!dfPlayer.available()) return;

  uint8_t type = dfPlayer.readType();
  int value = dfPlayer.read();

  if (type == DFPlayerError) {
    Serial.print(F("DFPlayer ERROR: "));
    switch (value) {
      case Busy:          Serial.println(F("card not found / busy")); break;
      case Sleeping:      Serial.println(F("module sleeping")); break;
      case FileIndexOut:  Serial.println(F("file number out of range")); break;
      case FileMismatch:  Serial.println(F("file not found - check /MP3/NNNN.mp3")); break;
      default:            Serial.println(value); break;
    }
  } else if (type == DFPlayerPlayFinished) {
    Serial.print(F("DFPlayer: finished track "));
    Serial.println(value);
  }
}

void playNextAudio() {
  uint16_t track = audioQueue[aHead];
  aHead = (aHead + 1) % AUDIO_QUEUE_SIZE;
  aCount--;

  Serial.print(F("Playing track "));
  Serial.println(track);

  if (!dfReady) dfReady = dfInit();   // it was not up at boot: try again now
  dfPlayer.playMp3Folder(track);      // plays /MP3/NNNN.mp3
}

// ---------- Parse one complete line ----------
void handleLine(char *line) {
  Serial.print(F("RX: "));
  Serial.println(line);

  // PLAY|<label>|<track>  - meal reminder from the ESP32
  if (strncmp(line, "PLAY|", 5) == 0) {
    char *lastPipe = strrchr(line, '|');
    int track = (lastPipe && *(lastPipe + 1) != '\0') ? atoi(lastPipe + 1) : 0;
    if (track <= 0) {
      Serial.println(F("PLAY has no valid track number"));
      return;
    }
    Serial.print(F("Queued track "));
    Serial.println(track);
    enqueueAudio(track);
    return;
  }

  if (strncmp(line, "FETCH|", 6) != 0) {
    Serial.println(F("Not a FETCH/PLAY line, ignored"));
    return;
  }

  // Slot number is the text after the last '|'
  char *lastPipe = strrchr(line, '|');
  if (!lastPipe || *(lastPipe + 1) == '\0') {
    Serial.println(F("FETCH has no slot field"));
    return;
  }

  int slotId = atoi(lastPipe + 1);
  int idx = slotIndexFromId(slotId);
  if (idx < 0) {
    Serial.print(F("Unknown slot: "));
    Serial.println(slotId);
    return;
  }

  Serial.print(F("Queued slot "));
  Serial.println(slotId);
  enqueue(idx);
}

// Non-blocking: reads whatever bytes are waiting and assembles lines.
void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf[lineLen] = '\0';
      if (lineLen > 0) handleLine(lineBuf);
      lineLen = 0;
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    } else {
      lineLen = 0;   // overflow, discard the garbage line
    }
  }
}

// ---------- Servo motion ----------
void moveSmooth(uint8_t idx, int targetAngle, unsigned long durationMs) {
  int startUs  = currentUs[idx];
  int targetUs = angleToUs(targetAngle);

  unsigned long startTime = millis();
  unsigned long lastUpdate = 0;

  while (true) {
    unsigned long elapsed = millis() - startTime;
    if (elapsed >= durationMs) break;

    if (elapsed - lastUpdate >= UPDATE_MS) {
      lastUpdate = elapsed;
      float t = (float)elapsed / durationMs;
      if (USE_EASING) t = t * t * (3.0f - 2.0f * t);
      currentUs[idx] = startUs + (int)((targetUs - startUs) * t);
      trapdoors[idx].writeMicroseconds(currentUs[idx]);
    }
    pollSerial();   // keep collecting new commands while moving
  }

  currentUs[idx] = targetUs;
  trapdoors[idx].writeMicroseconds(currentUs[idx]);
}

void waitMs(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) pollSerial();
}

void dispenseSlot(uint8_t idx) {
  Serial.print(F("Dispensing slot "));
  Serial.println(SLOT_IDS[idx]);

  trapdoors[idx].attach(SERVO_PINS[idx], MIN_US, MAX_US);

  Serial.println(F("Opening (6 s)"));
  moveSmooth(idx, OPEN_ANGLES[idx], MOVE_MS);

  Serial.println(F("Holding 5 s"));
  waitMs(HOLD_MS);

  Serial.println(F("Closing (6 s)"));
  moveSmooth(idx, CLOSED_ANGLES[idx], MOVE_MS);

  waitMs(300);
  trapdoors[idx].detach();
  Serial.println(F("Servo off"));
}

void setup() {
  Serial.begin(9600);      // ESP32 link + USB monitor (must match UART_BAUD)

  // Park both servos at the closed position, then power them down
  for (uint8_t i = 0; i < NUM_SLOTS; i++) {
    currentUs[i] = angleToUs(CLOSED_ANGLES[i]);
    trapdoors[i].attach(SERVO_PINS[i], MIN_US, MAX_US);
    trapdoors[i].writeMicroseconds(currentUs[i]);
    delay(800);
    trapdoors[i].detach();
  }

  // DFPlayer: SoftwareSerial stays listening, the library reads its replies.
  dfSerial.begin(9600);
  delay(500);                       // the servo parking above already gave the DFPlayer ~1.6 s to boot
  dfReady = dfInit();

  Serial.println(F("Arduino ready, waiting for FETCH|msg|slot or PLAY|label|track"));
}

void loop() {
  pollSerial();
  dfEvents();

  if (aCount > 0) playNextAudio();

  if (qCount > 0) {
    uint8_t idx = queueSlots[qHead];
    qHead = (qHead + 1) % QUEUE_SIZE;
    qCount--;
    dispenseSlot(idx);
  }
}
