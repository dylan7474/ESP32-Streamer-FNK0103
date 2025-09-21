#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <AudioFileSourceICYStream.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2SNoDAC.h>

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
AudioOutputI2SNoDAC *audioOutput = nullptr;

void startStreaming();
void stopStreaming();
void cleanupStream();

void drawLayout();
void drawStreamButton();
void updateStatusText();
void connectToWifi();
void handleTouch();
bool readTouchPoint(int &screenX, int &screenY);

void setup() {
  Serial.begin(115200);
  delay(100);

  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(COLOR_BACKGROUND);

  int buttonWidth = tft.width() - 60;
  int buttonHeight = 100;
  int buttonX = (tft.width() - buttonWidth) / 2;
  int buttonY = tft.height() - buttonHeight - 30;
  streamButton = { buttonX, buttonY, buttonWidth, buttonHeight };

  drawLayout();
  connectToWifi();
  updateStatusText();
}

void loop() {
  handleTouch();

  if (!wifiConnected && WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    wifiStatusMessage = WiFi.localIP().toString();
    updateStatusText();
  }

  if (WiFi.status() != WL_CONNECTED && wifiConnected) {
    wifiConnected = false;
    wifiStatusMessage = "WiFi connection lost";
    if (streamingEnabled) {
      stopStreaming();
      streamingEnabled = false;
      drawStreamButton();
    }
    updateStatusText();
  }

  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiAttempt > WIFI_RETRY_INTERVAL_MS) {
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
      }
    } else {
      streamStatusMessage = "Stream stopped";
      stopStreaming();
      streamingEnabled = false;
      drawStreamButton();
      updateStatusText();
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

  Serial.print("Connecting to WiFi network: ");
  Serial.println(WIFI_SSID);

  wifiConnected = false;
  wifiStatusMessage = String("Connecting to ") + WIFI_SSID + "...";
  updateStatusText();

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. IP address: ");
    Serial.println(WiFi.localIP());
    wifiConnected = true;
    wifiStatusMessage = WiFi.localIP().toString();
  } else {
    Serial.println("WiFi connection timed out.");
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
    streamingEnabled = !streamingEnabled;
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
  if (WiFi.status() != WL_CONNECTED) {
    streamStatusMessage = "WiFi required";
    streamingEnabled = false;
    return;
  }

  cleanupStream();

  streamStatusMessage = "Connecting...";
  updateStatusText();

  streamFile = new AudioFileSourceICYStream();
  if (!streamFile || !streamFile->open(STREAM_URL)) {
    streamStatusMessage = "Stream failed";
    cleanupStream();
    streamingEnabled = false;
    return;
  }

  audioOutput = new AudioOutputI2SNoDAC();
  audioOutput->SetPinout(I2S_SPEAKER_BCLK_PIN, I2S_SPEAKER_LRCLK_PIN, I2S_SPEAKER_DATA_PIN);
  audioOutput->SetOutputModeMono(true);
  audioOutput->SetGain(0.8f);

  mp3 = new AudioGeneratorMP3();
  if (!mp3->begin(streamFile, audioOutput)) {
    streamStatusMessage = "Decoder error";
    cleanupStream();
    streamingEnabled = false;
    return;
  }

  streamStatusMessage = "Streaming...";
}

void stopStreaming() {
  if (mp3) {
    if (mp3->isRunning()) {
      mp3->stop();
    }
  }
  cleanupStream();
  streamStatusMessage = "Streaming stopped";
}

void cleanupStream() {
  if (mp3) {
    delete mp3;
    mp3 = nullptr;
  }
  if (audioOutput) {
    delete audioOutput;
    audioOutput = nullptr;
  }
  if (streamFile) {
    streamFile->close();
    delete streamFile;
    streamFile = nullptr;
  }
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
