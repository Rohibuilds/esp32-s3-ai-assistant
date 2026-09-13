# ESP32-S3 AI Assistant
**Rohi | RA TECH** · Robotics, electronics and embedded systems

Push-button voice assistant using an INMP441 microphone, Gemini transcription and answers, and streamed speech through a MAX98357A.

**[Open the complete code](firmware/RA_AI_ESP32S3_Gemini_FAST_SILENT_V6/RA_AI_ESP32S3_Gemini_FAST_SILENT_V6.ino) · [Wiring and parts](hardware/README.md) · [How to build](docs/README.md)**

## What is included
2.5-second voice recording, strict transcription before answering, short conversation history, OLED/WS2812 states and muted I2S transitions.

This repository restores the previously delivered project source and repairs identified software issues. It is a hardware prototype; source restoration does not constitute a new hardware test.

## Get started
1. Download the repository using **Code → Download ZIP** and extract it.
2. Read the [wiring table](hardware/README.md); it retains the recovered pin map.
3. Follow the [build and configuration guide](docs/README.md).
4. Open `firmware/RA_AI_ESP32S3_Gemini_FAST_SILENT_V6/RA_AI_ESP32S3_Gemini_FAST_SILENT_V6.ino` in Arduino IDE. Keep the containing folder and companion headers together.

## Code and validation
- Main source: [RA_AI_ESP32S3_Gemini_FAST_SILENT_V6.ino](firmware/RA_AI_ESP32S3_Gemini_FAST_SILENT_V6/RA_AI_ESP32S3_Gemini_FAST_SILENT_V6.ino)
- [Dependency versions](DEPENDENCIES.md)
- [Fixes and validation record](docs/VALIDATION.md)
- [Build workflow](.github/workflows/build.yml) / [live build results](https://github.com/Rohibuilds/esp32-s3-ai-assistant/actions)

## Source provenance
Recovered from the earlier RA TECH deliverables `RA_AI_ESP32S3_Gemini_FAST_SILENT_V6.ino and RA_AI_ESP32S3_Complete_Wiring_Guide.pdf`. The wiring was cross-checked against those files. This update preserves the project's original purpose and identifies later repairs separately.

## Recent repairs
Moved credentials to an ignored configuration file; enabled TLS certificate verification and clock sync; corrected the Interactions JSON schema format; bounded microphone, playback and response loops; parsed TTS events as JSON; preserved PCM samples across odd-byte chunk boundaries; muted the amplifier on error paths.
