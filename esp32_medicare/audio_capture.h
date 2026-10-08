#pragma once
#include <Arduino.h>

// Installs the I2S driver for the INMP441 mic. Call once from setup().
void initI2SMic();

// Allocates the PCM buffer in PSRAM (once, from setup()) and pre-pends a
// placeholder WAV header. Halts with a Serial message if PSRAM allocation
// fails — call this once at boot, not per-recording.
void initAudioBuffer();

// Resets the write position back to just after the WAV header and clears
// the "buffer full" flag. Call once when the PTT button is first pressed.
void startRecording();

// Reads whatever audio is currently sitting in the I2S DMA buffer and
// appends it to the in-RAM PCM buffer. Non-blocking (0 timeout) — call
// this repeatedly from loop() while STATE_RECORDING is active. Silently
// stops appending (keeps the mic drained but drops samples) once
// MAX_RECORDING_SECONDS is reached.
void recordAudioChunk();

// Patches the in-RAM WAV header with the real RIFF/data chunk sizes now
// that the final byte count is known. No file I/O — the buffer is ready
// to hand to callGemini() immediately after this returns.
void stopRecording();

// Full WAV clip (44-byte header + PCM), still sitting in PSRAM.
const uint8_t* getRecordingBuffer();
size_t getRecordingSize();
