#include "audio_capture.h"
#include "config.h"
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include "oled_display.h"

#define I2S_PORT             I2S_NUM_0
#define I2S_SAMPLE_RATE      16000
#define I2S_READ_BUF_SAMPLES 512   // kept small so recordAudioChunk() never blocks loop() for long

// Hard cap on clip length so the PSRAM buffer size is known up front.
// 16000 Hz * 2 bytes/sample * 30s = 960,000 bytes of PCM.
#define MAX_RECORDING_SECONDS 30
#define WAV_HEADER_SIZE       44
#define MAX_PCM_BYTES         (I2S_SAMPLE_RATE * 2 * MAX_RECORDING_SECONDS)
#define MAX_BUFFER_BYTES      (WAV_HEADER_SIZE + MAX_PCM_BYTES)

static uint8_t *audioBuf = nullptr;   // PSRAM-backed, allocated once in initAudioBuffer()
static size_t pcmBytesWritten = 0;    // bytes written after the header, this clip
static bool recording = false;
static bool bufferFull = false;       // set once MAX_PCM_BYTES is hit; stops silently
static int peakSample = 0;            // loudest |sample| seen this clip (debug: dead/clipping mic)

static void writeWavHeaderPlaceholder(uint8_t *hdr) {
  memset(hdr, 0, WAV_HEADER_SIZE);
  memcpy(hdr + 0,  "RIFF", 4);
  memcpy(hdr + 8,  "WAVE", 4);
  memcpy(hdr + 12, "fmt ", 4);

  uint32_t fmtChunkSize = 16;
  memcpy(hdr + 16, &fmtChunkSize, 4);

  uint16_t audioFormat = 1; // PCM
  memcpy(hdr + 20, &audioFormat, 2);

  uint16_t numChannels = 1;
  memcpy(hdr + 22, &numChannels, 2);

  uint32_t sampleRate = I2S_SAMPLE_RATE;
  memcpy(hdr + 24, &sampleRate, 4);

  uint16_t bitsPerSample = 16;
  uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
  memcpy(hdr + 28, &byteRate, 4);

  uint16_t blockAlign = numChannels * (bitsPerSample / 8);
  memcpy(hdr + 32, &blockAlign, 2);
  memcpy(hdr + 34, &bitsPerSample, 2);

  memcpy(hdr + 36, "data", 4);
}

void initI2SMic() {
  i2s_config_t i2sConfig = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = I2S_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,  // INMP441 sends 24-bit data left-justified in a 32-bit frame
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,   // L/R pin tied to GND -> left channel only
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = I2S_READ_BUF_SAMPLES,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pinConfig = {
    .bck_io_num = I2S_BCLK_PIN,
    .ws_io_num = I2S_LRCLK_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_DIN_PIN
  };

  i2s_driver_install(I2S_PORT, &i2sConfig, 0, NULL);
  i2s_set_pin(I2S_PORT, &pinConfig);
  i2s_zero_dma_buffer(I2S_PORT);
}

void initAudioBuffer() {
  // MALLOC_CAP_SPIRAM: force this into PSRAM, not the small internal heap —
  // ~960KB won't fit in SRAM alongside everything else (WiFi/TLS buffers etc).
  audioBuf = (uint8_t*)heap_caps_malloc(MAX_BUFFER_BYTES, MALLOC_CAP_SPIRAM);
  if (!audioBuf) {
    DBG("AUDIO", "PSRAM alloc FAILED - set Tools > PSRAM > OPI PSRAM");
    showNotice("PSRAM FAIL", "Tools>PSRAM>OPI");
    while (true) delay(1000);
  }
  DBG("AUDIO", "PSRAM buffer allocated: %u bytes", (unsigned)MAX_BUFFER_BYTES);
  writeWavHeaderPlaceholder(audioBuf);
}

void startRecording() {
  pcmBytesWritten = 0;
  bufferFull = false;
  recording = true;
  peakSample = 0;
  DBG("AUDIO", "recording started");

  // Drop anything the mic buffered before PTT was pressed so the clip
  // starts clean instead of with a stale first chunk.
  i2s_zero_dma_buffer(I2S_PORT);
}

void recordAudioChunk() {
  if (!recording) return;

  static int32_t rawBuf[I2S_READ_BUF_SAMPLES];
  static int16_t pcmBuf[I2S_READ_BUF_SAMPLES];

  size_t bytesRead = 0;
  // 0 ticks_to_wait: return immediately with whatever's available rather
  // than blocking loop() waiting for a full buffer.
  esp_err_t err = i2s_read(I2S_PORT, rawBuf, sizeof(rawBuf), &bytesRead, 0);
  if (err != ESP_OK || bytesRead == 0) return;

  if (bufferFull) return;  // keep draining the DMA buffer, but stop appending

  size_t samplesRead = bytesRead / sizeof(int32_t);
  for (size_t i = 0; i < samplesRead; i++) {
    // INMP441 data occupies the upper bits of the 32-bit frame. >>14 is a
    // commonly-used shift to land in a usable 16-bit range — if recordings
    // come out too quiet, clipped, or noisy, this is the first thing to
    // retune (try >>11 for louder, >>16 for quieter).
    int16_t s = (int16_t)(rawBuf[i] >> 14);
    pcmBuf[i] = s;
    int a = abs((int)s);
    if (a > peakSample) peakSample = a;
  }

  size_t pcmBytes = samplesRead * sizeof(int16_t);

  if (pcmBytesWritten + pcmBytes > MAX_PCM_BYTES) {
    // Clip hit MAX_RECORDING_SECONDS - stop appending but don't overrun the buffer.
    pcmBytes = MAX_PCM_BYTES - pcmBytesWritten;
    bufferFull = true;
    DBG("AUDIO", "%d s cap reached - further audio dropped", MAX_RECORDING_SECONDS);
  }

  memcpy(audioBuf + WAV_HEADER_SIZE + pcmBytesWritten, pcmBuf, pcmBytes);
  pcmBytesWritten += pcmBytes;
}

void stopRecording() {
  if (!recording) return;
  recording = false;

  uint32_t dataSize = pcmBytesWritten;
  uint32_t riffSize = 36 + dataSize;

  memcpy(audioBuf + 4,  &riffSize, 4);
  memcpy(audioBuf + 40, &dataSize, 4);

  float secs = pcmBytesWritten / (float)(I2S_SAMPLE_RATE * 2);
  DBG("AUDIO", "stopped: %u PCM bytes = %.2f s, peak=%d/32767", (unsigned)pcmBytesWritten, secs, peakSample);
  if (peakSample < 200) DBG("AUDIO", "peak very low - mic silent? check wiring, L/R->GND, the >>14 shift");
}

const uint8_t* getRecordingBuffer() {
  return audioBuf;
}

size_t getRecordingSize() {
  return WAV_HEADER_SIZE + pcmBytesWritten;
}
