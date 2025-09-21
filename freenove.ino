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

unsigned long streamingStartMillis = 0;
unsigned long lastMp3LoopMs = 0;
uint32_t mp3LoopIterations = 0;
uint32_t mp3LoopFailures = 0;

void startStreaming();
void stopStreaming(const char *reason = nullptr);
void cleanupStream(const char *context = nullptr);
void logStreamingState(const char *context);
void logHeapUsage(const char *context);
void logWifiDetails(const char *context);
void logMp3LoopDiagnostics(const char *context);
void mp3StatusCallback(void *cbData, int code, const char *string);
void streamSourceStatusCallback(void *cbData, int code, const char *string);
void streamMetadataCallback(void *cbData, const char *type, bool isUnicode, const char *str);

void drawLayout();
void drawStreamButton();
void updateStatusText();
void connectToWifi();
void handleTouch();
bool readTouchPoint(int &screenX, int &screenY);

void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println();
  Serial.println("ESP32 Streamer booting");
  logHeapUsage("Boot");
  logWifiDetails("Boot");

  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(COLOR_BACKGROUND);

  Serial.println("Display initialized");

  int buttonWidth = tft.width() - 60;
  int buttonHeight = 100;
  int buttonX = (tft.width() - buttonWidth) / 2;
  int buttonY = tft.height() - buttonHeight - 30;
  streamButton = { buttonX, buttonY, buttonWidth, buttonHeight };

  drawLayout();
  Serial.println("Starting initial WiFi connection attempt");
  connectToWifi();
  updateStatusText();
}

void loop() {
  handleTouch();

  static unsigned long lastStateLog = 0;
  if (millis() - lastStateLog > 5000) {
    logStreamingState("Loop heartbeat");
    lastStateLog = millis();
  }

  if (!wifiConnected && WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    wifiStatusMessage = WiFi.localIP().toString();
    updateStatusText();
    Serial.print("WiFi connection established. IP: ");
    Serial.println(wifiStatusMessage);
    Serial.print("WiFi RSSI: ");
    Serial.println(WiFi.RSSI());
    Serial.printf("WiFi channel: %d\n", WiFi.channel());
    logWifiDetails("WiFi connected event");
    logHeapUsage("WiFi connected");
  }

  if (WiFi.status() != WL_CONNECTED && wifiConnected) {
    wifiConnected = false;
    wifiStatusMessage = "WiFi connection lost";
    if (streamingEnabled) {
      stopStreaming("WiFi connection lost");
      drawStreamButton();
    }
    updateStatusText();
    Serial.println("WiFi connection lost. Attempting reconnection when interval elapses.");
    logWifiDetails("WiFi lost event");
  }

  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiAttempt > WIFI_RETRY_INTERVAL_MS) {
    Serial.println("WiFi disconnected. Retrying connection.");
    logWifiDetails("WiFi retry trigger");
    connectToWifi();
  }

  if (streamingEnabled && mp3) {
    if (mp3->isRunning()) {
      bool loopResult = mp3->loop();
      if (loopResult) {
        mp3LoopIterations++;
        lastMp3LoopMs = millis();
        if (mp3LoopIterations <= 3 || mp3LoopIterations % 200 == 0) {
          Serial.printf("[MP3] loop ok #%lu | elapsed=%lums\n",
                        static_cast<unsigned long>(mp3LoopIterations),
                        lastMp3LoopMs - streamingStartMillis);
        }
      } else {
        mp3LoopFailures++;
        Serial.printf("[MP3] loop returned false at iteration %lu\n",
                      static_cast<unsigned long>(mp3LoopIterations + 1));
        stopStreaming("MP3 loop reported failure");
        drawStreamButton();
        updateStatusText();
      }
    } else {
      Serial.println("[MP3] Decoder stopped running before loop call.");
      stopStreaming("MP3 decoder stopped running");
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
  Serial.printf("[WiFi] Starting connection attempt at %lums since boot\n", lastWifiAttempt);

  if (strlen(WIFI_SSID) == 0) {
    Serial.println("WiFi credentials not set. Skipping connection attempt.");
    wifiConnected = false;
    wifiStatusMessage = "WiFi credentials not set";
    updateStatusText();
    return;
  }

  WiFi.mode(WIFI_STA);
  logWifiDetails("Before WiFi begin");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  logStreamingState("Initiated WiFi begin");

  Serial.print("Connecting to WiFi network: ");
  Serial.println(WIFI_SSID);

  if (strlen(WIFI_PASSWORD) == 0) {
    Serial.println("Warning: WiFi password is empty. Ensure this is expected.");
  }

  wifiConnected = false;
  wifiStatusMessage = String("Connecting to ") + WIFI_SSID + "...";
  updateStatusText();

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < WIFI_CONNECT_TIMEOUT_MS) {
    Serial.print("WiFi status: ");
    Serial.println(WiFi.status());
    logWifiDetails("WiFi connect wait loop");
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("WiFi RSSI: ");
    Serial.println(WiFi.RSSI());
    Serial.printf("WiFi channel: %d\n", WiFi.channel());
    logWifiDetails("WiFi begin success");
    wifiConnected = true;
    wifiStatusMessage = WiFi.localIP().toString();
  } else {
    Serial.println("WiFi connection timed out.");
    logWifiDetails("WiFi begin timeout");
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
    Serial.printf("Touch detected inside button at (%d, %d). Bounds x=%d..%d y=%d..%d\n",
                  touchX,
                  touchY,
                  streamButton.x,
                  streamButton.x + streamButton.w,
                  streamButton.y,
                  streamButton.y + streamButton.h);
    Serial.print("Touch toggled streaming state to: ");
    Serial.println(streamingEnabled ? "ENABLED" : "DISABLED");
    logStreamingState(streamingEnabled ? "Touch start request" : "Touch stop request");
    if (streamingEnabled) {
      startStreaming();
    } else {
      stopStreaming("User requested stop");
    }
    drawStreamButton();
    updateStatusText();
  }
}

void startStreaming() {
  if (WiFi.status() != WL_CONNECTED) {
    streamStatusMessage = "WiFi required";
    streamingEnabled = false;
    Serial.println("Start stream requested but WiFi not connected.");
    logStreamingState("Start denied - WiFi missing");
    logWifiDetails("Start denied - WiFi missing");
    return;
  }

  cleanupStream("Pre-start cleanup");

  streamStatusMessage = "Connecting...";
  updateStatusText();

  Serial.print("Opening stream URL: ");
  Serial.println(STREAM_URL);
  logHeapUsage("Before stream source allocation");

  streamFile = new AudioFileSourceICYStream();
  if (!streamFile) {
    streamStatusMessage = "Stream failed";
    cleanupStream("Stream allocation failure");
    streamingEnabled = false;
    Serial.println("Failed to allocate stream source instance.");
    logStreamingState("Stream allocation failure");
    logWifiDetails("Stream allocation failure");
    return;
  }

  streamFile->RegisterStatusCB(streamSourceStatusCallback, nullptr);
  streamFile->RegisterMetadataCB(streamMetadataCallback, nullptr);
  streamFile->SetReconnect(5, 2000);
  streamFile->useHTTP10();
  Serial.println("Configured stream source callbacks, reconnect policy, and HTTP/1.0 mode.");

  if (!streamFile->open(STREAM_URL)) {
    streamStatusMessage = "Stream failed";
    cleanupStream("Stream open failure");
    streamingEnabled = false;
    Serial.println("Failed to open stream URL.");
    logStreamingState("Stream open failure");
    logWifiDetails("Stream open failure");
    return;
  }

  Serial.println("Stream source opened successfully.");
  Serial.printf("[Stream] isOpen=%s | size=%u | pos=%u\n",
                streamFile->isOpen() ? "true" : "false",
                streamFile->getSize(),
                streamFile->getPos());
  logWifiDetails("After stream open");
  logHeapUsage("After stream source allocation");

  audioOutput = new AudioOutputI2SNoDAC();
  if (!audioOutput) {
    streamStatusMessage = "Audio alloc fail";
    cleanupStream("Audio output allocation failure");
    streamingEnabled = false;
    Serial.println("Failed to allocate audio output instance.");
    logStreamingState("Audio output allocation failure");
    logWifiDetails("Audio output allocation failure");
    return;
  }

  audioOutput->SetPinout(I2S_SPEAKER_BCLK_PIN, I2S_SPEAKER_LRCLK_PIN, I2S_SPEAKER_DATA_PIN);
  audioOutput->SetOutputModeMono(true);
  audioOutput->SetGain(0.8f);

  Serial.println("Configured I2S output pins and gain.");
  logHeapUsage("After audio output allocation");

  mp3 = new AudioGeneratorMP3();
  if (!mp3) {
    streamStatusMessage = "Decoder alloc fail";
    cleanupStream("Decoder allocation failure");
    streamingEnabled = false;
    Serial.println("Failed to allocate MP3 decoder instance.");
    logStreamingState("Decoder allocation failure");
    logWifiDetails("Decoder allocation failure");
    return;
  }

  mp3->RegisterStatusCB(mp3StatusCallback, nullptr);

  if (!mp3->begin(streamFile, audioOutput)) {
    streamStatusMessage = "Decoder error";
    cleanupStream("Decoder begin failure");
    streamingEnabled = false;
    Serial.println("Failed to start MP3 decoder.");
    logStreamingState("Decoder begin failure");
    logWifiDetails("Decoder begin failure");
    return;
  }

  streamingStartMillis = millis();
  lastMp3LoopMs = streamingStartMillis;
  mp3LoopIterations = 0;
  mp3LoopFailures = 0;

  streamStatusMessage = "Streaming...";
  Serial.println("MP3 decoder started. Streaming...");
  Serial.printf("[MP3] Initial stream position=%u | size=%u\n",
                streamFile ? streamFile->getPos() : 0,
                streamFile ? streamFile->getSize() : 0);
  logWifiDetails("Streaming started");
  logHeapUsage("After decoder begin");
  logStreamingState("Streaming started");
}

void stopStreaming(const char *reason) {
  const char *resolvedReason = reason ? reason : "No reason supplied";
  Serial.print("Stopping streaming. Reason: ");
  Serial.println(resolvedReason);
  unsigned long now = millis();
  unsigned long runtimeMs = streamingStartMillis ? (now - streamingStartMillis) : 0;
  unsigned long sinceLastLoopMs = lastMp3LoopMs ? (now - lastMp3LoopMs) : runtimeMs;
  Serial.printf("[StreamStats] runtimeMs=%lu | sinceLastLoopMs=%lu | loops=%lu | loopFailures=%lu\n",
                runtimeMs,
                sinceLastLoopMs,
                static_cast<unsigned long>(mp3LoopIterations),
                static_cast<unsigned long>(mp3LoopFailures));
  logMp3LoopDiagnostics(resolvedReason);
  if (mp3) {
    if (mp3->isRunning()) {
      mp3->stop();
      Serial.println("Stopped MP3 decoder.");
    }
  }
  streamingEnabled = false;
  cleanupStream("Stop streaming");
  streamStatusMessage = String("Stopped: ") + resolvedReason;
  Serial.print("Streaming resources cleaned up. Last reason: ");
  Serial.println(resolvedReason);
  logWifiDetails("After stop streaming");
  logStreamingState("Streaming stopped");
}

void cleanupStream(const char *context) {
  if (context) {
    Serial.print("Cleaning up stream resources. Context: ");
    Serial.println(context);
  }
  if (mp3) {
    Serial.println("Releasing MP3 decoder instance.");
    delete mp3;
    mp3 = nullptr;
  } else {
    Serial.println("MP3 decoder instance already null.");
  }
  if (audioOutput) {
    Serial.println("Releasing audio output instance.");
    delete audioOutput;
    audioOutput = nullptr;
  } else {
    Serial.println("Audio output instance already null.");
  }
  if (streamFile) {
    Serial.printf("Closing stream source instance. isOpen=%s | pos=%u | size=%u\n",
                  streamFile->isOpen() ? "true" : "false",
                  streamFile->getPos(),
                  streamFile->getSize());
    streamFile->close();
    delete streamFile;
    streamFile = nullptr;
  } else {
    Serial.println("Stream source instance already null.");
  }
  logHeapUsage("After cleanup");
  streamingStartMillis = 0;
  lastMp3LoopMs = 0;
  mp3LoopIterations = 0;
  mp3LoopFailures = 0;
}

void logStreamingState(const char *context) {
  const char *label = context ? context : "(no context)";
  Serial.printf("[StreamState] %s | WiFiStatus=%d | wifiConnected=%s | streamingEnabled=%s | mp3=%p | streamFile=%p | audioOutput=%p\n",
                label,
                WiFi.status(),
                wifiConnected ? "true" : "false",
                streamingEnabled ? "true" : "false",
                mp3,
                streamFile,
                audioOutput);
}

void logHeapUsage(const char *context) {
  const char *label = context ? context : "(no context)";
  Serial.printf("[Heap] %s | free=%u | minFree=%u | maxAlloc=%u\n",
                label,
                ESP.getFreeHeap(),
                ESP.getMinFreeHeap(),
                ESP.getMaxAllocHeap());
}

void logWifiDetails(const char *context) {
  const char *label = context ? context : "(no context)";
  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    Serial.printf("[WiFi] %s | status=%d | SSID=%s | IP=%s | RSSI=%d | channel=%d\n",
                  label,
                  status,
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI(),
                  WiFi.channel());
  } else {
    Serial.printf("[WiFi] %s | status=%d | wifiConnectedFlag=%s\n",
                  label,
                  status,
                  wifiConnected ? "true" : "false");
  }
}

void logMp3LoopDiagnostics(const char *context) {
  const char *label = context ? context : "(no context)";
  unsigned long now = millis();
  unsigned long runtimeMs = streamingStartMillis ? (now - streamingStartMillis) : 0;
  unsigned long sinceLastLoopMs = lastMp3LoopMs ? (now - lastMp3LoopMs) : runtimeMs;
  Serial.printf("[MP3Diag] %s | streamingEnabled=%s | mp3=%p | running=%s | loops=%lu | loopFailures=%lu | runtimeMs=%lu | sinceLastLoopMs=%lu\n",
                label,
                streamingEnabled ? "true" : "false",
                mp3,
                (mp3 && mp3->isRunning()) ? "true" : "false",
                static_cast<unsigned long>(mp3LoopIterations),
                static_cast<unsigned long>(mp3LoopFailures),
                runtimeMs,
                sinceLastLoopMs);
  if (streamFile) {
    Serial.printf("[MP3Diag] StreamState | isOpen=%s | pos=%u | size=%u\n",
                  streamFile->isOpen() ? "true" : "false",
                  streamFile->getPos(),
                  streamFile->getSize());
  } else {
    Serial.println("[MP3Diag] StreamState | streamFile pointer is null");
  }
  Serial.printf("[MP3Diag] AudioOutput pointer=%p\n", audioOutput);
  logWifiDetails("MP3 diagnostics");
  logHeapUsage("MP3 diagnostics");
}

void mp3StatusCallback(void *cbData, int code, const char *string) {
  (void)cbData;
  Serial.printf("[MP3Status] code=%d | message=%s\n", code, string ? string : "(null)");
}

void streamSourceStatusCallback(void *cbData, int code, const char *string) {
  (void)cbData;
  Serial.printf("[StreamSourceStatus] code=%d | message=%s\n", code, string ? string : "(null)");
}

void streamMetadataCallback(void *cbData, const char *type, bool isUnicode, const char *str) {
  (void)cbData;
  Serial.printf("[StreamMeta] type=%s | isUnicode=%s | value=%s\n",
                type ? type : "(null)",
                isUnicode ? "true" : "false",
                str ? str : "(null)");
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
