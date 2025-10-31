/*
 * ESP32-S3-AUDIO-Board: Gemini Voice Assistant
 *
 * This is a skeleton application demonstrating the full flow for a
 * voice-only Gemini assistant.
 *
 * Hardware Connections (from pinout):
 * - I2C (Codec Control): SDA=11, SCL=10
 * - I2S (Codec Data):   MCLK=12, BCLK=13, LRCK=14, DOUT=15 (Speaker), DIN=16 (Mic)
 * - Speaker Amp Enable:  PA_EN=8
 * - User Button:         KEY1=0
 * - Status LED:          RGB_LED=38 (WS2812)
 *
 * This code is modular, following your preferences. You will need to install:
 * 1. ArduinoJson
 * 2. Adafruit_NeoPixel
 * 3. A Base64 library (e.g., "Base64" by Arturo R." from the Library Manager)
 * 4. A library for the ES8311 codec, or use manual I2C commands.
 * (e.g., `pschatzmann/arduino-audio-driver`)
 */

// === LIBRARIES ===
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include "driver/i2s.h"
#include "Base64.h" // For encoding/decoding API data

// Optional but helpful ESP32 headers
#include "esp_err.h"
#include "esp_system.h"

// The Arduino toolchain supports much of the C++ standard library on ESP32.
#include <memory>
#include <new>
#include <cstring>

// === HARDWARE DEFINITIONS ===
// Wi-Fi Credentials
constexpr const char* WIFI_SSID = "YOUR_WIFI_SSID";
constexpr const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// Gemini API
const String GEMINI_API_KEY = ""; // Leave as-is, will be populated by the environment
const String GEMINI_STT_LLM_URL = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash-preview-09-2025:generateContent?key=" + GEMINI_API_KEY;
const String GEMINI_TTS_URL = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash-preview-tts:generateContent?key=" + GEMINI_API_KEY;

// Pin Definitions
#define I2S_MCLK_PIN 12
#define I2S_BCLK_PIN 13
#define I2S_LRCK_PIN 14
#define I2S_DOUT_PIN 15 // To Speaker
#define I2S_DIN_PIN  16 // From Mic
#define I2C_SDA_PIN  11
#define I2C_SCL_PIN  10
#define PA_EN_PIN    8  // Speaker Amp Enable
#define BUTTON_PIN   0  // KEY1
#define LED_PIN      38
#define LED_COUNT    7  // 7 RGB LEDs on the board

// === AUDIO CONFIGURATION ===
// NOTE: 16000Hz is a common sample rate for STT
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint8_t BITS_PER_SAMPLE = 16;
constexpr uint32_t RECORD_DURATION_SEC = 5;
// Buffer size: 5 seconds * 16000 samples/sec * 16 bits/sample * 1 channel / 8 bits/byte
constexpr size_t AUDIO_BUFFER_SIZE =
    static_cast<size_t>(RECORD_DURATION_SEC) * SAMPLE_RATE * (BITS_PER_SAMPLE / 8);

constexpr uint8_t I2S_DMA_BUF_COUNT = 8;
constexpr uint16_t I2S_DMA_BUF_LEN = 1024;

constexpr uint32_t HTTP_REQUEST_TIMEOUT_MS = 60000;
constexpr size_t TTS_RESPONSE_DOC_CAPACITY = 16384;

// === GLOBAL OBJECTS ===
// State Machine
enum AppState {
  STATE_IDLE,
  STATE_LISTENING,
  STATE_PROCESSING,
  STATE_SPEAKING
};
volatile AppState currentState = STATE_IDLE;
volatile bool buttonPressed = false;
volatile bool wifiReady = false;

// Hardware Objects
Adafruit_NeoPixel pixels(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
WiFiClientSecure client; // Use WiFiClientSecure for HTTPS

constexpr uint32_t COLOR_OFF = 0;
constexpr uint32_t COLOR_LISTENING = Adafruit_NeoPixel::Color(0, 0, 255);
constexpr uint32_t COLOR_PROCESSING = Adafruit_NeoPixel::Color(255, 255, 0);
constexpr uint32_t COLOR_SPEAKING = Adafruit_NeoPixel::Color(0, 255, 0);
constexpr uint32_t COLOR_ERROR = Adafruit_NeoPixel::Color(255, 0, 0);

// Buffers
// We need one buffer for recording...
uint8_t recordingBuffer[AUDIO_BUFFER_SIZE];
// ...and another for playback.
// This is dynamically allocated in this example to save RAM,
// but a static buffer is safer if you have memory.
uint8_t* playbackBuffer = NULL;
size_t playbackBufferSize = 0;


// === FUNCTION PROTOTYPES ===
void setupWifi();
void setupHardware();
void setupI2S_Mic();
void setupI2S_Speaker();
void configureCodec();
void handleWifiEvent(WiFiEvent_t event);
bool recordAudio(uint8_t* buffer, size_t bufferSize);
bool playAudio(uint8_t* buffer, size_t bufferSize);
bool ensureWifiConnected(uint32_t timeoutMs = 10000);
void configureSecureClient(WiFiClientSecure& secureClient);
String callGeminiForAnswer(uint8_t* audioBuffer, size_t audioSize);
bool callGeminiTTS(const String& text);
void setLedColor(uint32_t color);
void releasePlaybackBuffer();
bool beginGeminiRequest(HTTPClient& http, WiFiClientSecure& secureClient,
                        const String& url, const char* contextLabel);
void logGeminiError(JsonVariantConst errorNode, const char* contextLabel);
void logHeapStatus(const char* contextLabel);
void logHttpResponsePreview(const char* contextLabel, const String& body);
#ifdef ENABLE_RTOS_VARIANT
void handleButtonPressFromISR();
#endif
void IRAM_ATTR onButtonPress();

// ===================================
//  SETUP & MAIN LOOP
// ===================================

#ifndef ENABLE_RTOS_VARIANT
void setup() {
  Serial.begin(115200);
  Serial.println("Gemini Voice Assistant Booting...");
  logHeapStatus("Boot");

  setupHardware();
  setupWifi();
  configureCodec(); // IMPORTANT: Must configure the ES8311
}

void loop() {
  if (!ensureWifiConnected()) {
    setLedColor(COLOR_ERROR);
    delay(100);
    return;
  }

  switch (currentState) {
    case STATE_IDLE:
      if (buttonPressed) {
        buttonPressed = false; // Consume the flag
        Serial.println("Button pressed, moving to LISTENING");
        currentState = STATE_LISTENING;
      }
      break;

    case STATE_LISTENING:
      setLedColor(COLOR_LISTENING); // Blue
      Serial.println("Listening...");

      // Re-configure I2S for Mic
      setupI2S_Mic();

      if (recordAudio(recordingBuffer, AUDIO_BUFFER_SIZE)) {
        Serial.println("Recording complete.");
        currentState = STATE_PROCESSING;
      } else {
        Serial.println("Recording failed.");
        currentState = STATE_IDLE;
        setLedColor(0); // Off
      }
      break;

    case STATE_PROCESSING:
      setLedColor(COLOR_PROCESSING); // Yellow
      Serial.println("Processing... Calling Gemini API...");

      // Step 1: STT + LLM
      String geminiAnswer;
      geminiAnswer = callGeminiForAnswer(recordingBuffer, AUDIO_BUFFER_SIZE);

      if (geminiAnswer.length() > 0) {
        Serial.printf("Gemini Answer: %s\n", geminiAnswer.c_str());

        // Step 2: TTS
        Serial.println("Calling Gemini TTS...");
        if (callGeminiTTS(geminiAnswer)) {
          Serial.println("TTS audio received.");
          currentState = STATE_SPEAKING;
        } else {
          Serial.println("TTS API failed.");
          currentState = STATE_IDLE;
          setLedColor(COLOR_OFF); // Off
        }
      } else {
        Serial.println("STT/LLM API failed.");
        currentState = STATE_IDLE;
        setLedColor(COLOR_OFF); // Off
      }
      break;

    case STATE_SPEAKING:
      setLedColor(COLOR_SPEAKING); // Green
      Serial.println("Speaking...");

      // Re-configure I2S for Speaker
      setupI2S_Speaker();
      
      if (playbackBuffer != NULL && playbackBufferSize > 0) {
        playAudio(playbackBuffer, playbackBufferSize);
        releasePlaybackBuffer();
      } else {
        Serial.println("No playback buffer to speak!");
      }

      Serial.println("Playback complete, returning to IDLE.");
      currentState = STATE_IDLE;
      setLedColor(COLOR_OFF); // Off
      break;
  }
  delay(10); // Small delay to prevent watchdog timeout
}
#endif  // ENABLE_RTOS_VARIANT

// ===================================
//  INITIALIZATION FUNCTIONS
// ===================================

void setupHardware() {
  // Button
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), onButtonPress, FALLING);

  // Speaker Amp
  pinMode(PA_EN_PIN, OUTPUT);
  digitalWrite(PA_EN_PIN, LOW); // Off by default

  // LEDs
  pixels.begin();
  pixels.clear();
  pixels.show();

  // I2C for Codec
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  logHeapStatus("Hardware-Ready");
}

void setupWifi() {
  WiFi.onEvent(handleWifiEvent);
  Serial.print("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  ensureWifiConnected();
  configureSecureClient(client);
  logHeapStatus("WiFi Ready");
}

/**
 * @brief Configures the ES8311 Codec via I2C
 *
 * THIS IS A CRITICAL STUB! The ES8311 requires a specific
 * sequence of I2C commands to set up its clocks, ADC, DAC,
 * and paths.
 *
 * You MUST implement this, likely by using a library like
 * `pschatzmann/arduino-audio-driver` or by porting
 * initialization values from an ESP-IDF example.
 */
void configureCodec() {
  Serial.println("Configuring ES8311 Codec (STUB)...");
  // Example using Wire.h (psuedo-code)
  // Wire.beginTransmission(ES8311_I2C_ADDR);
  // Wire.write(ES8311_REG_RESET);
  // Wire.write(0x00);
  // Wire.endTransmission();
  // ... many more commands ...
  Serial.println("Codec configuration is a STUB. Please implement!");
}

/**
 * @brief Configures the I2S peripheral for Microphone (Read)
 */
void setupI2S_Mic() {
  // Uninstall previous driver
  i2s_driver_uninstall(I2S_NUM_0);

  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = I2S_DMA_BUF_COUNT,
    .dma_buf_len = I2S_DMA_BUF_LEN,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK_PIN,
    .ws_io_num = I2S_LRCK_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE, // Not used for RX
    .data_in_num = I2S_DIN_PIN,
    .mck_io_num = I2S_MCLK_PIN
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  i2s_set_clk(I2S_NUM_0, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
}

/**
 * @brief Configures the I2S peripheral for Speaker (Write)
 */
void setupI2S_Speaker() {
  // Uninstall previous driver
  i2s_driver_uninstall(I2S_NUM_0);
  
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE, // TTS audio will be 16kHz
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = I2S_DMA_BUF_COUNT,
    .dma_buf_len = I2S_DMA_BUF_LEN,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK_PIN,
    .ws_io_num = I2S_LRCK_PIN,
    .data_out_num = I2S_DOUT_PIN,
    .data_in_num = I2S_PIN_NO_CHANGE, // Not used for TX
    .mck_io_num = I2S_MCLK_PIN
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  i2s_set_clk(I2S_NUM_0, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
}


// ===================================
//  AUDIO HANDLING FUNCTIONS
// ===================================

/**
 * @brief Records audio from I2S mic into the buffer.
 * This is a simple blocking implementation.
 */
bool recordAudio(uint8_t* buffer, size_t bufferSize) {
  Serial.printf("Starting recording (%u bytes buffer)...\n", static_cast<unsigned>(bufferSize));
  logHeapStatus("Record-Before");
  const uint32_t startMs = millis();
  size_t bytes_read = 0;
  size_t total_bytes_read = 0;

  while (total_bytes_read < bufferSize) {
    esp_err_t result = i2s_read(I2S_NUM_0, buffer + total_bytes_read, bufferSize - total_bytes_read, &bytes_read, portMAX_DELAY);
    if (result != ESP_OK) {
      Serial.printf("I2S Read Error: %d\n", result);
      return false;
    }
    total_bytes_read += bytes_read;
  }

  Serial.printf("Finished recording %u bytes in %lu ms.\n", static_cast<unsigned>(total_bytes_read), millis() - startMs);
  logHeapStatus("Record-After");
  return true;
}

/**
 * @brief Plays audio from the buffer to the I2S speaker.
 * This is a simple blocking implementation.
 */
bool playAudio(uint8_t* buffer, size_t bufferSize) {
  Serial.printf("Starting playback (%u bytes)...\n", static_cast<unsigned>(bufferSize));
  logHeapStatus("Playback-Before");
  digitalWrite(PA_EN_PIN, HIGH); // Turn on the amp
  delay(10); // Give amp time to stabilize

  size_t bytes_written = 0;
  esp_err_t result = i2s_write(I2S_NUM_0, buffer, bufferSize, &bytes_written, portMAX_DELAY);

  digitalWrite(PA_EN_PIN, LOW); // Turn off the amp

  if (result != ESP_OK) {
    Serial.printf("I2S Write Error: %d\n", result);
    return false;
  }
  if (bytes_written < bufferSize) {
    Serial.printf("Warning: Only wrote %d of %d bytes\n", bytes_written, bufferSize);
  }

  Serial.printf("Finished playback, wrote %u bytes.\n", static_cast<unsigned>(bytes_written));
  logHeapStatus("Playback-After");
  return true;
}


bool beginGeminiRequest(HTTPClient& http, WiFiClientSecure& secureClient,
                        const String& url, const char* contextLabel) {
  configureSecureClient(secureClient);
  http.setConnectTimeout(HTTP_REQUEST_TIMEOUT_MS);
  http.setTimeout(HTTP_REQUEST_TIMEOUT_MS);
  if (!http.begin(secureClient, url)) {
    Serial.printf("Failed to start HTTP connection for %s.\n", contextLabel);
    return false;
  }
  Serial.printf("[%s] HTTP client ready.\n", contextLabel);
  http.addHeader("Content-Type", "application/json");
  return true;
}

void logGeminiError(JsonVariantConst errorNode, const char* contextLabel) {
  if (errorNode.isNull()) {
    return;
  }

  Serial.printf("%s API error", contextLabel);
  const char* status = errorNode["status"] | nullptr;
  if (status != nullptr) {
    Serial.printf(" (%s)", status);
  }
  Serial.print(": ");

  if (errorNode["message"].is<const char*>()) {
    Serial.println(errorNode["message"].as<const char*>());
  } else {
    serializeJson(errorNode, Serial);
    Serial.println();
  }
}

void logHeapStatus(const char* contextLabel) {
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t minHeap = ESP.getMinFreeHeap();
  Serial.printf("[%s] Heap free: %u bytes (min: %u)\n", contextLabel, static_cast<unsigned>(freeHeap),
                static_cast<unsigned>(minHeap));
}

void logHttpResponsePreview(const char* contextLabel, const String& body) {
  if (body.isEmpty()) {
    Serial.printf("[%s] Empty HTTP body.\n", contextLabel);
    return;
  }

  const size_t previewLen = body.length() < 160 ? body.length() : 160;
  String preview = body.substring(0, previewLen);
  preview.replace("\n", " ");
  if (body.length() > previewLen) {
    preview += "...";
  }
  Serial.printf("[%s] HTTP body preview (%u bytes total): %s\n", contextLabel, static_cast<unsigned>(body.length()),
                preview.c_str());
}

void releasePlaybackBuffer() {
  if (playbackBuffer != nullptr) {
    Serial.printf("Releasing playback buffer (%u bytes).\n", static_cast<unsigned>(playbackBufferSize));
    free(playbackBuffer);
    playbackBuffer = nullptr;
  }
  playbackBufferSize = 0;
}

// ===================================
//  GEMINI API FUNCTIONS
// ===================================

/**
 * @brief Sends audio to Gemini, gets text response.
 */
String callGeminiForAnswer(uint8_t* audioBuffer, size_t audioSize) {
  // 1. Encode audio to Base64
  const size_t encodedLength = base64_enc_len(audioSize);
  std::unique_ptr<char[]> base64Data(new (std::nothrow) char[encodedLength + 1]);
  if (!base64Data) {
    Serial.println("Failed to allocate memory for base64 encoding.");
    return "";
  }
  Base64.encode(base64Data.get(), reinterpret_cast<char*>(audioBuffer), audioSize);
  base64Data[encodedLength] = '\0';
  String base64String = String(base64Data.get());
  Serial.printf("[STT/LLM] Encoded audio length: %u bytes (base64).\n", static_cast<unsigned>(base64String.length()));
  logHeapStatus("STT-AfterEncode");

  // 2. Create JSON Payload
  // Use ArduinoJson Assistant to calculate size:
  // https://arduinojson.org/v6/assistant/
  // For large audio, we must use DynamicJsonDocument
  // The payload will be ~1.33x the audio size + overhead
  const size_t jsonCapacity = JSON_OBJECT_SIZE(1) + JSON_OBJECT_SIZE(1) + JSON_ARRAY_SIZE(2) +
                              JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(2) + base64String.length() + 256;
  DynamicJsonDocument payload(jsonCapacity);

  JsonObject contents_0 = payload.createNestedObject("contents");
  JsonArray parts = contents_0.createNestedArray("parts");

  JsonObject parts_0 = parts.createNestedObject();
  parts_0["text"] = "Transcribe this audio and provide a concise answer.";

  JsonObject parts_1 = parts.createNestedObject();
  JsonObject inlineData = parts_1.createNestedObject("inlineData");
  inlineData["mimeType"] = "audio/l16;rate=16000"; // LINEAR16 PCM
  inlineData["data"] = base64String;

  String jsonPayload;
  serializeJson(payload, jsonPayload);
  base64String = ""; // Free memory
  Serial.printf("[STT/LLM] JSON payload length: %u bytes.\n", static_cast<unsigned>(jsonPayload.length()));
  logHeapStatus("STT-BeforePOST");

  // 3. Make HTTPS POST request
  String responseText = "";
  WiFiClientSecure secureClient;
  HTTPClient http;
  if (!beginGeminiRequest(http, secureClient, GEMINI_STT_LLM_URL, "STT/LLM")) {
    return responseText;
  }

  int httpCode = http.POST(jsonPayload);
  String response;

  if (httpCode > 0) {
    response = http.getString();
    Serial.printf("STT/LLM API Response Code: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      // 4. Parse Response
      DynamicJsonDocument responseDoc(4096);
      DeserializationError error = deserializeJson(responseDoc, response);

      if (error) {
        Serial.print(F("Failed to parse STT/LLM response: "));
        Serial.println(error.f_str());
        logHttpResponsePreview("STT/LLM", response);
      } else if (responseDoc.containsKey("error")) {
        logGeminiError(responseDoc["error"], "STT/LLM");
      } else if (responseDoc.containsKey("candidates") &&
                 responseDoc["candidates"][0].containsKey("content") &&
                 responseDoc["candidates"][0]["content"].containsKey("parts") &&
                 responseDoc["candidates"][0]["content"]["parts"][0].containsKey("text")) {

        responseText = responseDoc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
      } else {
        Serial.println("Could not parse Gemini answer from JSON.");
        logHttpResponsePreview("STT/LLM", response);
      }
    } else {
      Serial.printf("STT/LLM request returned HTTP %d\n", httpCode);
      logHttpResponsePreview("STT/LLM", response);
    }

  } else {
    Serial.printf("STT/LLM API request failed, error: %s\n", http.errorToString(httpCode).c_str());
  }

  logHeapStatus("STT-AfterResponse");
  http.end();
  return responseText;
}

/**
 * @brief Sends text to Gemini TTS, gets audio response.
 * Stores audio in global `playbackBuffer`.
 */
bool callGeminiTTS(const String& text) {
  // 1. Create JSON Payload
  StaticJsonDocument<1024> doc;
  
  JsonObject contents_0 = doc.createNestedObject("contents");
  JsonArray parts = contents_0.createNestedArray("parts");
  JsonObject parts_0 = parts.createNestedObject();
  parts_0["text"] = text;
  
  JsonObject generationConfig = doc.createNestedObject("generationConfig");
  JsonArray responseModalities = generationConfig.createNestedArray("responseModalities");
  responseModalities.add("AUDIO");

  // Optional: Specify voice, but default is fine
  // JsonObject speechConfig = generationConfig.createNestedObject("speechConfig");
  // ...

  String jsonPayload;
  serializeJson(doc, jsonPayload);
  Serial.printf("[TTS] JSON payload length: %u bytes.\n", static_cast<unsigned>(jsonPayload.length()));
  logHeapStatus("TTS-BeforePOST");

  // 2. Make HTTPS POST request
  WiFiClientSecure secureClient;
  HTTPClient http;
  if (!beginGeminiRequest(http, secureClient, GEMINI_TTS_URL, "TTS")) {
    return false;
  }

  int httpCode = http.POST(jsonPayload);
  bool success = false;
  String response;

  if (httpCode > 0) {
    response = http.getString();
    Serial.printf("TTS API Response Code: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      // 3. Parse Response
      // TTS response can be large.
      DynamicJsonDocument responseDoc(TTS_RESPONSE_DOC_CAPACITY);
      DeserializationError error = deserializeJson(responseDoc, response);

      if (error) {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        logHttpResponsePreview("TTS", response);
      } else if (responseDoc.containsKey("error")) {
        logGeminiError(responseDoc["error"], "TTS");
      } else if (responseDoc.containsKey("candidates") &&
                 responseDoc["candidates"][0]["content"].containsKey("parts") &&
                 responseDoc["candidates"][0]["content"]["parts"].size() > 0 &&
                 responseDoc["candidates"][0]["content"]["parts"][0].containsKey("inlineData") &&
                 responseDoc["candidates"][0]["content"]["parts"][0]["inlineData"].containsKey("data")) {

        const char* base64Audio = responseDoc["candidates"][0]["content"]["parts"][0]["inlineData"]["data"];
        const size_t base64Length = strlen(base64Audio);
        Serial.printf("[TTS] Base64 audio length: %u bytes.\n", static_cast<unsigned>(base64Length));

        // 4. Decode Base64 Audio
        const size_t decodedSize = base64_dec_len(base64Audio, base64Length);
        if (decodedSize == 0) {
          Serial.println("Decoded playback buffer has zero length.");
        } else {
          releasePlaybackBuffer();
          playbackBuffer = static_cast<uint8_t*>(malloc(decodedSize));

          if (!playbackBuffer) {
            Serial.println("Failed to allocate memory for playback buffer!");
          } else {
            Base64.decode(reinterpret_cast<char*>(playbackBuffer), base64Audio, base64Length);
            playbackBufferSize = decodedSize;
            Serial.printf("Decoded %d bytes of audio.\n", playbackBufferSize);
            logHeapStatus("TTS-AfterDecode");
            success = true;
          }
        }

      } else {
        Serial.println("Could not parse TTS audio from JSON.");
        logHttpResponsePreview("TTS", response);
      }
    } else {
      Serial.printf("TTS request returned HTTP %d\n", httpCode);
      logHttpResponsePreview("TTS", response);
    }

  } else {
    Serial.printf("TTS API request failed, error: %s\n", http.errorToString(httpCode).c_str());
  }

  logHeapStatus("TTS-AfterResponse");
  http.end();
  return success;
}


// ===================================
//  UTILITY FUNCTIONS
// ===================================

/**
 * @brief Sets all LEDs to a single color.
 */
void setLedColor(uint32_t color) {
  for (int i = 0; i < LED_COUNT; i++) {
    pixels.setPixelColor(i, color);
  }
  pixels.show();
}

/**
 * @brief Interrupt Service Routine for the button.
 * Sets a volatile flag to be handled in the main loop.
 */
void IRAM_ATTR onButtonPress() {
  // Simple de-bounce
  static unsigned long last_interrupt_time = 0;
  unsigned long interrupt_time = millis();
  if (interrupt_time - last_interrupt_time > 200) {
#ifdef ENABLE_RTOS_VARIANT
    handleButtonPressFromISR();
#else
    if (currentState == STATE_IDLE) {
      buttonPressed = true;
    }
#endif
  }
  last_interrupt_time = interrupt_time;
}
void handleWifiEvent(WiFiEvent_t event) {
  switch (event) {
#if defined(ARDUINO_EVENT_WIFI_STA_CONNECTED)
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
#else
    case SYSTEM_EVENT_STA_CONNECTED:
#endif
      Serial.println("\nWiFi connected, waiting for IP...");
      break;
#if defined(ARDUINO_EVENT_WIFI_STA_GOT_IP)
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
#else
    case SYSTEM_EVENT_STA_GOT_IP:
#endif
      Serial.printf("WiFi ready. IP address: %s\n", WiFi.localIP().toString().c_str());
      wifiReady = true;
      logHeapStatus("WiFi-IP");
      break;
#if defined(ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
#else
    case SYSTEM_EVENT_STA_DISCONNECTED:
#endif
      Serial.println("WiFi disconnected. Attempting reconnection...");
      wifiReady = false;
      WiFi.reconnect();
      logHeapStatus("WiFi-Disconnected");
      break;
    default:
      break;
  }
}

bool ensureWifiConnected(uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED && wifiReady) {
    return true;
  }

  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED && wifiReady) {
      return true;
    }
    delay(100);
  }

  Serial.printf("WiFi connection timed out after %lu ms (status=%d, ready=%d).\n", millis() - start,
                static_cast<int>(WiFi.status()), wifiReady ? 1 : 0);
  return WiFi.status() == WL_CONNECTED;
}

void configureSecureClient(WiFiClientSecure& secureClient) {
  // For HTTPS
  // Note: Skipping root CA cert validation for simplicity.
  // In production, you should add the Google Root CA.
  secureClient.setInsecure();
  secureClient.setHandshakeTimeout(30);
  secureClient.setTimeout(HTTP_REQUEST_TIMEOUT_MS / 1000);
  secureClient.setBufferSizes(4096, 4096);
}

