#ifndef CONFIG_H
#define CONFIG_H

// WiFi credentials
#define WIFI_SSID "YOUR-SSID"
#define WIFI_PASSWORD "YOUR-PASSWORD"

// Audio streaming configuration
#define STREAM_URL "http://192.168.50.4:8000/airband.mp3"

// I2S pin configuration for the speaker connector
// Set the pins that map to the speaker header. Use -1 for pins that are not used.
#define I2S_SPEAKER_BCLK_PIN -1
#define I2S_SPEAKER_LRCLK_PIN -1
#define I2S_SPEAKER_DATA_PIN 25

#endif // CONFIG_H
