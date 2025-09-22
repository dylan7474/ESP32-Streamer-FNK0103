#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <AudioFileSourceICYStream.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>

#include "config.h"

static const uint16_t COLOR_BACKGROUND = TFT_BLACK;
static const uint16_t COLOR_TEXT = TFT_WHITE;
static const uint16_t COLOR_ACCENT = TFT_DARKGREEN;
static const uint16_t COLOR_BUTTON_ACTIVE = TFT_GREEN;
static const uint16_t COLOR_BUTTON_INACTIVE = TFT_DARKGREY;
static const uint16_t COLOR_BUTTON_BORDER = TFT_WHITE;

static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
static const unsigned long WIFI_RETRY_INTERVAL_MS = 30000;
static const unsigned long TOUCH_DEBOUNCE_MS = 250;

#ifndef TOUCH_RAW_MIN_X
#define TOUCH_RAW_MIN_X 200
#endif
#ifndef TOUCH_RAW_MAX_X
#define TOUCH_RAW_MAX_X 3900
#endif
#ifndef TOUCH_RAW_MIN_Y
#define TOUCH_RAW_MIN_Y 200
#endif
#ifndef TOUCH_RAW_MAX_Y
#define TOUCH_RAW_MAX_Y 3900
#endif
#ifndef TOUCH_SWAP_XY
#define TOUCH_SWAP_XY 0
#endif
#ifndef TOUCH_INVERT_X
#define TOUCH_INVERT_X 0
#endif
#ifndef TOUCH_INVERT_Y
#define TOUCH_INVERT_Y 1
#endif

TFT_eSPI tft = TFT_eSPI();

struct Button {
  int x;
  int y;
  int w;
  int h;
};

Button streamButton;
bool streamingEnabled = false;
unsigned long lastTouchTime = 0;
unsigned long lastWifiAttempt = 0;
bool wifiConnected = false;
String wifiStatusMessage = "Not connected";
String streamStatusMessage = "Streaming stopped";

AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceICYStream *streamFile = nullptr;
AudioOutput *audioOutput = nullptr;
AudioOutputI2S *audioI2S = nullptr;
bool amplifierEnabled = false;
bool amplifierStateInitialised = false;

void startStreaming();
void stopStreaming();
void cleanupStream();
void setAmplifierState(bool enable);

void drawLayout();
void drawStreamButton();
void updateStatusText();
void connectToWifi();
void handleTouch();
bool readTouchPoint(int &screenX, int &screenY);

void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println("\n[SETUP] Initialising TFT display and touch controller...");

  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(COLOR_BACKGROUND);

  if (AUDIO_AMP_ENABLE_PIN >= 0) {
    pinMode(AUDIO_AMP_ENABLE_PIN, OUTPUT);
    setAmplifierState(false);
    Serial.print("[SETUP] Audio amplifier control initialised on GPIO");
    Serial.println(AUDIO_AMP_ENABLE_PIN);
  }

  Serial.println("[SETUP] Display initialised. Preparing UI layout.");

  int buttonWidth = tft.width() - 60;
  int buttonHeight = 100;
  int buttonX = (tft.width() - buttonWidth) / 2;
  int buttonY = tft.height() - buttonHeight - 30;
  streamButton = { buttonX, buttonY, buttonWidth, buttonHeight };

  drawLayout();
  Serial.println("[SETUP] Layout drawn. Attempting WiFi connection.");
  connectToWifi();
  updateStatusText();
}

void loop() {
  handleTouch();

  if (!wifiConnected && WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    wifiStatusMessage = WiFi.localIP().toString();
    updateStatusText();
    Serial.print("[WIFI] Connection restored. IP address: ");
    Serial.println(wifiStatusMessage);
  }

  if (WiFi.status() != WL_CONNECTED && wifiConnected) {
    wifiConnected = false;
    wifiStatusMessage = "WiFi connection lost";
    if (streamingEnabled) {
      Serial.println("[STREAM] WiFi connection lost during playback. Stopping stream.");
      stopStreaming();
      streamingEnabled = false;
      drawStreamButton();
    }
    updateStatusText();
    Serial.println("[WIFI] Connection lost. Streaming disabled.");
  }

  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiAttempt > WIFI_RETRY_INTERVAL_MS) {
    Serial.println("[WIFI] Connection lost. Retrying WiFi connection...");
    connectToWifi();
  }

  if (streamingEnabled && mp3) {
    if (mp3->isRunning()) {
      if (!mp3->loop()) {
        streamStatusMessage = "Stream stopped";
        stopStreaming();
        streamingEnabled = false;
        drawStreamButton();
        updateStatusText();
        Serial.println("[STREAM] Stream loop returned false. Stream stopped.");
      }
    } else {
      streamStatusMessage = "Stream stopped";
      stopStreaming();
      streamingEnabled = false;
      drawStreamButton();
      updateStatusText();
      Serial.println("[STREAM] Decoder reported not running. Stream stopped.");
    }
  }

  delay(10);
}

void drawLayout() {
  tft.fillScreen(COLOR_BACKGROUND);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COLOR_TEXT, COLOR_BACKGROUND);
  tft.setTextSize(3);
  tft.drawString("ESP32 Streamer", tft.width() / 2, 40);

  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(2);
  tft.drawString("Status", 20, 100);

  drawStreamButton();
  updateStatusText();
}

void drawStreamButton() {
  uint16_t fillColor = streamingEnabled ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON_INACTIVE;
  const int radius = 16;
  tft.fillRoundRect(streamButton.x, streamButton.y, streamButton.w, streamButton.h, radius, fillColor);
  tft.drawRoundRect(streamButton.x, streamButton.y, streamButton.w, streamButton.h, radius, COLOR_BUTTON_BORDER);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COLOR_TEXT, fillColor);
  tft.setTextSize(3);
  tft.drawString(streamingEnabled ? "Stop Stream" : "Start Stream",
                 streamButton.x + streamButton.w / 2,
                 streamButton.y + streamButton.h / 2);
}

void updateStatusText() {
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(2);

  const int statusAreaX = 20;
  const int statusAreaY = 130;
  const int statusLineHeight = 28;
  const int statusWidth = tft.width() - 40;
  const int statusHeight = (statusLineHeight * 2) + 8;

  tft.fillRect(statusAreaX, statusAreaY - 4, statusWidth, statusHeight, COLOR_BACKGROUND);

  tft.drawString(wifiStatusMessage, statusAreaX, statusAreaY);

  tft.drawString(streamStatusMessage,
                 statusAreaX,
                 statusAreaY + statusLineHeight);
}

void connectToWifi() {
  lastWifiAttempt = millis();

  if (strlen(WIFI_SSID) == 0) {
    Serial.println("WiFi credentials not set. Skipping connection attempt.");
    wifiConnected = false;
    wifiStatusMessage = "WiFi credentials not set";
    updateStatusText();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("[WIFI] Connecting to WiFi network: ");
  Serial.println(WIFI_SSID);

  wifiConnected = false;
  wifiStatusMessage = String("Connecting to ") + WIFI_SSID + "...";
  updateStatusText();

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WIFI] Connected. IP address: ");
    Serial.println(WiFi.localIP());
    wifiConnected = true;
    wifiStatusMessage = WiFi.localIP().toString();
  } else {
    Serial.println("[WIFI] Connection timed out.");
    wifiConnected = false;
    wifiStatusMessage = "WiFi connection timed out";
  }

  updateStatusText();
}

void handleTouch() {
  int touchX = 0;
  int touchY = 0;

  if (!readTouchPoint(touchX, touchY)) {
    return;
  }

  unsigned long now = millis();
  if (now - lastTouchTime < TOUCH_DEBOUNCE_MS) {
    return;
  }
  lastTouchTime = now;

  bool insideButton = touchX >= streamButton.x && touchX <= (streamButton.x + streamButton.w) &&
                      touchY >= streamButton.y && touchY <= (streamButton.y + streamButton.h);

  if (insideButton) {
    bool requestStart = !streamingEnabled;
    streamingEnabled = requestStart;
    Serial.println(requestStart ? "[STREAM] Start requested from touch UI." :
                                      "[STREAM] Stop requested from touch UI.");
    if (streamingEnabled) {
      startStreaming();
    } else {
      stopStreaming();
    }
    drawStreamButton();
    updateStatusText();
  }
}

void startStreaming() {
  Serial.println("[STREAM] Start request received.");
  if (WiFi.status() != WL_CONNECTED) {
    streamStatusMessage = "WiFi required";
    streamingEnabled = false;
    Serial.println("[STREAM] Cannot start - WiFi not connected.");
    return;
  }

  cleanupStream();

  Serial.println("[STREAM] Creating stream source and audio output.");

  streamStatusMessage = "Connecting...";
  updateStatusText();

  streamFile = new AudioFileSourceICYStream();
  if (!streamFile || !streamFile->open(STREAM_URL)) {
    streamStatusMessage = "Stream failed";
    cleanupStream();
    streamingEnabled = false;
    Serial.println("[STREAM] Failed to open stream URL.");
    return;
  }

  Serial.print("[STREAM] Connected to stream URL: ");
  Serial.println(STREAM_URL);

  bool useExternalI2S = I2S_SPEAKER_BCLK_PIN >= 0 && I2S_SPEAKER_LRCLK_PIN >= 0 && I2S_SPEAKER_DATA_PIN >= 0;
  if (useExternalI2S) {
    Serial.print("[STREAM] Configuring external I2S pins BCLK=");
    Serial.print(I2S_SPEAKER_BCLK_PIN);
    Serial.print(", LRCLK=");
    Serial.print(I2S_SPEAKER_LRCLK_PIN);
    Serial.print(", DATA=");
    Serial.println(I2S_SPEAKER_DATA_PIN);
    audioI2S = new AudioOutputI2S();
  } else {
    Serial.print("[STREAM] Using internal DAC on GPIO");
    Serial.println(I2S_SPEAKER_DATA_PIN);
    audioI2S = new AudioOutputI2S(0, 1);
  }

  if (!audioI2S) {
    streamStatusMessage = "Audio init failed";
    cleanupStream();
    streamingEnabled = false;
    Serial.println("[STREAM] Failed to allocate audio output.");
    return;
  }

  audioOutput = audioI2S;

  bool pinoutOk = false;
  if (useExternalI2S) {
    pinoutOk = audioI2S->SetPinout(I2S_SPEAKER_BCLK_PIN, I2S_SPEAKER_LRCLK_PIN, I2S_SPEAKER_DATA_PIN);
  } else {
    if (I2S_SPEAKER_DATA_PIN < 0) {
      Serial.println("[STREAM] Invalid DAC pin configuration. Set I2S_SPEAKER_DATA_PIN.");
    } else {
      pinoutOk = audioI2S->SetPinout(-1, -1, I2S_SPEAKER_DATA_PIN);
    }
  }

  if (!pinoutOk) {
    streamStatusMessage = "Audio pin error";
    cleanupStream();
    streamingEnabled = false;
    Serial.println("[STREAM] Audio pin configuration failed.");
    return;
  }

  audioI2S->SetOutputModeMono(true);
  audioOutput->SetGain(1.0f);
  setAmplifierState(true);
  Serial.println("[STREAM] Audio output configured.");

  mp3 = new AudioGeneratorMP3();
  if (!mp3->begin(streamFile, audioOutput)) {
    streamStatusMessage = "Decoder error";
    cleanupStream();
    streamingEnabled = false;
    Serial.println("[STREAM] MP3 decoder failed to begin.");
    return;
  }

  streamStatusMessage = "Streaming...";
  Serial.println("[STREAM] Streaming started successfully.");
}

void stopStreaming() {
  Serial.println("[STREAM] Stop request received.");
  if (mp3) {
    if (mp3->isRunning()) {
      mp3->stop();
      Serial.println("[STREAM] MP3 decoder stopped.");
    }
  }
  cleanupStream();
  streamStatusMessage = "Streaming stopped";
}

void cleanupStream() {
  Serial.println("[STREAM] Cleaning up stream resources.");
  if (mp3) {
    delete mp3;
    mp3 = nullptr;
  }
  if (audioOutput) {
    delete audioOutput;
    audioOutput = nullptr;
    audioI2S = nullptr;
  }
  if (streamFile) {
    streamFile->close();
    delete streamFile;
    streamFile = nullptr;
  }
  setAmplifierState(false);
  Serial.println("[STREAM] Cleanup complete.");
}

bool readTouchPoint(int &screenX, int &screenY) {
  uint16_t rawX = 0;
  uint16_t rawY = 0;
  if (!tft.getTouchRaw(&rawX, &rawY)) {
    return false;
  }

  long calMinX = TOUCH_RAW_MIN_X;
  long calMaxX = TOUCH_RAW_MAX_X;
  long calMinY = TOUCH_RAW_MIN_Y;
  long calMaxY = TOUCH_RAW_MAX_Y;

#if TOUCH_SWAP_XY
  uint16_t rawSwap = rawX;
  rawX = rawY;
  rawY = rawSwap;
  long calSwap = calMinX;
  calMinX = calMinY;
  calMinY = calSwap;
  calSwap = calMaxX;
  calMaxX = calMaxY;
  calMaxY = calSwap;
#endif

#if TOUCH_INVERT_X
  long calSwap = calMinX;
  calMinX = calMaxX;
  calMaxX = calSwap;
#endif

#if TOUCH_INVERT_Y
  long calSwap = calMinY;
  calMinY = calMaxY;
  calMaxY = calSwap;
#endif

  long mappedX = rawX;
  long mappedY = rawY;

  if (calMaxX != calMinX) {
    mappedX = map((long)rawX, calMinX, calMaxX, 0, (long)tft.width() - 1);
  } else {
    mappedX = rawX % tft.width();
  }

  if (calMaxY != calMinY) {
    mappedY = map((long)rawY, calMinY, calMaxY, 0, (long)tft.height() - 1);
  } else {
    mappedY = rawY % tft.height();
  }

  mappedX = constrain(mappedX, 0, (long)tft.width() - 1);
  mappedY = constrain(mappedY, 0, (long)tft.height() - 1);

  screenX = (int)mappedX;
  screenY = (int)mappedY;
  return true;
}

void setAmplifierState(bool enable) {
  if (AUDIO_AMP_ENABLE_PIN < 0) {
    return;
  }
  if (!amplifierStateInitialised || enable != amplifierEnabled) {
    digitalWrite(AUDIO_AMP_ENABLE_PIN, enable ? LOW : HIGH);
    if (!enable) {
      delay(5);
    }
    amplifierEnabled = enable;
    amplifierStateInitialised = true;
  }
}
