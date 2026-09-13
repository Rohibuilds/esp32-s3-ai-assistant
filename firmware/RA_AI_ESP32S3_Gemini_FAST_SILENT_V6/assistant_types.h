#pragma once

#include <Arduino.h>

enum AssistantState {
  STATE_BOOT,
  STATE_CONNECTING,
  STATE_READY,
  STATE_LISTENING,
  STATE_THINKING,
  STATE_SPEAKING,
  STATE_ERROR
};

struct TranscriptionResult {
  String transcript;
  bool speechDetected = false;
  bool ok = false;
};

struct BodyReaderState {
  bool chunked = false;
  long chunkRemaining = -1;
  bool finished = false;
};
