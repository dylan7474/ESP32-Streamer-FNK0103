#ifndef CONFIG_H
#define CONFIG_H

// WiFi credentials
#define WIFI_SSID "YOUR-SSID"
#define WIFI_PASSWORD "YOUR-PASSWORD"

// Audio streaming configuration
#define STREAM_URL "http://192.168.50.4:8000/airband.mp3"

// Audio output configuration
// For the onboard speaker amplifier the ESP32's internal DAC (GPIO25) is used, so
// leave the BCLK/LRCLK pins at -1 and only set the data pin.  If you wire an
// external I2S amplifier provide all three pin numbers so the firmware switches
// to true I2S mode.
#define I2S_SPEAKER_BCLK_PIN -1
#define I2S_SPEAKER_LRCLK_PIN -1
#define I2S_SPEAKER_DATA_PIN 25

#endif // CONFIG_H
