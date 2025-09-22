#ifndef CONFIG_H
#define CONFIG_H

// WiFi credentials
#define WIFI_SSID "YOUR-SSID"
#define WIFI_PASSWORD "YOUR-PASSWORD"

// Audio streaming configuration
#define STREAM_URL "http://192.168.50.4:8000/airband.mp3"

// Audio output configuration
// The onboard speaker amplifier is controlled by an enable pin and receives audio
// from the ESP32 using the internal DAC path on GPIO25.
#define AUDIO_AMP_ENABLE_PIN 4
#define I2S_SPEAKER_BCLK_PIN -1
#define I2S_SPEAKER_LRCLK_PIN -1
#define I2S_SPEAKER_DATA_PIN 25

#endif // CONFIG_H
