#ifdef ENABLE_RTOS_VARIANT
/*
 * ESP32-S3-AUDIO-Board: Gemini Voice Assistant (FreeRTOS Variant)
 *
 * Enable this file by defining ENABLE_RTOS_VARIANT in your build flags.
 * The cooperative task scheduling in this variant reduces long blocking
 * sections in loop() and allows the audio pipeline, network requests, and
 * playback to run on dedicated tasks.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// Forward declarations of shared functions and globals from voice-assistant.ino
extern void setupHardware();
extern void setupWifi();
extern void configureCodec();
extern void setupI2S_Mic();
extern void setupI2S_Speaker();
extern bool recordAudio(uint8_t* buffer, size_t bufferSize);
extern bool playAudio(uint8_t* buffer, size_t bufferSize);
extern String callGeminiForAnswer(uint8_t* audioBuffer, size_t audioSize);
extern bool callGeminiTTS(const String& text);
extern bool ensureWifiConnected(uint32_t timeoutMs);
extern void setLedColor(uint32_t color);
extern uint8_t recordingBuffer[];
extern uint8_t* playbackBuffer;
extern size_t playbackBufferSize;
extern volatile AppState currentState;
extern void releasePlaybackBuffer();
extern void logHeapStatus(const char* contextLabel);

namespace {

enum class EventType : uint8_t {
  kButtonPressed,
  kRecordingCompleted,
  kRecordingFailed,
  kProcessingCompleted,
  kProcessingFailed,
  kPlaybackCompleted
};

struct AppEvent {
  EventType type;
};

QueueHandle_t g_eventQueue = nullptr;
TaskHandle_t g_stateTaskHandle = nullptr;
TaskHandle_t g_recordingTaskHandle = nullptr;
TaskHandle_t g_processingTaskHandle = nullptr;
TaskHandle_t g_playbackTaskHandle = nullptr;

void postEvent(EventType type) {
  if (g_eventQueue == nullptr) {
    return;
  }
  AppEvent event{type};
  if (xQueueSend(g_eventQueue, &event, pdMS_TO_TICKS(50)) != pdPASS) {
    Serial.println("Event queue full, dropping event.");
    logHeapStatus("EventQueue-Dropped");
  }
}

}  // namespace

void handleButtonPressFromISR() {
  if (g_eventQueue == nullptr) {
    return;
  }
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  AppEvent event{EventType::kButtonPressed};
  if (xQueueSendFromISR(g_eventQueue, &event, &xHigherPriorityTaskWoken) != pdPASS) {
    // Optional: log once the CPU is available again.
  }
  if (xHigherPriorityTaskWoken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

static void recordingTask(void* parameter) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!ensureWifiConnected()) {
      Serial.println("WiFi unavailable, aborting recording.");
      logHeapStatus("RTOS-Record-NoWiFi");
      postEvent(EventType::kRecordingFailed);
      continue;
    }

    setLedColor(COLOR_LISTENING);
    setupI2S_Mic();
    const size_t recordingSize = sizeof(recordingBuffer);
    bool ok = recordAudio(recordingBuffer, recordingSize);
    postEvent(ok ? EventType::kRecordingCompleted : EventType::kRecordingFailed);
  }
}

static void processingTask(void* parameter) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!ensureWifiConnected()) {
      Serial.println("WiFi unavailable, aborting processing.");
      logHeapStatus("RTOS-Process-NoWiFi");
      postEvent(EventType::kProcessingFailed);
      continue;
    }

    setLedColor(COLOR_PROCESSING);
    const size_t recordingSize = sizeof(recordingBuffer);
    String answer = callGeminiForAnswer(recordingBuffer, recordingSize);
    if (answer.length() == 0) {
      Serial.println("Gemini STT/LLM returned no answer.");
      postEvent(EventType::kProcessingFailed);
      continue;
    }

    Serial.printf("Gemini Answer: %s\n", answer.c_str());
    Serial.println("Calling Gemini TTS...");
    if (!callGeminiTTS(answer)) {
      Serial.println("Gemini TTS failed.");
      postEvent(EventType::kProcessingFailed);
      continue;
    }

    postEvent(EventType::kProcessingCompleted);
  }
}

static void playbackTask(void* parameter) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    setLedColor(COLOR_SPEAKING);
    setupI2S_Speaker();

    if (playbackBuffer != nullptr && playbackBufferSize > 0) {
      if (!playAudio(playbackBuffer, playbackBufferSize)) {
        Serial.println("Playback reported an error.");
        logHeapStatus("RTOS-Playback-Error");
      }
      releasePlaybackBuffer();
    } else {
      Serial.println("Playback requested without audio buffer.");
    }

    postEvent(EventType::kPlaybackCompleted);
  }
}

static void stateTask(void* parameter) {
  currentState = STATE_IDLE;
  setLedColor(COLOR_OFF);

  for (;;) {
    AppEvent event{};
    if (xQueueReceive(g_eventQueue, &event, portMAX_DELAY) != pdPASS) {
      continue;
    }

    switch (event.type) {
      case EventType::kButtonPressed:
        if (currentState != STATE_IDLE) {
          break;
        }
        currentState = STATE_LISTENING;
        logHeapStatus("RTOS-State-Listening");
        xTaskNotifyGive(g_recordingTaskHandle);
        break;

      case EventType::kRecordingCompleted:
        currentState = STATE_PROCESSING;
        logHeapStatus("RTOS-State-Processing");
        xTaskNotifyGive(g_processingTaskHandle);
        break;

      case EventType::kRecordingFailed:
        Serial.println("Recording failed, returning to idle.");
        currentState = STATE_IDLE;
        setLedColor(COLOR_ERROR);
        logHeapStatus("RTOS-Record-Failed");
        vTaskDelay(pdMS_TO_TICKS(300));
        setLedColor(COLOR_OFF);
        break;

      case EventType::kProcessingCompleted:
        currentState = STATE_SPEAKING;
        logHeapStatus("RTOS-State-Speaking");
        xTaskNotifyGive(g_playbackTaskHandle);
        break;

      case EventType::kProcessingFailed:
        Serial.println("Processing failed, returning to idle.");
        currentState = STATE_IDLE;
        setLedColor(COLOR_ERROR);
        releasePlaybackBuffer();
        logHeapStatus("RTOS-Process-Failed");
        vTaskDelay(pdMS_TO_TICKS(300));
        setLedColor(COLOR_OFF);
        break;

      case EventType::kPlaybackCompleted:
        Serial.println("Playback complete, returning to IDLE.");
        currentState = STATE_IDLE;
        setLedColor(COLOR_OFF);
        logHeapStatus("RTOS-State-Idle");
        break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Gemini Voice Assistant (RTOS) Booting...");

  setupHardware();
  setupWifi();
  configureCodec();

  g_eventQueue = xQueueCreate(10, sizeof(AppEvent));
  configASSERT(g_eventQueue != nullptr);

  xTaskCreatePinnedToCore(recordingTask, "record", 6144, nullptr, 4, &g_recordingTaskHandle, APP_CPU_NUM);
  xTaskCreatePinnedToCore(processingTask, "process", 8192, nullptr, 3, &g_processingTaskHandle, APP_CPU_NUM);
  xTaskCreatePinnedToCore(playbackTask, "play", 6144, nullptr, 3, &g_playbackTaskHandle, PRO_CPU_NUM);
  xTaskCreatePinnedToCore(stateTask, "state", 4096, nullptr, 2, &g_stateTaskHandle, PRO_CPU_NUM);
}

void loop() {
  // Nothing to do here; tasks handle the workflow.
  vTaskDelay(pdMS_TO_TICKS(1000));
}

#endif  // ENABLE_RTOS_VARIANT
