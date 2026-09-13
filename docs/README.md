# Build the ESP32-S3 AI Assistant
1. Assemble the [original V6 wiring](../hardware/README.md) with power off. Use a stable supply; speaker bursts must not brown out the ESP32.
2. Install Espressif core **3.3.0** and the [pinned libraries](../DEPENDENCIES.md). Select **ESP32S3 Dev Module** and the real serial port. Match flash size and PSRAM type to the module label. If a default application partition is too small, choose **Huge APP** on a board with at least 4 MB flash; this firmware does not implement OTA.
3. Download and extract the whole repository. Open `firmware/RA_AI_ESP32S3_Gemini_FAST_SILENT_V6/RA_AI_ESP32S3_Gemini_FAST_SILENT_V6.ino`.
4. Copy `secrets.example.h` to **secrets.h** in the same folder. Set your Wi-Fi SSID/password and Gemini API key there. The example compiles but cannot connect until configured. Never publish the filled file.
5. The original models are `gemini-3.5-flash-lite` and `gemini-3.1-flash-tts-preview`, with voice `Kore`. They are configurable constants in the sketch. Account access, region, quota and billing determine availability; free access is not guaranteed.
6. Verify and upload. Open Serial Monitor at **115200 baud**. Confirm Wi-Fi connects. The first request synchronizes time so TLS certificates can be verified; permit NTP through your network.
7. Press and release the button. Wait for **SPEAK NOW**, then say a short sentence within the **2.5-second** recording window. Read the recorded RMS/peak, transcription and answer in Serial Monitor.
8. Confirm the amplifier is quiet during recording and I2S direction changes, then verify the spoken reply. Longer input requires adjusting `RECORD_MS`, with corresponding memory and latency costs.

## Troubleshooting
| Symptom | Check |
|---|---|
| No recording / mic timeout | INMP441 3V3, L/R grounded, SCK14, WS15, SD16 |
| No intelligible speech | Speak near the microphone after the prompt; inspect RMS/peak and clipping |
| Loud click at button press | MAX98357A SD/EN to GPIO5; common ground and amplifier decoupling |
| HTTP 400 | Request schema/model configuration; current service error in Serial Monitor |
| HTTP 401/403 | API key and account/model permission |
| HTTP 404 | Model identifier or model retirement |
| HTTP 429 | Quota/rate limit; wait or check account limits |
| TLS connection failure | Clock sync, internet access and current Google root certificates |
| TTS fails after text succeeds | TTS model access, audio stream completion, I2S wiring and supply |
| Missing ESP_I2S.h | Install the specified Espressif 3.x core, not an unrelated I2S library |

The device sends button-triggered microphone recordings and text to Gemini. Avoid recording private speech you do not intend to send. Audio/API operation requires your configured device and was not exercised using your credentials during this repair.

## API references checked during repair
- [Google structured output format](https://ai.google.dev/gemini-api/docs/structured-output)
- [Streaming speech generation](https://ai.google.dev/gemini-api/docs/speech-generation)
- [Model lifecycle](https://ai.google.dev/gemini-api/docs/deprecations)
- [Espressif I2S documentation](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/i2s.html)
