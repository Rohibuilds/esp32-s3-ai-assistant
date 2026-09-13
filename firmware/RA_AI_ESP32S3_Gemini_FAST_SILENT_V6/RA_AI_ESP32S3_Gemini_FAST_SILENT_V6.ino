/*
  RA AI - ESP32-S3 FREE GEMINI VOICE ASSISTANT
  ============================================

  Free-tier architecture:
    ESP32-S3 + INMP441 -> Gemini multimodal audio understanding
    Gemini transcribes audio strictly, then a separate Gemini text call answers
    Gemini TTS -> streamed 24 kHz PCM -> MAX98357A speaker

  Hardware:
    ESP32-S3 DevKit
    INMP441 I2S microphone
    MAX98357A I2S amplifier
    3W/5W speaker
    SSD1306 128x64 I2C OLED
    WS2812 / NeoPixel
    Push button

  Libraries:
    - Adafruit GFX Library
    - Adafruit SSD1306
    - Adafruit NeoPixel
    - ArduinoJson 7.x

  Board package:
    - esp32 by Espressif Systems 3.x

  IMPORTANT:
    1. Create a Gemini API key in Google AI Studio.
    2. Put your Wi-Fi details and GEMINI_API_KEY below.
    3. Do not publish firmware containing your real API key.
    4. This sketch uses Google Gemini services; a valid account and quota are required. Free-tier quotas
       and model availability can change over time.

  Tested architecture / API assumptions:
    - Gemini Interactions API
    - Audio input sent inline as base64 WAV
    - Text/audio AI model: gemini-3.5-flash-lite (Fast Silent V6)
    - TTS model: gemini-3.1-flash-tts-preview
    - TTS stream returns 24 kHz 16-bit mono PCM audio chunks
*/

#include <Arduino.h>
#include <time.h>
#include "assistant_types.h"
#include "google_roots.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif
#include <math.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <ESP_I2S.h>
#include "mbedtls/base64.h"

// ============================================================
// USER SETTINGS
// ============================================================




// Create this at Google AI Studio.
// Example only: "AIza...."


// Current Gemini models used by this free build.
const char* GEMINI_AI_MODEL  = "gemini-3.5-flash-lite";
const char* GEMINI_TTS_MODEL = "gemini-3.1-flash-tts-preview";

// Gemini TTS voice.
// Other examples: Puck, Kore, Charon, Achird, Sulafat, Aoede.
const char* GEMINI_VOICE = "Kore";

// Recording time after button press.
// Fast capture window. 2500 ms keeps short voice commands responsive.
const uint32_t RECORD_MS = 2500;

// INMP441 software gain.
// If audio is too quiet: try 10-14.
// If distorted/clipping: try 3-6.
// INMP441 L/R is wired to GND, so use the LEFT I2S slot.
// Audio level is normalized automatically after recording.
const bool MIC_USE_RIGHT_CHANNEL = false;

// ============================================================
// PIN MAP
// ============================================================

// OLED
#define OLED_SDA   8
#define OLED_SCL   9

// Shared I2S clocks
#define I2S_BCLK   14
#define I2S_WS     15

// INMP441 data -> ESP32-S3
#define MIC_SD     16

// ESP32-S3 -> MAX98357A DIN
#define SPK_DIN    17

// Optional but HIGHLY RECOMMENDED:
// MAX98357A SD / EN / SD_MODE -> GPIO5.
// LOW = amplifier shutdown/mute.
// HIGH = amplifier enabled.
// If your breakout has no SD/EN pin, leave GPIO5 unconnected;
// the firmware still forces SPK_DIN LOW during I2S switching.
#define AMP_SD_PIN 5

// WS2812
#define LED_PIN    18
#define LED_COUNT  1

// Push button: GPIO4 -> button -> GND
#define BUTTON_PIN 4

// ============================================================
// DISPLAY / AUDIO
// ============================================================

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDR     0x3C

const uint32_t MIC_SAMPLE_RATE = 16000;
const uint32_t TTS_SAMPLE_RATE = 24000;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_NeoPixel pixel(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
I2SClass I2S;

// ============================================================
// ASSISTANT STATE
// ============================================================



AssistantState currentState = STATE_BOOT;

// Keep a little conversation context.
const int HISTORY_SIZE = 3;
String historyUser[HISTORY_SIZE];
String historyAI[HISTORY_SIZE];
int historyCount = 0;

// ============================================================
// SMALL DATA STRUCTURE
// ============================================================



// Last microphone diagnostics.
uint32_t lastMicRms = 0;
uint16_t lastMicPeak = 0;
float lastMicClipPercent = 0.0f;

// ============================================================
// LED + OLED UI
// ============================================================

void setPixel(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

void oledCenterText(const String& text, int y, int textSize = 1) {
  display.setTextSize(textSize);
  display.setTextColor(SSD1306_WHITE);

  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);

  int x = max(0, (SCREEN_WIDTH - (int)w) / 2);
  display.setCursor(x, y);
  display.print(text);
}

void drawEyes(bool blink = false, int lookX = 0) {
  if (blink) {
    display.fillRoundRect(24, 27, 28, 5, 2, SSD1306_WHITE);
    display.fillRoundRect(76, 27, 28, 5, 2, SSD1306_WHITE);
    return;
  }

  display.drawRoundRect(23, 15, 30, 30, 9, SSD1306_WHITE);
  display.drawRoundRect(75, 15, 30, 30, 9, SSD1306_WHITE);

  display.fillCircle(38 + lookX, 30, 6, SSD1306_WHITE);
  display.fillCircle(90 + lookX, 30, 6, SSD1306_WHITE);
}

void drawBootScreen() {
  currentState = STATE_BOOT;

  display.clearDisplay();
  oledCenterText("RA", 7, 3);
  oledCenterText("AI ASSISTANT", 42, 1);
  display.display();

  setPixel(25, 0, 45);
}

void drawConnecting() {
  currentState = STATE_CONNECTING;

  display.clearDisplay();
  oledCenterText("RA AI", 7, 2);
  oledCenterText("CONNECTING", 35, 1);
  oledCenterText("Wi-Fi...", 49, 1);
  display.display();

  setPixel(8, 8, 25);
}

void drawReady(bool blink = false) {
  currentState = STATE_READY;

  display.clearDisplay();
  drawEyes(blink);

  if (!blink) {
    oledCenterText("PRESS BUTTON", 53, 1);
  }

  display.display();
  setPixel(0, 28, 5);
}

void drawListening(uint8_t level = 0) {
  currentState = STATE_LISTENING;

  display.clearDisplay();
  drawEyes(false);

  int bars = map(level, 0, 255, 0, 8);

  for (int i = 0; i < 8; i++) {
    int h = (i < bars) ? (4 + (i % 4) * 2) : 2;
    int x = 42 + i * 6;
    display.fillRect(x, 50 - h, 3, h, SSD1306_WHITE);
  }

  oledCenterText("LISTENING...", 54, 1);
  display.display();

  setPixel(0, 0, 60);
}

void drawThinking(uint8_t phase = 0) {
  currentState = STATE_THINKING;

  display.clearDisplay();

  int look = 0;
  if (phase % 3 == 0) look = -4;
  if (phase % 3 == 2) look = 4;

  drawEyes(false, look);
  oledCenterText("THINKING...", 53, 1);
  display.display();

  setPixel(38, 0, 45);
}

void drawSpeaking(uint8_t level = 80) {
  currentState = STATE_SPEAKING;

  display.clearDisplay();
  drawEyes(false);

  int mouthW = map(level, 0, 255, 12, 38);
  int mouthH = map(level, 0, 255, 3, 10);

  int x = (SCREEN_WIDTH - mouthW) / 2;
  display.drawRoundRect(x, 47, mouthW, mouthH, 3, SSD1306_WHITE);

  oledCenterText("SPEAKING", 57, 1);
  display.display();

  uint8_t glow = constrain(map(level, 0, 255, 10, 55), 10, 55);
  setPixel(0, glow, glow / 3);
}

void drawError(const String& message) {
  currentState = STATE_ERROR;

  display.clearDisplay();
  oledCenterText("ERROR", 5, 2);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 31);

  String msg = message;
  if (msg.length() > 52) {
    msg = msg.substring(0, 52);
  }

  display.println(msg);
  display.display();

  setPixel(60, 0, 0);
}

// ============================================================
// WIFI
// ============================================================

bool syncClock() {
  if (time(nullptr) > 1700000000) return true;
  configTime(0, 0, "time.google.com", "pool.ntp.org");
  uint32_t start = millis();
  while (time(nullptr) < 1700000000 && millis()-start < 15000) delay(50);
  if (time(nullptr) < 1700000000) {
    drawError("Clock sync failed");
    return false;
  }
  return true;
}

bool connectWiFi() {
  drawConnecting();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  uint8_t dots = 0;

  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    dots++;

    display.clearDisplay();
    oledCenterText("RA AI", 5, 2);
    oledCenterText("CONNECTING WIFI", 31, 1);

    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(50, 48);

    for (int i = 0; i < dots % 4; i++) {
      display.print(".");
    }

    display.display();

    if (millis() - start > 20000) {
      drawError("Wi-Fi connection failed");
      return false;
    }
  }

  Serial.print("Wi-Fi connected. IP: ");
  Serial.println(WiFi.localIP());

  return true;
}

bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return syncClock();
  }

  return connectWiFi() && syncClock();
}

// ============================================================
// MAX98357A POP / BURST PROTECTION
// ============================================================

void forceSpeakerDataLow() {
  // Detach from any previous peripheral state and hold DIN at digital zero.
  pinMode(SPK_DIN, OUTPUT);
  digitalWrite(SPK_DIN, LOW);
}

void muteAmplifier() {
  // SD_MODE LOW places MAX98357A in shutdown.
  digitalWrite(AMP_SD_PIN, LOW);
  forceSpeakerDataLow();
}

void enableAmplifier() {
  digitalWrite(AMP_SD_PIN, HIGH);
}

bool audioWriteFailed = false;
bool writeAudioAll(uint8_t* data, size_t bytes) {
  size_t offset = 0;
  uint32_t progress = millis();
  while (offset < bytes) {
    size_t n = I2S.write(data + offset, bytes - offset);
    if (n) { offset += n; progress = millis(); }
    else if (millis()-progress > 3000) {
      digitalWrite(AMP_SD_PIN, LOW);
      audioWriteFailed = true;
      return false;
    } else delay(1);
  }
  return true;
}

bool writeSpeakerSilence(uint32_t durationMs) {
  int16_t silence[192] = {};
  uint32_t remaining = TTS_SAMPLE_RATE * durationMs / 1000;
  while (remaining) {
    uint32_t frames = min((uint32_t)96, remaining);
    if (!writeAudioAll((uint8_t*)silence, frames*4)) return false;
    remaining -= frames;
  }
  return true;
}

// ============================================================
// I2S
// ============================================================

bool beginMicrophone() {
  // Mute BEFORE touching the shared I2S clocks.
  digitalWrite(AMP_SD_PIN, LOW);

  I2S.end();

  // Once I2S TX is detached, force the amp data pin to a defined LOW.
  forceSpeakerDataLow();
  delay(8);

  // BCLK, WS, DOUT, DIN, MCLK
  I2S.setPins(I2S_BCLK, I2S_WS, -1, MIC_SD, -1);

  bool ok = I2S.begin(
    I2S_MODE_STD,
    MIC_SAMPLE_RATE,
    I2S_DATA_BIT_WIDTH_32BIT,
    I2S_SLOT_MODE_STEREO,
    I2S_STD_SLOT_BOTH
  );

  if (!ok) {
    Serial.println("ERROR: microphone I2S init failed.");
  }

  // Amplifier intentionally remains muted throughout recording.
  return ok;
}

bool beginSpeaker() {
  // Keep amplifier physically muted while changing I2S mode.
  digitalWrite(AMP_SD_PIN, LOW);

  I2S.end();
  forceSpeakerDataLow();
  delay(8);

  // BCLK, WS, DOUT, DIN, MCLK
  I2S.setPins(I2S_BCLK, I2S_WS, SPK_DIN, -1, -1);

  bool ok = I2S.begin(
    I2S_MODE_STD,
    TTS_SAMPLE_RATE,
    I2S_DATA_BIT_WIDTH_16BIT,
    I2S_SLOT_MODE_STEREO,
    I2S_STD_SLOT_BOTH
  );

  if (!ok) {
    Serial.println("ERROR: speaker I2S init failed.");
    muteAmplifier();
    return false;
  }

  // Establish stable clocks + all-zero PCM BEFORE enabling the amp.
  audioWriteFailed = false;
  if (!writeSpeakerSilence(25)) {
    digitalWrite(AMP_SD_PIN, LOW); I2S.end(); forceSpeakerDataLow();
    return false;
  }
  enableAmplifier();
  delay(4);

  return true;
}

// ============================================================
// WAV HELPERS
// ============================================================

void writeLE16(uint8_t* p, uint16_t value) {
  p[0] = value & 0xFF;
  p[1] = (value >> 8) & 0xFF;
}

void writeLE32(uint8_t* p, uint32_t value) {
  p[0] = value & 0xFF;
  p[1] = (value >> 8) & 0xFF;
  p[2] = (value >> 16) & 0xFF;
  p[3] = (value >> 24) & 0xFF;
}

void writeWavHeader(uint8_t* wav, uint32_t dataBytes) {
  memcpy(wav + 0, "RIFF", 4);
  writeLE32(wav + 4, 36 + dataBytes);

  memcpy(wav + 8, "WAVE", 4);

  memcpy(wav + 12, "fmt ", 4);
  writeLE32(wav + 16, 16);
  writeLE16(wav + 20, 1);              // PCM
  writeLE16(wav + 22, 1);              // mono
  writeLE32(wav + 24, MIC_SAMPLE_RATE);
  writeLE32(wav + 28, MIC_SAMPLE_RATE * 2);
  writeLE16(wav + 32, 2);
  writeLE16(wav + 34, 16);

  memcpy(wav + 36, "data", 4);
  writeLE32(wav + 40, dataBytes);
}

uint8_t* allocateLargeBuffer(size_t bytes) {
  uint8_t* p = nullptr;

  if (psramFound()) {
    p = (uint8_t*)ps_malloc(bytes);

    if (p) {
      Serial.printf("Allocated %u bytes in PSRAM.\n", (unsigned)bytes);
      return p;
    }
  }

  p = (uint8_t*)malloc(bytes);

  if (p) {
    Serial.printf("Allocated %u bytes in internal RAM.\n", (unsigned)bytes);
  }

  return p;
}

// ============================================================
// RECORD INMP441 -> WAV
// ============================================================

uint8_t* recordVoice(size_t& wavSize) {
  const size_t totalSamples = (MIC_SAMPLE_RATE * RECORD_MS) / 1000UL;
  const size_t dataBytes = totalSamples * sizeof(int16_t);

  wavSize = 44 + dataBytes;

  uint8_t* wav = allocateLargeBuffer(wavSize);

  if (!wav) {
    Serial.println("ERROR: not enough RAM for recording.");
    return nullptr;
  }

  writeWavHeader(wav, dataBytes);

  if (!beginMicrophone()) {
    free(wav);
    return nullptr;
  }

  delay(300);

  drawListening(20);

  Serial.println();
  Serial.println("=== SPEAK NOW ===");
  Serial.println("Recording clean fixed-channel INMP441 audio...");
  Serial.print("MIC I2S CHANNEL: ");
  Serial.println(MIC_USE_RIGHT_CHANNEL ? "RIGHT" : "LEFT");

  int16_t* pcm = (int16_t*)(wav + 44);
  size_t captured = 0;

  // ESP32 standard stereo RX returns L,R,L,R...
  int32_t raw[256];

  uint32_t lastUi = 0;
  uint16_t peakUi = 0;

  // First pass stats.
  int64_t dcAccumulator = 0;
  uint16_t rawPeak = 0;

  const uint32_t captureStart = millis();
  while (captured < totalSamples) {
    if (millis() - captureStart > RECORD_MS + 5000) {
      Serial.println("Microphone timeout: check clocks and data pin.");
      digitalWrite(AMP_SD_PIN, LOW);
      I2S.end(); forceSpeakerDataLow(); free(wav);
      return nullptr;
    }
    size_t gotBytes = I2S.readBytes((char*)raw, sizeof(raw));

    if (gotBytes == 0) {
      delay(1);
      continue;
    }

    size_t words = gotBytes / sizeof(int32_t);

    for (size_t i = 0; i + 1 < words && captured < totalSamples; i += 2) {
      // INMP441 is 24-bit two's-complement I2S in a 32-bit slot.
      int32_t sample32 =
        MIC_USE_RIGHT_CHANNEL
          ? raw[i + 1]
          : raw[i];

      // Keep the upper 16 useful bits for a standard 16-bit WAV.
      int32_t sample = sample32 >> 16;

      if (sample > 32767) sample = 32767;
      if (sample < -32768) sample = -32768;

      int16_t out = (int16_t)sample;
      pcm[captured++] = out;

      dcAccumulator += out;

      int32_t a = out < 0 ? -(int32_t)out : (int32_t)out;

      if (a > rawPeak) {
        rawPeak = (uint16_t)a;
      }

      if (a > peakUi) {
        peakUi = (uint16_t)a;
      }
    }

    if (millis() - lastUi > 80) {
      // Raw INMP441 upper-16 values are usually small, so use a sensitive meter.
      uint8_t ui = constrain(
        map(peakUi, 0, 5000, 0, 255),
        0,
        255
      );

      drawListening(ui);
      peakUi = 0;
      lastUi = millis();
    }
  }

  // Keep the amplifier muted before tearing down microphone I2S.
  digitalWrite(AMP_SD_PIN, LOW);
  I2S.end();
  forceSpeakerDataLow();

  // ----------------------------------------------------------
  // CLEANUP PASS:
  // 1) Remove DC offset.
  // 2) Find peak after DC removal.
  // 3) Normalize to a safe ~15000 peak.
  // ----------------------------------------------------------

  int32_t dcOffset =
    (int32_t)(dcAccumulator / (int64_t)totalSamples);

  uint16_t centeredPeak = 0;

  for (size_t i = 0; i < totalSamples; i++) {
    int32_t centered = (int32_t)pcm[i] - dcOffset;
    int32_t a = centered < 0 ? -centered : centered;

    if (a > centeredPeak) {
      centeredPeak = (uint16_t)min(a, (int32_t)32767);
    }
  }

  // Target about -6.8 dBFS. Cap gain so silence/noise is never boosted absurdly.
  float normalizeGain = 1.0f;

  if (centeredPeak > 0) {
    normalizeGain = 15000.0f / (float)centeredPeak;
  }

  if (normalizeGain > 8.0f) normalizeGain = 8.0f;
  if (normalizeGain < 0.25f) normalizeGain = 0.25f;

  uint64_t sumSquares = 0;
  uint16_t finalPeak = 0;
  uint32_t clippedSamples = 0;

  for (size_t i = 0; i < totalSamples; i++) {
    int32_t centered = (int32_t)pcm[i] - dcOffset;
    int32_t scaled = (int32_t)((float)centered * normalizeGain);

    if (scaled > 32767) {
      scaled = 32767;
      clippedSamples++;
    } else if (scaled < -32768) {
      scaled = -32768;
      clippedSamples++;
    }

    int16_t out = (int16_t)scaled;
    pcm[i] = out;

    int32_t a = out < 0 ? -(int32_t)out : (int32_t)out;

    if (a > finalPeak) {
      finalPeak = (uint16_t)a;
    }

    sumSquares +=
      (uint64_t)((int32_t)out * (int32_t)out);
  }

  double meanSquare =
    (double)sumSquares / (double)totalSamples;

  lastMicRms = (uint32_t)sqrt(meanSquare);
  lastMicPeak = finalPeak;
  lastMicClipPercent =
    (100.0f * (float)clippedSamples) /
    (float)totalSamples;

  Serial.print("Recorded WAV bytes: ");
  Serial.println(wavSize);

  Serial.print("RAW PEAK: ");
  Serial.println(rawPeak);

  Serial.print("DC OFFSET: ");
  Serial.println(dcOffset);

  Serial.print("CENTERED PEAK: ");
  Serial.println(centeredPeak);

  Serial.print("NORMALIZE GAIN: ");
  Serial.println(normalizeGain, 2);

  Serial.print("MIC RMS: ");
  Serial.println(lastMicRms);

  Serial.print("MIC PEAK: ");
  Serial.println(lastMicPeak);

  Serial.print("MIC CLIPPING: ");
  Serial.print(lastMicClipPercent, 2);
  Serial.println("%");

  return wav;
}

// ============================================================
// HTTP HELPERS
// ============================================================

bool waitForClientData(WiFiClientSecure& client, uint32_t timeoutMs) {
  uint32_t start = millis();

  while (!client.available()) {
    if (!client.connected()) {
      return false;
    }

    if (millis() - start > timeoutMs) {
      return false;
    }

    delay(1);
  }

  return true;
}

bool writeAll(
  WiFiClientSecure& client,
  const uint8_t* data,
  size_t length
) {
  size_t sent = 0;
  uint32_t lastProgress = millis();

  while (sent < length) {
    size_t n = client.write(data + sent, length - sent);

    if (n > 0) {
      sent += n;
      lastProgress = millis();
    } else {
      if (!client.connected()) {
        return false;
      }

      if (millis() - lastProgress > 15000) {
        return false;
      }

      delay(1);
    }
  }

  return true;
}

bool writeString(WiFiClientSecure& client, const String& value) {
  return writeAll(
    client,
    (const uint8_t*)value.c_str(),
    value.length()
  );
}

int readHttpStatusAndHeaders(
  WiFiClientSecure& client,
  int& contentLength,
  bool& chunked
) {
  contentLength = -1;
  chunked = false;

  if (!waitForClientData(client, 30000)) {
    return -1;
  }

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();

  Serial.println(statusLine);

  int code = -1;
  int firstSpace = statusLine.indexOf(' ');

  if (firstSpace >= 0 && statusLine.length() >= firstSpace + 4) {
    code = statusLine.substring(firstSpace + 1, firstSpace + 4).toInt();
  }

  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();

    if (line.length() == 0) {
      break;
    }

    String lower = line;
    lower.toLowerCase();

    if (lower.startsWith("content-length:")) {
      contentLength = line.substring(line.indexOf(':') + 1).toInt();
    }

    if (
      lower.startsWith("transfer-encoding:") &&
      lower.indexOf("chunked") >= 0
    ) {
      chunked = true;
    }
  }

  return code;
}

String readHttpBody(
  WiFiClientSecure& client,
  int contentLength,
  bool chunked
) {
  String body;
  const size_t maxBody = 65536;
  if (contentLength > (int)maxBody) { client.stop(); return ""; }

  if (contentLength > 0 && contentLength < 200000) {
    body.reserve(contentLength + 1);
  } else {
    body.reserve(4096);
  }

  if (chunked) {
    while (true) {
      if (!waitForClientData(client, 30000)) {
        break;
      }

      String sizeLine = client.readStringUntil('\n');
      sizeLine.trim();

      int semicolon = sizeLine.indexOf(';');
      if (semicolon >= 0) {
        sizeLine = sizeLine.substring(0, semicolon);
      }

      long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);

      if (chunkSize <= 0) {
        while (client.connected() || client.available()) {
          String trailer = client.readStringUntil('\n');
          trailer.trim();

          if (trailer.length() == 0) {
            break;
          }
        }

        break;
      }

      long remaining = chunkSize;
      uint8_t buf[512];

      while (remaining > 0) {
        if (!waitForClientData(client, 30000)) {
          return body;
        }

        int want = min((long)sizeof(buf), remaining);
        int n = client.read(buf, want);

        if (n > 0) {
          if (body.length() + n > maxBody) { client.stop(); return ""; }
        body.concat((const char*)buf, n);
          remaining -= n;
        }
      }

      // chunk CRLF
      if (waitForClientData(client, 30000)) client.read();
      if (waitForClientData(client, 30000)) client.read();
    }

    return body;
  }

  if (contentLength >= 0) {
    int remaining = contentLength;
    uint8_t buf[512];

    while (remaining > 0) {
      if (!waitForClientData(client, 30000)) {
        break;
      }

      int n = client.read(
        buf,
        min((int)sizeof(buf), remaining)
      );

      if (n > 0) {
        if (body.length() + n > maxBody) { client.stop(); return ""; }
        body.concat((const char*)buf, n);
        remaining -= n;
      }
    }

    return body;
  }

  uint32_t lastData = millis();
  uint8_t buf[512];

  while (client.connected() || client.available()) {
    int available = client.available();

    if (available > 0) {
      int n = client.read(buf, min((int)sizeof(buf), available));

      if (n > 0) {
        if (body.length() + n > maxBody) { client.stop(); return ""; }
        body.concat((const char*)buf, n);
        lastData = millis();
      }
    } else {
      if (millis() - lastData > 30000) {
        break;
      }

      delay(1);
    }
  }

  return body;
}

// ============================================================
// JSON ESCAPE
// ============================================================

String jsonEscape(const String& input) {
  String out;
  out.reserve(input.length() + 32);

  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];

    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;

      default:
        if ((uint8_t)c < 0x20) {
          // Ignore uncommon control characters.
        } else {
          out += c;
        }
        break;
    }
  }

  return out;
}

// ============================================================
// CONVERSATION PROMPT
// ============================================================

String buildAssistantPrompt() {
  String prompt;
  prompt.reserve(2400);

  prompt +=
    "You are RA AI, a compact voice assistant built into an ESP32-S3 "
    "electronics workbench. Listen carefully to the attached voice recording. "
    "First determine exactly what the user said. Then answer that spoken request. "
    "Be friendly, practical, technically accurate and concise because your answer "
    "will be spoken aloud. Usually answer in 1 to 4 short sentences. "
    "For electronics questions, prioritize practical instructions and electrical safety. "
    "Do not use markdown tables, URLs, emojis, or unnecessary formatting. "
    "If the speech is unclear, say that briefly in the answer. ";

  if (historyCount > 0) {
    prompt += "\nRecent conversation context:\n";

    for (int i = 0; i < historyCount; i++) {
      prompt += "User: ";
      prompt += historyUser[i];
      prompt += "\nRA AI: ";
      prompt += historyAI[i];
      prompt += "\n";
    }
  }

  prompt +=
    "\nReturn two fields only: transcript, containing the user's current spoken words; "
    "and answer, containing the reply that RA AI should speak.";

  return prompt;
}

// ============================================================
// STREAM BASE64 WITHOUT CREATING A 220 KB BASE64 STRING
// ============================================================

size_t base64EncodedLength(size_t inputLength) {
  return 4 * ((inputLength + 2) / 3);
}

bool streamBase64(
  WiFiClientSecure& client,
  const uint8_t* data,
  size_t length
) {
  // 768 is divisible by 3 and produces exactly 1024 base64 bytes.
  const size_t INPUT_CHUNK = 768;

  uint8_t encoded[1028];
  size_t offset = 0;

  while (offset < length) {
    size_t chunk = min(INPUT_CHUNK, length - offset);
    size_t encodedLength = 0;

    int result = mbedtls_base64_encode(
      encoded,
      sizeof(encoded),
      &encodedLength,
      data + offset,
      chunk
    );

    if (result != 0) {
      Serial.printf("Base64 encode error: %d\n", result);
      return false;
    }

    if (!writeAll(client, encoded, encodedLength)) {
      return false;
    }

    offset += chunk;
  }

  return true;
}

// ============================================================
// GEMINI RESPONSE TEXT EXTRACTOR
// ============================================================

String extractGeminiModelText(const String& body) {
  JsonDocument outer;
  DeserializationError err = deserializeJson(outer, body);

  if (err) {
    Serial.print("Gemini outer JSON error: ");
    Serial.println(err.c_str());
    return "";
  }

  if (outer["output_text"].is<const char*>()) {
    return String(outer["output_text"].as<const char*>());
  }
  String modelText;
  JsonArray steps = outer["steps"].as<JsonArray>();

  for (JsonObject step : steps) {
    const char* stepType = step["type"] | "";

    if (strcmp(stepType, "model_output") != 0) {
      continue;
    }

    JsonArray content = step["content"].as<JsonArray>();

    for (JsonObject part : content) {
      const char* type = part["type"] | "";

      if (strcmp(type, "text") == 0) {
        modelText += String((const char*)(part["text"] | ""));
      }
    }
  }

  modelText.trim();
  return modelText;
}

// ============================================================
// GEMINI STAGE 1: STRICT AUDIO TRANSCRIPTION ONLY
// ============================================================

TranscriptionResult transcribeGeminiAudio(
  const uint8_t* wav,
  size_t wavSize
) {
  TranscriptionResult result;

  WiFiClientSecure client;
  client.setCACert(GOOGLE_ROOTS);
  client.setTimeout(45000);

  const char* host = "generativelanguage.googleapis.com";

  if (!client.connect(host, 443)) {
    Serial.println("ERROR: Gemini transcription connection failed.");
    return result;
  }

  // IMPORTANT:
  // No conversation history is sent to this request.
  // This prevents Gemini from filling unclear audio with an earlier topic.
  String prompt =
    "Transcribe ONLY the clearly audible human speech in this WAV recording. "
    "Do not answer the speaker. Do not infer missing words. Do not use outside context. "
    "If there is no clearly intelligible speech, set speech_detected to false and "
    "transcript to an empty string. Preserve the words actually spoken.";

  prompt = jsonEscape(prompt);

  String prefix;
  prefix.reserve(prompt.length() + 700);

  prefix += "{\"model\":\"";
  prefix += GEMINI_AI_MODEL;
  prefix += "\",\"input\":[";
  prefix += "{\"type\":\"text\",\"text\":\"";
  prefix += prompt;
  prefix += "\"},";
  prefix += "{\"type\":\"audio\",\"data\":\"";

  String suffix =
    "\",\"mime_type\":\"audio/wav\"}"
    "],"
    "\"response_format\":{"
      "\"type\":\"text\","
      "\"mime_type\":\"application/json\","
      "\"schema\":{"
        "\"type\":\"object\","
        "\"properties\":{"
          "\"speech_detected\":{\"type\":\"boolean\"},"
          "\"transcript\":{\"type\":\"string\"}"
        "},"
        "\"required\":[\"speech_detected\",\"transcript\"]"
      "}"
    "},"
    "\"generation_config\":{\"thinking_level\":\"minimal\"}"
    "}";

  size_t b64Length = base64EncodedLength(wavSize);
  size_t bodyLength =
    prefix.length() +
    b64Length +
    suffix.length();

  Serial.printf(
    "Gemini STT request: WAV=%u bytes, total body=%u bytes\n",
    (unsigned)wavSize,
    (unsigned)bodyLength
  );

  client.print("POST /v1beta/interactions HTTP/1.1\r\n");
  client.print("Host: ");
  client.print(host);
  client.print("\r\n");

  client.print("x-goog-api-key: ");
  client.print(GEMINI_API_KEY);
  client.print("\r\n");

  client.print("Api-Revision: 2026-05-20\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Accept: application/json\r\n");

  client.print("Content-Length: ");
  client.print(bodyLength);
  client.print("\r\n");

  client.print("Connection: close\r\n\r\n");

  if (!writeString(client, prefix)) {
    client.stop();
    return result;
  }

  if (!streamBase64(client, wav, wavSize)) {
    client.stop();
    return result;
  }

  if (!writeString(client, suffix)) {
    client.stop();
    return result;
  }

  int contentLength = -1;
  bool chunked = false;

  int status = readHttpStatusAndHeaders(
    client,
    contentLength,
    chunked
  );

  String body = readHttpBody(
    client,
    contentLength,
    chunked
  );

  client.stop();

  Serial.print("Gemini STT HTTP: ");
  Serial.println(status);

  if (status != 200) {
    Serial.println(body);
    return result;
  }

  String structuredText = extractGeminiModelText(body);

  if (structuredText.length() == 0) {
    Serial.println("ERROR: Gemini STT returned no text.");
    return result;
  }

  Serial.print("STT structured text: ");
  Serial.println(structuredText);

  JsonDocument structured;
  DeserializationError err =
    deserializeJson(structured, structuredText);

  if (err) {
    Serial.print("STT structured JSON error: ");
    Serial.println(err.c_str());
    return result;
  }

  result.speechDetected =
    structured["speech_detected"] | false;

  result.transcript =
    String((const char*)(structured["transcript"] | ""));

  result.transcript.trim();

  result.ok = true;
  return result;
}

// ============================================================
// BUILD TEXT-ONLY ASSISTANT PROMPT
// ============================================================

String buildTextAssistantPrompt(const String& userText) {
  String prompt;
  prompt.reserve(2600);

  prompt +=
    "You are RA AI, a compact voice assistant built into an ESP32-S3 "
    "electronics workbench. Respond naturally to the user's CURRENT message. "
    "Be friendly, practical and technically accurate. Your response will be "
    "spoken aloud, so answer in one compact sentence by default, unless more detail is truly necessary. "
    "Do not use markdown tables, raw URLs or unnecessary formatting. ";

  if (historyCount > 0) {
    prompt += "\nRecent conversation context:\n";

    for (int i = 0; i < historyCount; i++) {
      prompt += "User: ";
      prompt += historyUser[i];
      prompt += "\nRA AI: ";
      prompt += historyAI[i];
      prompt += "\n";
    }
  }

  prompt += "\nCURRENT USER MESSAGE: ";
  prompt += userText;
  prompt += "\nRA AI:";

  return prompt;
}

// ============================================================
// GEMINI STAGE 2: TEXT TRANSCRIPT -> AI ANSWER
// ============================================================

String askGeminiText(const String& userText) {
  WiFiClientSecure client;
  client.setCACert(GOOGLE_ROOTS);
  client.setTimeout(45000);

  const char* host = "generativelanguage.googleapis.com";

  if (!client.connect(host, 443)) {
    Serial.println("ERROR: Gemini text connection failed.");
    return "";
  }

  String input = jsonEscape(
    buildTextAssistantPrompt(userText)
  );

  String payload;
  payload.reserve(input.length() + 180);

  payload += "{\"model\":\"";
  payload += GEMINI_AI_MODEL;
  payload += "\",\"input\":\"";
  payload += input;
  payload += "\",\"generation_config\":{\"thinking_level\":\"minimal\"}}";

  client.print("POST /v1beta/interactions HTTP/1.1\r\n");
  client.print("Host: ");
  client.print(host);
  client.print("\r\n");

  client.print("x-goog-api-key: ");
  client.print(GEMINI_API_KEY);
  client.print("\r\n");

  client.print("Api-Revision: 2026-05-20\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Accept: application/json\r\n");

  client.print("Content-Length: ");
  client.print(payload.length());
  client.print("\r\n");

  client.print("Connection: close\r\n\r\n");

  if (!writeString(client, payload)) {
    client.stop();
    return "";
  }

  int contentLength = -1;
  bool chunked = false;

  int status = readHttpStatusAndHeaders(
    client,
    contentLength,
    chunked
  );

  String body = readHttpBody(
    client,
    contentLength,
    chunked
  );

  client.stop();

  Serial.print("Gemini AI HTTP: ");
  Serial.println(status);

  if (status != 200) {
    Serial.println(body);
    return "";
  }

  String answer = extractGeminiModelText(body);
  answer.trim();

  return answer;
}

// ============================================================
// HISTORY
// ============================================================

void addConversationHistory(
  const String& userText,
  const String& answer
) {
  if (historyCount < HISTORY_SIZE) {
    historyUser[historyCount] = userText;
    historyAI[historyCount] = answer;
    historyCount++;
    return;
  }

  for (int i = 0; i < HISTORY_SIZE - 1; i++) {
    historyUser[i] = historyUser[i + 1];
    historyAI[i] = historyAI[i + 1];
  }

  historyUser[HISTORY_SIZE - 1] = userText;
  historyAI[HISTORY_SIZE - 1] = answer;
}

// ============================================================
// CLEAN SPEAKER OUTPUT SETTINGS
// ============================================================

// Reduce low-level digital hiss during quiet TTS portions.
const int16_t TTS_NOISE_GATE = 180;

// Keep a little headroom to reduce harshness/distortion.
const float TTS_OUTPUT_GAIN = 0.78f;

// Fade in over first ~12 ms at 24 kHz.
const uint32_t TTS_FADE_IN_SAMPLES = 288;

uint32_t ttsSampleCounter = 0;
int ttsPendingByte = -1;
int16_t ttsLastSample = 0;

void resetTtsAudioCleaner() {
  ttsSampleCounter = 0;
  ttsPendingByte = -1;
  ttsLastSample = 0;
}

bool sendTtsFadeOut() {
  int16_t stereo[192];
  for (int base=0; base<288; base+=96) {
    for (int i=0;i<96;i++) {
      int16_t value=(int16_t)((int32_t)ttsLastSample*(287-base-i)/288);
      stereo[2*i]=stereo[2*i+1]=value;
    }
    if (!writeAudioAll((uint8_t*)stereo,sizeof(stereo))) return false;
  }
  ttsLastSample=0;
  return true;
}

// ============================================================
// TTS: BASE64 AUDIO DELTA -> I2S
// ============================================================

bool playBase64PcmChunk(const char* b64, size_t b64Length, uint16_t& peak) {
  if (!b64 || b64Length==0) return true;
  if (b64Length % 4 != 0) return false;
  // Decode in complete base64 quartets: no heap-sized stereo expansion.
  uint8_t decoded[576];
  int16_t stereo[576];
  for (size_t start=0; start<b64Length; start+=768) {
    size_t chars=min((size_t)768,b64Length-start), count=0;
    if (mbedtls_base64_decode(decoded,sizeof(decoded),&count,
                             (const uint8_t*)b64+start,chars) != 0) return false;
    size_t frames=0;
    for (size_t i=0;i<count;i++) {
      if (ttsPendingByte<0) { ttsPendingByte=decoded[i]; continue; }
      int32_t v=(int16_t)((uint16_t)ttsPendingByte | ((uint16_t)decoded[i]<<8));
      ttsPendingByte=-1;
      int32_t magnitude=v<0?-v:v;
      if (magnitude<TTS_NOISE_GATE) v=0;
      v=(int32_t)(v*TTS_OUTPUT_GAIN);
      if (ttsSampleCounter<TTS_FADE_IN_SAMPLES)
        v=v*(int32_t)ttsSampleCounter/(int32_t)TTS_FADE_IN_SAMPLES;
      v=constrain(v,(int32_t)-32768,(int32_t)32767);
      stereo[2*frames]=stereo[2*frames+1]=(int16_t)v;
      frames++;
      peak=max(peak,(uint16_t)(v<0?-v:v));
      ttsLastSample=(int16_t)v;
      ttsSampleCounter++;
    }
    if (!writeAudioAll((uint8_t*)stereo,frames*4)) return false;
  }
  return true;
}

// ============================================================
// DECODE HTTP CHUNKED TRANSFER FOR SSE
// ============================================================



int readRawByteWithTimeout(
  WiFiClientSecure& client,
  uint32_t timeoutMs
) {
  uint32_t started = millis();

  while (!client.available()) {
    if (!client.connected()) {
      return -1;
    }

    if (millis() - started > timeoutMs) {
      return -1;
    }

    delay(1);
  }

  return client.read();
}

int readDecodedBodyByte(
  WiFiClientSecure& client,
  BodyReaderState& state,
  uint32_t timeoutMs = 45000
) {
  if (state.finished) {
    return -1;
  }

  if (!state.chunked) {
    return readRawByteWithTimeout(client, timeoutMs);
  }

  while (!state.finished) {
    // Data remaining in current HTTP chunk.
    if (state.chunkRemaining > 0) {
      int b = readRawByteWithTimeout(client, timeoutMs);

      if (b < 0) {
        return -1;
      }

      state.chunkRemaining--;
      return b;
    }

    // Current chunk ended. Consume its trailing CRLF.
    if (state.chunkRemaining == 0) {
      int c1 = readRawByteWithTimeout(client, timeoutMs);
      int c2 = readRawByteWithTimeout(client, timeoutMs);

      if (c1 < 0 || c2 < 0) {
        return -1;
      }

      state.chunkRemaining = -1;
    }

    // Read the next hexadecimal chunk-size line from the raw socket.
    if (state.chunkRemaining < 0) {
      String sizeLine;
      sizeLine.reserve(24);

      while (true) {
        int b = readRawByteWithTimeout(client, timeoutMs);

        if (b < 0) {
          return -1;
        }

        if (b == '\n') {
          break;
        }

        if (b != '\r') {
          sizeLine += (char)b;
        }
      }

      int semicolon = sizeLine.indexOf(';');

      if (semicolon >= 0) {
        sizeLine = sizeLine.substring(0, semicolon);
      }

      sizeLine.trim();

      long size = strtol(sizeLine.c_str(), nullptr, 16);

      if (size <= 0) {
        state.finished = true;
        return -1;
      }

      state.chunkRemaining = size;
    }
  }

  return -1;
}

bool readDecodedBodyLine(
  WiFiClientSecure& client,
  BodyReaderState& state,
  String& line,
  uint32_t timeoutMs = 45000
) {
  line = "";
  line.reserve(4096);

  while (true) {
    int b = readDecodedBodyByte(
      client,
      state,
      timeoutMs
    );

    if (b < 0) {
      return line.length() > 0;
    }

    if (b == '\n') {
      if (line.endsWith("\r")) {
        line.remove(line.length() - 1);
      }

      return true;
    }

    if (line.length() >= 262144) { client.stop(); return false; }
    line += (char)b;
  }
}

// ============================================================
// GEMINI TTS - STREAM RESPONSE DIRECTLY TO SPEAKER
// ============================================================

bool speakWithGemini(const String& answer) {
  if (answer.length() == 0) {
    return false;
  }

  WiFiClientSecure client;
  client.setCACert(GOOGLE_ROOTS);
  client.setTimeout(45000);

  const char* host = "generativelanguage.googleapis.com";

  if (!client.connect(host, 443)) {
    Serial.println("ERROR: TTS connection failed.");
    return false;
  }

  String spokenInput =
    "Speak clearly and warmly as RA AI at a quick natural pace, about 15 percent faster than normal. ";
  spokenInput +=
    "Use very short pauses and no dramatic spacing. Say exactly this response: ";
  spokenInput += answer;

  String payload;
  payload.reserve(spokenInput.length() + 450);

  payload += "{\"model\":\"";
  payload += GEMINI_TTS_MODEL;
  payload += "\",";
  payload += "\"input\":\"";
  payload += jsonEscape(spokenInput);
  payload += "\",";
  payload += "\"response_format\":{";
  payload += "\"type\":\"audio\"";
  payload += "},";
  payload += "\"generation_config\":{";
  payload += "\"speech_config\":[{\"voice\":\"";
  payload += GEMINI_VOICE;
  payload += "\"}]";
  payload += "},";
  payload += "\"stream\":true";
  payload += "}";

  client.print("POST /v1beta/interactions?alt=sse HTTP/1.1\r\n");
  client.print("Host: ");
  client.print(host);
  client.print("\r\n");

  client.print("x-goog-api-key: ");
  client.print(GEMINI_API_KEY);
  client.print("\r\n");

  client.print("Api-Revision: 2026-05-20\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Accept: text/event-stream\r\n");

  client.print("Content-Length: ");
  client.print(payload.length());
  client.print("\r\n");

  client.print("Connection: close\r\n\r\n");

  if (!writeString(client, payload)) {
    client.stop();
    return false;
  }

  int contentLength = -1;
  bool chunked = false;

  int status = readHttpStatusAndHeaders(
    client,
    contentLength,
    chunked
  );

  Serial.print("Gemini TTS HTTP: ");
  Serial.println(status);

  if (status != 200) {
    String errorBody = readHttpBody(
      client,
      contentLength,
      chunked
    );

    Serial.println(errorBody);
    client.stop();
    return false;
  }

  if (!beginSpeaker()) {
    client.stop();
    return false;
  }

  resetTtsAudioCleaner();
  drawSpeaking(60);

  uint32_t lastData = millis();
  uint32_t lastUi = 0;
  uint16_t peak = 0;
  bool gotAudio = false;
  bool complete = false;

  // SSE is line based:
  // event: step.delta
  // data: {"delta":{"type":"audio","data":"BASE64..."},...}
  //
  // Google may deliver SSE using HTTP chunked transfer encoding.
  // BodyReaderState removes the HTTP chunk framing before we parse SSE.

  BodyReaderState bodyState;
  bodyState.chunked = chunked;

  while (!complete && !bodyState.finished) {
    String line;

    if (!readDecodedBodyLine(
          client,
          bodyState,
          line,
          45000
        )) {
      if (millis() - lastData > 45000) {
        Serial.println("TTS stream timeout.");
      }

      break;
    }

    line.trim();

    if (line.length() == 0) {
      continue;
    }

    lastData = millis();

    if (!line.startsWith("data:")) {
      continue;
    }

    String dataLine = line.substring(5);
    dataLine.trim();

    if (dataLine == "[DONE]") {
      complete = true;
      break;
    }

    JsonDocument event;
    if (deserializeJson(event, dataLine)) {
      Serial.println("Malformed TTS event.");
      audioWriteFailed=true; break;
    }
    const char* type=event["event_type"] | "";
    if (!strcmp(type,"interaction.completed")) { complete=true; break; }
    if (strstr(type,"error") || !event["error"].isNull() || !strcmp(type,"interaction.failed")) {
      Serial.println("TTS service returned an error.");
      audioWriteFailed=true; break;
    }
    if (strcmp(type,"step.delta") || strcmp(event["delta"]["type"] | "","audio")) continue;
    const char* b64=event["delta"]["data"] | "";
    if (!*b64) continue;
    if (!playBase64PcmChunk(b64,strlen(b64),peak)) {
      audioWriteFailed=true; break;
    }

    if (!gotAudio) {
      Serial.println("TTS AUDIO DATA RECEIVED - sending to MAX98357A");
    }

    gotAudio = true;

    if (millis() - lastUi > 90) {
      uint8_t ui = constrain(
        map(peak, 0, 14000, 35, 255),
        35,
        255
      );

      drawSpeaking(ui);

      peak = 0;
      lastUi = millis();
    }
  }

  if (gotAudio) {
    sendTtsFadeOut();
    writeSpeakerSilence(20);
  }

  // Mute the MAX98357A BEFORE disabling the shared I2S peripheral.
  digitalWrite(AMP_SD_PIN, LOW);
  delay(3);

  I2S.end();
  forceSpeakerDataLow();

  client.stop();

  return gotAudio && complete && !audioWriteFailed && ttsPendingByte < 0;
}

// ============================================================
// ONE COMPLETE ASSISTANT TURN
// ============================================================

void runAssistantTurn() {
  if (!ensureWiFi()) {
    delay(1500);
    drawReady();
    return;
  }

  if (
    String(GEMINI_API_KEY).length() < 10 ||
    String(GEMINI_API_KEY).startsWith("YOUR_")
  ) {
    drawError("Set key in secrets.h");
    delay(2200);
    drawReady();
    return;
  }

  size_t wavSize = 0;
  uint8_t* wav = recordVoice(wavSize);

  if (!wav) {
    drawError("Recording failed");
    delay(1800);
    drawReady();
    return;
  }

  // Reject recordings that are effectively silent before asking Gemini.
  // These limits are intentionally low so normal speech is not rejected.
  if (
    lastMicRms < 35 ||
    lastMicPeak < 250
  ) {
    free(wav);

    Serial.println("NO SPEECH: cleaned microphone signal too low.");
    Serial.println("Check L/R -> GND and INMP441 SD -> GPIO16.");
    drawError("Mic signal too low");
    delay(1600);
    drawReady();
    return;
  }

  // Heavy clipping makes speech recognition unreliable.
  if (lastMicClipPercent > 8.0f) {
    free(wav);

    Serial.println(
      "AUDIO CLIPPED: speak farther from mic or check its supply."
    );

    drawError("Mic too loud / clipping");
    delay(1800);
    drawReady();
    return;
  }

  drawThinking(0);

  // STAGE 1: audio -> exact transcript, with NO conversation history.
  TranscriptionResult stt =
    transcribeGeminiAudio(wav, wavSize);

  free(wav);

  if (!stt.ok) {
    drawError("Transcription failed");
    delay(1800);
    drawReady();
    return;
  }

  if (
    !stt.speechDetected ||
    stt.transcript.length() == 0
  ) {
    Serial.println("Gemini detected no clear speech.");
    drawError("Please speak again");
    delay(1600);
    drawReady();
    return;
  }

  Serial.println();
  Serial.print("HEARD EXACTLY: ");
  Serial.println(stt.transcript);

  // STAGE 2: clean transcript -> assistant reasoning.
  drawThinking(1);

  String answer =
    askGeminiText(stt.transcript);

  if (answer.length() == 0) {
    drawError("Gemini answer failed");
    delay(1800);
    drawReady();
    return;
  }

  Serial.println();
  Serial.println("======================================");
  Serial.print("YOU: ");
  Serial.println(stt.transcript);
  Serial.print("RA AI: ");
  Serial.println(answer);
  Serial.println("======================================");
  Serial.println();

  addConversationHistory(
    stt.transcript,
    answer
  );

  drawSpeaking(60);

  if (!speakWithGemini(answer)) {
    drawError("Gemini TTS failed");
    delay(2000);
  }

  drawReady();
}

// ============================================================
// IDLE FACE ANIMATION
// ============================================================

uint32_t nextBlinkAt = 0;

void updateIdleAnimation() {
  if (currentState != STATE_READY) {
    return;
  }

  if ((int32_t)(millis() - nextBlinkAt) >= 0) {
    drawReady(true);
    delay(90);
    drawReady(false);

    if (random(0, 4) == 0) {
      delay(90);
      drawReady(true);
      delay(75);
      drawReady(false);
    }

    nextBlinkAt = millis() + random(2300, 5200);
  }
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(350);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Start with the speaker amplifier fully muted.
  pinMode(AMP_SD_PIN, OUTPUT);
  digitalWrite(AMP_SD_PIN, LOW);
  forceSpeakerDataLow();

  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(
        SSD1306_SWITCHCAPVCC,
        OLED_ADDR
      )) {
    Serial.println("ERROR: SSD1306 OLED not found.");

    while (true) {
      delay(1000);
    }
  }

  pixel.begin();
  pixel.setBrightness(80);
  pixel.show();

  randomSeed(esp_random());

  drawBootScreen();
  delay(1200);

  connectWiFi();

  drawReady();
  nextBlinkAt = millis() + 2500;

  Serial.println();
  Serial.println("======================================");
  Serial.println("RA AI - FREE GEMINI VERSION");
  Serial.println("ESP32-S3 READY");
  Serial.println("Press the button, release it, then speak.");
  Serial.println("======================================");

  if (psramFound()) {
    Serial.println("PSRAM: FOUND");
  } else {
    Serial.println("PSRAM: NOT FOUND");
  }

  Serial.println();
}

// ============================================================
// LOOP
// ============================================================

void loop() {
  updateIdleAnimation();

  static bool previousButton = HIGH;
  bool button = digitalRead(BUTTON_PIN);

  if (
    previousButton == HIGH &&
    button == LOW
  ) {
    delay(30);

    if (digitalRead(BUTTON_PIN) == LOW) {
      uint32_t pressedAt=millis();
      while (digitalRead(BUTTON_PIN) == LOW && millis()-pressedAt<10000) delay(5);
      if (digitalRead(BUTTON_PIN) == LOW) {
        drawError("Release the button");
        previousButton=LOW;
        return;
      }

      runAssistantTurn();

      nextBlinkAt =
        millis() + random(2300, 5200);
    }
  }

  previousButton = button;
  delay(5);
}
