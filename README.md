# ESP32 Streamer FNK0103

A minimal Arduino sketch for the Freenove ESP32 4.0" display board (FNK0103) that prepares the hardware for an Icecast audio
player. The application now focuses on a single touch control so you can validate WiFi connectivity and the touch interface
before adding streaming logic.

## Features

- **Single touch toggle** – a single large on-screen button flips between "Start Stream" and "Stop Stream" states.
- **WiFi status feedback** – connection progress and IP information are displayed directly on the TFT.
- **Touch calibration hooks** – the resistive touch calibration values remain configurable via macros, matching the original
  Freenove examples.

## Getting Started

1. Update `config.h` with your WiFi SSID and password.
2. Ensure the [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) library is configured for the FNK0103 display (set the
   `FNK0103S_4P0_320x480_ST7796` panel in `User_Setup_Select.h`).
3. Open `freenove.ino` in the Arduino IDE (or use `arduino-cli`) and select your ESP32 board/port.
4. Compile and upload the sketch. Once running, tap the button to toggle the stream state placeholder.

## Next Steps

With the UI pared back, the project is ready for audio features:

- Add Icecast stream configuration (URL, buffering and metadata handling).
- Configure the I2S amplifier pins defined in `config.h` for audio output.
- Replace the placeholder toggle handler with logic that starts/stops the Icecast audio pipeline.

## Codex/Codespace Environment

The `CodexEnvironment` script installs `arduino-cli`, the ESP32 board support package and the TFT_eSPI library. Run it in a new
container to provision everything needed to compile the simplified sketch.
