#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <AudioFileSourceICYStream.h>
#include <AudioGeneratorMP3.h>
#include <AudioLogger.h>
#include <AudioOutputI2SNoDAC.h>
#include <HTTPClient.h>

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
uint32_t lastStreamPosition = 0;
uint64_t totalStreamBytesRead = 0;
unsigned long lastStreamReadUpdateMs = 0;

struct StreamSourceStats {
  uint32_t totalCallbacks;
  uint32_t noDataEvents;
  uint32_t disconnectEvents;
  uint32_t reconnectAttempts;
  uint32_t reconnectSuccesses;
  uint32_t consecutiveSameCode;
  uint32_t consecutiveNoData;
  int lastCode;
  unsigned long firstCallbackMs;
  unsigned long lastCallbackMs;
  unsigned long lastCodeChangeMs;
  unsigned long lastNoDataMs;
};

StreamSourceStats streamSourceStats = {};

static const size_t MP3_STATUS_HISTORY_LENGTH = 10;
static const size_t STREAM_STATUS_HISTORY_LENGTH = 12;

struct Mp3StatusEvent {
  int code;
  unsigned long timestampMs;
  unsigned long runtimeMs;
  unsigned long sinceLastMs;
  String message;
};

struct StreamStatusEvent {
  int code;
  unsigned long timestampMs;
  unsigned long runtimeMs;
  unsigned long sinceLastMs;
  String message;
};

Mp3StatusEvent mp3StatusHistory[MP3_STATUS_HISTORY_LENGTH];
size_t mp3StatusHistoryCount = 0;
size_t mp3StatusHistoryNextIndex = 0;

StreamStatusEvent streamStatusHistory[STREAM_STATUS_HISTORY_LENGTH];
size_t streamStatusHistoryCount = 0;
size_t streamStatusHistoryNextIndex = 0;

void startStreaming();
void stopStreaming(const char *reason = nullptr);
void cleanupStream(const char *context = nullptr);
void logStreamingState(const char *context);
void logHeapUsage(const char *context);
void logWifiDetails(const char *context);
void logMp3LoopDiagnostics(const char *context);
void logRecentMp3StatusEvents(const char *context);
void logStreamSourceSummary(const char *context);
void logStreamBufferState(const char *context);
void logStreamReadProgress(const char *context, bool forceLog);
void logStreamComponents(const char *context);
void logRecentStreamStatusEvents(const char *context);
void resetStreamSourceStats(const char *context);
void resetMp3StatusHistory(const char *context);
void resetStreamStatusHistory(const char *context);
void resetStreamReadStats(const char *context);
bool probeStreamUrl(const char *url);
const char *streamSourceStatusToString(int code);
const char *mp3ErrorCodeToString(int code);
void recordMp3StatusEvent(int code, const char *message);
void recordStreamStatusEvent(int code, const char *message);
void mp3StatusCallback(void *cbData, int code, const char *string);
void streamSourceStatusCallback(void *cbData, int code, const char *string);
void streamMetadataCallback(void *cbData, const char *type, bool isUnicode, const char *str);

void drawLayout();
void drawStreamButton();
void updateStatusText();
void connectToWifi();
void handleTouch();
bool readTouchPoint(int &screenX, int &screenY);

bool probeStreamUrl(const char *url) {
  if (!url || strlen(url) == 0) {
    Serial.println("[StreamProbe] Probe skipped because URL is empty.");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[StreamProbe] Probe skipped because WiFi is not connected.");
    return false;
  }

  Serial.printf("[StreamProbe] Probing stream URL: %s\n", url);
  WiFiClient probeClient;
  HTTPClient probeHttp;
  const char *headers[] = {
    "Content-Type",
    "icy-metaint",
    "icy-br",
    "icy-name",
    "Transfer-Encoding",
    "Accept-Ranges",
    "Server"
  };
  const size_t headerCount = sizeof(headers) / sizeof(headers[0]);

  probeHttp.setReuse(false);
#ifdef HTTPC_FORCE_FOLLOW_REDIRECTS
  probeHttp.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
#endif
  probeHttp.collectHeaders(headers, headerCount);

  unsigned long startMs = millis();
  if (!probeHttp.begin(probeClient, url)) {
    Serial.println("[StreamProbe] Failed to initialize HTTPClient for probe.");
    return false;
  }

  int httpCode = probeHttp.GET();
  unsigned long elapsedMs = millis() - startMs;
  Serial.printf("[StreamProbe] HTTP GET completed | code=%d | elapsedMs=%lu | contentLength=%d | isChunked=%s\n",
                httpCode,
                elapsedMs,
                probeHttp.getSize(),
                probeHttp.isChunked() ? "true" : "false");

  for (size_t i = 0; i < headerCount; ++i) {
    if (probeHttp.hasHeader(headers[i])) {
      Serial.printf("[StreamProbe] Header %s: %s\n",
                    headers[i],
                    probeHttp.header(headers[i]).c_str());
    }
  }

  WiFiClient *probeStream = probeHttp.getStreamPtr();
  if (probeStream) {
    size_t available = probeStream->available();
    Serial.printf("[StreamProbe] Initial available payload bytes=%u\n", static_cast<unsigned int>(available));
  } else {
    Serial.println("[StreamProbe] Stream pointer unavailable during probe.");
  }

  probeHttp.end();
  bool success = httpCode == HTTP_CODE_OK;
  Serial.printf("[StreamProbe] Probe %s\n", success ? "succeeded" : "failed");
  return success;
}

void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println();
  Serial.println("ESP32 Streamer booting");
  audioLogger = &Serial;
  Serial.println("Audio library logging routed to Serial.");
  logHeapUsage("Boot");
  logWifiDetails("Boot");
  resetMp3StatusHistory("Boot");
  resetStreamStatusHistory("Boot");

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

  static unsigned long lastComponentLog = 0;
  unsigned long nowMillis = millis();
  if (streamingEnabled && mp3 && streamingStartMillis != 0) {
    unsigned long sinceStart = nowMillis - streamingStartMillis;
    unsigned long desiredInterval = sinceStart < 5000 ? 1000 : 5000;
    if (nowMillis - lastComponentLog > desiredInterval) {
      logStreamComponents("Loop detailed heartbeat");
      lastComponentLog = nowMillis;
    }
  } else {
    lastComponentLog = nowMillis;
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
        bool logThisIteration = (mp3LoopIterations <= 3 || mp3LoopIterations % 200 == 0);
        logStreamReadProgress("MP3 loop iteration", logThisIteration);
        if (logThisIteration) {
          Serial.printf("[MP3] loop ok #%lu | elapsed=%lums\n",
                        static_cast<unsigned long>(mp3LoopIterations),
                        lastMp3LoopMs - streamingStartMillis);
          logStreamBufferState("MP3 loop success snapshot");
          logStreamReadProgress("MP3 loop success snapshot", true);
          logStreamComponents("MP3 loop success snapshot");
        }
      } else {
        mp3LoopFailures++;
        Serial.printf("[MP3] loop returned false at iteration %lu\n",
                      static_cast<unsigned long>(mp3LoopIterations + 1));
        logStreamBufferState("MP3 loop failure snapshot");
        logStreamReadProgress("MP3 loop failure snapshot", true);
        logStreamComponents("MP3 loop failure snapshot");
        logStreamSourceSummary("MP3 loop failure snapshot");
        logRecentStreamStatusEvents("MP3 loop failure snapshot");
        logRecentMp3StatusEvents("MP3 loop failure snapshot");
        logHeapUsage("MP3 loop failure snapshot");
        logWifiDetails("MP3 loop failure snapshot");
        stopStreaming("MP3 loop reported failure");
        drawStreamButton();
        updateStatusText();
      }
    } else {
      Serial.println("[MP3] Decoder stopped running before loop call.");
      logStreamBufferState("MP3 decoder stopped running before loop");
      logStreamReadProgress("MP3 decoder stopped running before loop", true);
      logStreamComponents("MP3 decoder stopped running before loop");
      logStreamSourceSummary("MP3 decoder stopped running before loop");
      logRecentStreamStatusEvents("MP3 decoder stopped running before loop");
      logRecentMp3StatusEvents("MP3 decoder stopped running before loop");
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

  resetStreamReadStats("Start streaming request");
  cleanupStream("Pre-start cleanup");
  resetMp3StatusHistory("Start streaming requested");

  streamStatusMessage = "Connecting...";
  updateStatusText();

  Serial.print("Opening stream URL: ");
  Serial.println(STREAM_URL);
  bool probeSuccess = probeStreamUrl(STREAM_URL);
  Serial.printf("[StreamProbe] Pre-open probe result: %s\n", probeSuccess ? "success" : "failure");
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

  logStreamBufferState("After stream source allocation");

  resetStreamSourceStats("Start streaming - after source allocation");
  resetStreamReadStats("Start streaming - before stream open");

  if (!streamFile->open(STREAM_URL)) {
    streamStatusMessage = "Stream failed";
    logStreamBufferState("Stream open failure");
    logStreamReadProgress("Stream open failure", true);
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
  resetStreamReadStats("After stream open");
  logStreamReadProgress("After stream open", true);
  logStreamBufferState("After stream open");
  logWifiDetails("After stream open");
  logHeapUsage("After stream source allocation");
  logStreamComponents("After stream source allocation");

  audioOutput = new AudioOutputI2SNoDAC();
  if (!audioOutput) {
    streamStatusMessage = "Audio alloc fail";
    logStreamBufferState("Audio output allocation failure");
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
  logStreamComponents("After audio output allocation");

  mp3 = new AudioGeneratorMP3();
  if (!mp3) {
    streamStatusMessage = "Decoder alloc fail";
    logStreamBufferState("Decoder allocation failure");
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
    logStreamBufferState("Decoder begin failure");
    logStreamReadProgress("Decoder begin failure", true);
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
  recordMp3StatusEvent(MAD_ERROR_NONE, "MP3 decoder started (manual marker)");
  recordStreamStatusEvent(-1, "Streaming session started (manual marker)");
  Serial.printf("[MP3] Initial stream position=%u | size=%u\n",
                streamFile ? streamFile->getPos() : 0,
                streamFile ? streamFile->getSize() : 0);
  logStreamReadProgress("Streaming started", true);
  logStreamBufferState("Streaming started");
  logWifiDetails("Streaming started");
  logHeapUsage("After decoder begin");
  logStreamingState("Streaming started");
  logStreamComponents("Streaming started");
}

void stopStreaming(const char *reason) {
  const char *resolvedReason = reason ? reason : "No reason supplied";
  Serial.print("Stopping streaming. Reason: ");
  Serial.println(resolvedReason);
  unsigned long now = millis();
  unsigned long runtimeMs = streamingStartMillis ? (now - streamingStartMillis) : 0;
  unsigned long sinceLastLoopMs = lastMp3LoopMs ? (now - lastMp3LoopMs) : runtimeMs;
  String stopReasonMessage = String("Stop requested: ") + resolvedReason;
  recordMp3StatusEvent(MAD_ERROR_NONE, stopReasonMessage.c_str());
  recordStreamStatusEvent(-2, stopReasonMessage.c_str());
  Serial.printf("[StreamStats] runtimeMs=%lu | sinceLastLoopMs=%lu | loops=%lu | loopFailures=%lu\n",
                runtimeMs,
                sinceLastLoopMs,
                static_cast<unsigned long>(mp3LoopIterations),
                static_cast<unsigned long>(mp3LoopFailures));
  logStreamReadProgress("Stop streaming", true);
  logMp3LoopDiagnostics(resolvedReason);
  logStreamSourceSummary("Stop streaming (pre-cleanup)");
  logStreamComponents("Stop streaming (pre-cleanup)");
  logStreamBufferState("Stop streaming (pre-cleanup)");
  logRecentStreamStatusEvents("Stop streaming (pre-cleanup)");
  logRecentMp3StatusEvents("Stop streaming (pre-cleanup)");
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
  logStreamSourceSummary("Cleanup start");
  logStreamBufferState("Cleanup start");
  logStreamReadProgress("Cleanup start", true);
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
    logStreamBufferState("Cleanup before stream close");
    streamFile->close();
    delete streamFile;
    streamFile = nullptr;
  } else {
    Serial.println("Stream source instance already null.");
  }
  logStreamComponents("After stream resource release");
  logHeapUsage("After cleanup");
  streamingStartMillis = 0;
  lastMp3LoopMs = 0;
  mp3LoopIterations = 0;
  mp3LoopFailures = 0;
  resetStreamSourceStats("Cleanup complete");
  resetMp3StatusHistory("Cleanup complete");
  resetStreamReadStats("Cleanup complete");
}

void resetMp3StatusHistory(const char *context) {
  if (context) {
    Serial.print("[MP3StatusHistory] Reset requested. Context: ");
    Serial.println(context);
  }
  for (size_t i = 0; i < MP3_STATUS_HISTORY_LENGTH; ++i) {
    mp3StatusHistory[i] = Mp3StatusEvent();
  }
  mp3StatusHistoryCount = 0;
  mp3StatusHistoryNextIndex = 0;
}

void resetStreamStatusHistory(const char *context) {
  if (context) {
    Serial.print("[StreamStatusHistory] Reset requested. Context: ");
    Serial.println(context);
  }
  for (size_t i = 0; i < STREAM_STATUS_HISTORY_LENGTH; ++i) {
    streamStatusHistory[i] = StreamStatusEvent();
  }
  streamStatusHistoryCount = 0;
  streamStatusHistoryNextIndex = 0;
}

void resetStreamReadStats(const char *context) {
  if (context) {
    Serial.print("[StreamRead] Reset requested. Context: ");
    Serial.println(context);
  }
  lastStreamPosition = streamFile ? streamFile->getPos() : 0;
  totalStreamBytesRead = 0;
  lastStreamReadUpdateMs = millis();
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

void logStreamSourceSummary(const char *context) {
  const char *label = context ? context : "(no context)";
  if (streamSourceStats.totalCallbacks == 0) {
    Serial.printf("[StreamSourceSummary] %s | no status callbacks recorded yet\n", label);
    return;
  }
  Serial.printf("[StreamSourceSummary] %s | totalCallbacks=%lu | lastCode=%d (%s) | consecutiveSame=%lu | noDataEvents=%lu | consecutiveNoData=%lu | disconnects=%lu | reconnectAttempts=%lu | reconnectSuccesses=%lu | firstCallbackMs=%lu | lastCallbackMs=%lu | lastCodeChangeMs=%lu | lastNoDataMs=%lu\n",
                label,
                static_cast<unsigned long>(streamSourceStats.totalCallbacks),
                streamSourceStats.lastCode,
                streamSourceStatusToString(streamSourceStats.lastCode),
                static_cast<unsigned long>(streamSourceStats.consecutiveSameCode),
                static_cast<unsigned long>(streamSourceStats.noDataEvents),
                static_cast<unsigned long>(streamSourceStats.consecutiveNoData),
                static_cast<unsigned long>(streamSourceStats.disconnectEvents),
                static_cast<unsigned long>(streamSourceStats.reconnectAttempts),
                static_cast<unsigned long>(streamSourceStats.reconnectSuccesses),
                streamSourceStats.firstCallbackMs,
                streamSourceStats.lastCallbackMs,
                streamSourceStats.lastCodeChangeMs,
                streamSourceStats.lastNoDataMs);
}

void logStreamBufferState(const char *context) {
  const char *label = context ? context : "(no context)";
  if (!streamFile) {
    Serial.printf("[StreamBuffer] %s | streamFile pointer is null\n", label);
    return;
  }

  Serial.printf("[StreamBuffer] %s | isOpen=%s | pos=%u | size=%u\n",
                label,
                streamFile->isOpen() ? "true" : "false",
                streamFile->getPos(),
                streamFile->getSize());
}

void logStreamReadProgress(const char *context, bool forceLog) {
  const char *label = context ? context : "(no context)";
  unsigned long now = millis();
  if (!streamFile) {
    if (forceLog) {
      Serial.printf("[StreamRead] %s | streamFile pointer is null\n", label);
    }
    lastStreamPosition = 0;
    lastStreamReadUpdateMs = now;
    return;
  }

  uint32_t currentPos = streamFile->getPos();
  bool positionWrapped = currentPos < lastStreamPosition;
  uint32_t delta = 0;
  if (positionWrapped) {
    delta = currentPos;
  } else {
    delta = currentPos - lastStreamPosition;
  }

  totalStreamBytesRead += delta;
  unsigned long sinceLastMs = lastStreamReadUpdateMs ? (now - lastStreamReadUpdateMs) : 0;
  bool shouldLog = forceLog || positionWrapped;
  if (shouldLog) {
    if (positionWrapped) {
      Serial.printf("[StreamRead] %s | stream position wrapped or reset (previous=%u, current=%u)\n",
                    label,
                    lastStreamPosition,
                    currentPos);
    }
    Serial.printf("[StreamRead] %s | pos=%u | delta=%u | total=%llu | sinceLastMs=%lu | isOpen=%s\n",
                  label,
                  currentPos,
                  delta,
                  static_cast<unsigned long long>(totalStreamBytesRead),
                  sinceLastMs,
                  streamFile->isOpen() ? "true" : "false");
  }

  lastStreamPosition = currentPos;
  lastStreamReadUpdateMs = now;
}

void resetStreamSourceStats(const char *context) {
  if (context) {
    Serial.print("[StreamSourceStats] Reset requested. Context: ");
    Serial.println(context);
  }
  streamSourceStats = {};
  streamSourceStats.lastCode = -1;
  streamSourceStats.firstCallbackMs = 0;
  streamSourceStats.lastCallbackMs = 0;
  streamSourceStats.lastCodeChangeMs = 0;
  streamSourceStats.lastNoDataMs = 0;
  resetStreamStatusHistory(context);
  resetStreamReadStats(context);
}

void logStreamComponents(const char *context) {
  const char *label = context ? context : "(no context)";
  unsigned long now = millis();
  unsigned long runtimeMs = streamingStartMillis ? (now - streamingStartMillis) : 0;
  unsigned long sinceLastLoopMs = lastMp3LoopMs ? (now - lastMp3LoopMs) : runtimeMs;
  Serial.printf("[StreamComponents] %s | runtimeMs=%lu | sinceLastLoopMs=%lu | streamingEnabled=%s | wifiConnectedFlag=%s | WiFiStatus=%d\n",
                label,
                runtimeMs,
                sinceLastLoopMs,
                streamingEnabled ? "true" : "false",
                wifiConnected ? "true" : "false",
                WiFi.status());
  Serial.printf("[StreamComponents] loopIterations=%lu | loopFailures=%lu\n",
                static_cast<unsigned long>(mp3LoopIterations),
                static_cast<unsigned long>(mp3LoopFailures));
  if (streamFile) {
    Serial.printf("[StreamComponents] streamFile=%p | isOpen=%s | pos=%u | size=%u\n",
                  streamFile,
                  streamFile->isOpen() ? "true" : "false",
                  streamFile->getPos(),
                  streamFile->getSize());
  } else {
    Serial.println("[StreamComponents] streamFile pointer is null");
  }
  unsigned long sinceLastReadMs = lastStreamReadUpdateMs ? (now - lastStreamReadUpdateMs) : 0;
  Serial.printf("[StreamComponents] streamBytesReadTotal=%llu | lastStreamPos=%u | sinceLastReadMs=%lu\n",
                static_cast<unsigned long long>(totalStreamBytesRead),
                lastStreamPosition,
                sinceLastReadMs);
  Serial.printf("[StreamComponents] mp3=%p | running=%s | audioOutput=%p\n",
                mp3,
                (mp3 && mp3->isRunning()) ? "true" : "false",
                audioOutput);
  Serial.printf("[StreamComponents] statusMessages | wifi=\"%s\" | stream=\"%s\"\n",
                wifiStatusMessage.c_str(),
                streamStatusMessage.c_str());
}

void logRecentMp3StatusEvents(const char *context) {
  const char *label = context ? context : "(no context)";
  if (mp3StatusHistoryCount == 0) {
    Serial.printf("[MP3StatusHistory] %s | no events recorded\n", label);
    return;
  }

  Serial.printf("[MP3StatusHistory] %s | count=%u\n",
                label,
                static_cast<unsigned int>(mp3StatusHistoryCount));

  for (size_t i = 0; i < mp3StatusHistoryCount; ++i) {
    size_t index = (mp3StatusHistoryNextIndex + MP3_STATUS_HISTORY_LENGTH - mp3StatusHistoryCount + i) % MP3_STATUS_HISTORY_LENGTH;
    const Mp3StatusEvent &event = mp3StatusHistory[index];
    Serial.printf("[MP3StatusHistory] #%u | code=%d (%s) | message=%s | timestampMs=%lu | runtimeMs=%lu | sinceLastMs=%lu\n",
                  static_cast<unsigned int>(i + 1),
                  event.code,
                  mp3ErrorCodeToString(event.code),
                  event.message.c_str(),
                  event.timestampMs,
                  event.runtimeMs,
                  event.sinceLastMs);
  }
}

void logRecentStreamStatusEvents(const char *context) {
  const char *label = context ? context : "(no context)";
  if (streamStatusHistoryCount == 0) {
    Serial.printf("[StreamStatusHistory] %s | no events recorded\n", label);
    return;
  }

  Serial.printf("[StreamStatusHistory] %s | count=%u\n",
                label,
                static_cast<unsigned int>(streamStatusHistoryCount));

  for (size_t i = 0; i < streamStatusHistoryCount; ++i) {
    size_t index = (streamStatusHistoryNextIndex + STREAM_STATUS_HISTORY_LENGTH - streamStatusHistoryCount + i) % STREAM_STATUS_HISTORY_LENGTH;
    const StreamStatusEvent &event = streamStatusHistory[index];
    Serial.printf("[StreamStatusHistory] #%u | code=%d (%s) | message=%s | timestampMs=%lu | runtimeMs=%lu | sinceLastMs=%lu\n",
                  static_cast<unsigned int>(i + 1),
                  event.code,
                  streamSourceStatusToString(event.code),
                  event.message.c_str(),
                  event.timestampMs,
                  event.runtimeMs,
                  event.sinceLastMs);
  }
}

const char *streamSourceStatusToString(int code) {
  switch (code) {
    case 0:
      return "Idle";
    case 1:
      return "Connecting";
    case 2:
      return "Connected";
    case 3:
      return "Stream disconnected";
    case 4:
      return "Reconnect attempt";
    case 5:
      return "Reconnect success";
    case 6:
      return "No stream data";
    default:
      return "Unknown";
  }
}

const char *mp3ErrorCodeToString(int code) {
  switch (code) {
    case MAD_ERROR_NONE:
      return "MAD_ERROR_NONE";
    case MAD_ERROR_BUFLEN:
      return "MAD_ERROR_BUFLEN";
    case MAD_ERROR_BUFPTR:
      return "MAD_ERROR_BUFPTR";
    case MAD_ERROR_NOMEM:
      return "MAD_ERROR_NOMEM";
    case MAD_ERROR_LOSTSYNC:
      return "MAD_ERROR_LOSTSYNC";
    case MAD_ERROR_BADLAYER:
      return "MAD_ERROR_BADLAYER";
    case MAD_ERROR_BADBITRATE:
      return "MAD_ERROR_BADBITRATE";
    case MAD_ERROR_BADSAMPLERATE:
      return "MAD_ERROR_BADSAMPLERATE";
    case MAD_ERROR_BADEMPHASIS:
      return "MAD_ERROR_BADEMPHASIS";
    case MAD_ERROR_BADCRC:
      return "MAD_ERROR_BADCRC";
    case MAD_ERROR_BADBITALLOC:
      return "MAD_ERROR_BADBITALLOC";
    case MAD_ERROR_BADSCALEFACTOR:
      return "MAD_ERROR_BADSCALEFACTOR";
    case MAD_ERROR_BADMODE:
      return "MAD_ERROR_BADMODE";
    case MAD_ERROR_BADFRAMELEN:
      return "MAD_ERROR_BADFRAMELEN";
    case MAD_ERROR_BADBIGVALUES:
      return "MAD_ERROR_BADBIGVALUES";
    case MAD_ERROR_BADBLOCKTYPE:
      return "MAD_ERROR_BADBLOCKTYPE";
    case MAD_ERROR_BADSCFSI:
      return "MAD_ERROR_BADSCFSI";
    case MAD_ERROR_BADDATAPTR:
      return "MAD_ERROR_BADDATAPTR";
    case MAD_ERROR_BADPART3LEN:
      return "MAD_ERROR_BADPART3LEN";
    case MAD_ERROR_BADHUFFTABLE:
      return "MAD_ERROR_BADHUFFTABLE";
    case MAD_ERROR_BADHUFFDATA:
      return "MAD_ERROR_BADHUFFDATA";
    case MAD_ERROR_BADSTEREO:
      return "MAD_ERROR_BADSTEREO";
    default:
      break;
  }

  switch (code) {
    case AudioFileSourceHTTPStream::STATUS_HTTPFAIL:
      return "HTTP_STATUS_FAIL";
    case AudioFileSourceHTTPStream::STATUS_DISCONNECTED:
      return "HTTP_STATUS_DISCONNECTED";
    case AudioFileSourceHTTPStream::STATUS_RECONNECTING:
      return "HTTP_STATUS_RECONNECTING";
    case AudioFileSourceHTTPStream::STATUS_RECONNECTED:
      return "HTTP_STATUS_RECONNECTED";
    case AudioFileSourceHTTPStream::STATUS_NODATA:
      return "HTTP_STATUS_NODATA";
    default:
      return "Unknown";
  }
}

void recordMp3StatusEvent(int code, const char *message) {
  unsigned long now = millis();
  unsigned long runtimeMs = streamingStartMillis ? (now - streamingStartMillis) : 0;
  unsigned long sinceLastMs = 0;
  if (mp3StatusHistoryCount > 0) {
    size_t lastIndex = (mp3StatusHistoryNextIndex + MP3_STATUS_HISTORY_LENGTH - 1) % MP3_STATUS_HISTORY_LENGTH;
    sinceLastMs = now - mp3StatusHistory[lastIndex].timestampMs;
  }

  Mp3StatusEvent &event = mp3StatusHistory[mp3StatusHistoryNextIndex];
  event.code = code;
  event.timestampMs = now;
  event.runtimeMs = runtimeMs;
  event.sinceLastMs = sinceLastMs;
  event.message = message ? String(message) : String("(null)");

  mp3StatusHistoryNextIndex = (mp3StatusHistoryNextIndex + 1) % MP3_STATUS_HISTORY_LENGTH;
  if (mp3StatusHistoryCount < MP3_STATUS_HISTORY_LENGTH) {
    mp3StatusHistoryCount++;
  }
}

void recordStreamStatusEvent(int code, const char *message) {
  unsigned long now = millis();
  unsigned long runtimeMs = streamingStartMillis ? (now - streamingStartMillis) : 0;
  unsigned long sinceLastMs = 0;
  if (streamStatusHistoryCount > 0) {
    size_t lastIndex = (streamStatusHistoryNextIndex + STREAM_STATUS_HISTORY_LENGTH - 1) % STREAM_STATUS_HISTORY_LENGTH;
    sinceLastMs = now - streamStatusHistory[lastIndex].timestampMs;
  }

  StreamStatusEvent &event = streamStatusHistory[streamStatusHistoryNextIndex];
  event.code = code;
  event.timestampMs = now;
  event.runtimeMs = runtimeMs;
  event.sinceLastMs = sinceLastMs;
  event.message = message ? String(message) : String("(null)");

  streamStatusHistoryNextIndex = (streamStatusHistoryNextIndex + 1) % STREAM_STATUS_HISTORY_LENGTH;
  if (streamStatusHistoryCount < STREAM_STATUS_HISTORY_LENGTH) {
    streamStatusHistoryCount++;
  }
}

void mp3StatusCallback(void *cbData, int code, const char *string) {
  (void)cbData;
  Serial.printf("[MP3Status] code=%d (%s) | message=%s\n",
                code,
                mp3ErrorCodeToString(code),
                string ? string : "(null)");
  recordMp3StatusEvent(code, string);
  logStreamComponents("MP3 status callback");
}

void streamSourceStatusCallback(void *cbData, int code, const char *string) {
  (void)cbData;
  unsigned long now = millis();
  unsigned long sinceStreamStart = streamingStartMillis ? (now - streamingStartMillis) : 0;
  unsigned long sinceLastCallback = streamSourceStats.lastCallbackMs ? (now - streamSourceStats.lastCallbackMs) : 0;

  if (streamSourceStats.totalCallbacks == 0) {
    streamSourceStats.firstCallbackMs = now;
  }

  streamSourceStats.totalCallbacks++;

  if (streamSourceStats.lastCode == code) {
    streamSourceStats.consecutiveSameCode++;
  } else {
    streamSourceStats.lastCode = code;
    streamSourceStats.consecutiveSameCode = 1;
    streamSourceStats.lastCodeChangeMs = now;
  }

  switch (code) {
    case 3:
      streamSourceStats.disconnectEvents++;
      break;
    case 4:
      streamSourceStats.reconnectAttempts++;
      break;
    case 5:
      streamSourceStats.reconnectSuccesses++;
      break;
    case 6:
      streamSourceStats.noDataEvents++;
      streamSourceStats.consecutiveNoData++;
      streamSourceStats.lastNoDataMs = now;
      break;
    default:
      streamSourceStats.consecutiveNoData = 0;
      break;
  }

  if (code != 6) {
    streamSourceStats.consecutiveNoData = 0;
  }

  Serial.printf("[StreamSourceStatus] code=%d (%s) | message=%s | callbacks=%lu | consecutiveSame=%lu | consecutiveNoData=%lu | sinceStart=%lums | sinceLast=%lums\n",
                code,
                streamSourceStatusToString(code),
                string ? string : "(null)",
                static_cast<unsigned long>(streamSourceStats.totalCallbacks),
                static_cast<unsigned long>(streamSourceStats.consecutiveSameCode),
                static_cast<unsigned long>(streamSourceStats.consecutiveNoData),
                sinceStreamStart,
                sinceLastCallback);
  recordStreamStatusEvent(code, string);
  logStreamReadProgress("Stream source status callback", true);

  if (streamFile) {
    Serial.printf("[StreamSourceStatus] StreamState | isOpen=%s | pos=%u | size=%u | WiFiRSSI=%d | channel=%d\n",
                  streamFile->isOpen() ? "true" : "false",
                  streamFile->getPos(),
                  streamFile->getSize(),
                  WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
                  WiFi.status() == WL_CONNECTED ? WiFi.channel() : -1);
  } else {
    Serial.println("[StreamSourceStatus] StreamState | streamFile pointer is null");
  }

  Serial.printf("[StreamSourceStatus] MP3 running=%s | audioOutput=%p\n",
                (mp3 && mp3->isRunning()) ? "true" : "false",
                audioOutput);

  if (code == 6) {
    Serial.println("[StreamSourceStatus] Detected 'no data' condition. Logging WiFi and stream state.");
    logWifiDetails("Stream source no data");
    logStreamingState("Stream source no data");
    logStreamBufferState("Stream source no data");
    logRecentStreamStatusEvents("Stream source no data");
    logRecentMp3StatusEvents("Stream source no data");
    if (streamSourceStats.consecutiveNoData % 3 == 0) {
      logHeapUsage("Stream source repeated no data");
      logStreamSourceSummary("Stream source repeated no data");
    }
  } else if (code == 3 || code == 4) {
    Serial.println("[StreamSourceStatus] Connection instability detected. Capturing diagnostics.");
    logWifiDetails("Stream source reconnect/ disconnect");
    logStreamingState("Stream source reconnect/ disconnect");
    logStreamSourceSummary("Stream source reconnect/ disconnect");
    logStreamBufferState("Stream source reconnect/ disconnect");
    logRecentStreamStatusEvents("Stream source reconnect/ disconnect");
    logRecentMp3StatusEvents("Stream source reconnect/ disconnect");
  }

  if (code == 2 || code == 3 || code == 4 || code == 5 || code == 6) {
    logStreamComponents("Stream source status callback");
    logStreamBufferState("Stream source status callback");
  }

  streamSourceStats.lastCallbackMs = now;
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
