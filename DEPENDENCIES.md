# Build dependencies

- Espressif Arduino core: **3.3.0**
- Adafruit GFX Library@1.11.11
- Adafruit SSD1306@2.5.13
- Adafruit NeoPixel@1.12.3
- ArduinoJson@7.3.0

The build workflow pins these versions. Do not install a separate I2S library for the AI Assistant; `ESP_I2S.h` comes from the ESP32 core. Hardware behavior still requires testing on the physical build.
