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

// === HARDWARE DEFINITIONS ===
// Wi-Fi Credentials
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

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
#define SAMPLE_RATE 16000
#define BITS_PER_SAMPLE 16
#define RECORD_DURATION_SEC 5
// Buffer size: 5 seconds * 16000 samples/sec * 16 bits/sample * 1 channel / 8 bits/byte
#define AUDIO_BUFFER_SIZE (RECORD_DURATION_SEC * SAMPLE_RATE * (BITS_PER_SAMPLE / 8))

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

// Hardware Objects
Adafruit_NeoPixel pixels(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
HTTPClient http;
WiFiClientSecure client; // Use WiFiClientSecure for HTTPS

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
bool recordAudio(uint8_t* buffer, size_t bufferSize);
bool playAudio(uint8_t* buffer, size_t bufferSize);
String callGeminiForAnswer(uint8_t* audioBuffer, size_t audioSize);
bool callGeminiTTS(String text);
void setLedColor(uint32_t color);
void IRAM_ATTR onButtonPress();

// ===================================
//  SETUP & MAIN LOOP
// ===================================

void setup() {
  Serial.begin(115200);
  Serial.println("Gemini Voice Assistant Booting...");

  setupHardware();
  setupWifi();
  configureCodec(); // IMPORTANT: Must configure the ES8311
}

void loop() {
  switch (currentState) {
    case STATE_IDLE:
      if (buttonPressed) {
        buttonPressed = false; // Consume the flag
        Serial.println("Button pressed, moving to LISTENING");
        currentState = STATE_LISTENING;
      }
      break;

    case STATE_LISTENING:
      setLedColor(pixels.Color(0, 0, 255)); // Blue
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
      setLedColor(pixels.Color(255, 255, 0)); // Yellow
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
          setLedColor(0); // Off
        }
      } else {
        Serial.println("STT/LLM API failed.");
        currentState = STATE_IDLE;
        setLedColor(0); // Off
      }
      break;

    case STATE_SPEAKING:
      setLedColor(pixels.Color(0, 255, 0)); // Green
      Serial.println("Speaking...");

      // Re-configure I2S for Speaker
      setupI2S_Speaker();
      
      if (playbackBuffer != NULL && playbackBufferSize > 0) {
        playAudio(playbackBuffer, playbackBufferSize);
        // Clean up the playback buffer
        free(playbackBuffer);
        playbackBuffer = NULL;
        playbackBufferSize = 0;
      } else {
        Serial.println("No playback buffer to speak!");
      }

      Serial.println("Playback complete, returning to IDLE.");
      currentState = STATE_IDLE;
      setLedColor(0); // Off
      break;
  }
  delay(10); // Small delay to prevent watchdog timeout
}

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
}

void setupWifi() {
  Serial.print("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi!");
  
  // For HTTPS
  // Note: Skipping root CA cert validation for simplicity.
  // In production, you should add the Google Root CA.
  client.setInsecure();
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
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
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
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
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
  Serial.println("Starting recording...");
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
  
  Serial.println("Finished recording.");
  return true;
}

/**
 * @brief Plays audio from the buffer to the I2S speaker.
 * This is a simple blocking implementation.
 */
bool playAudio(uint8_t* buffer, size_t bufferSize) {
  Serial.println("Starting playback...");
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

  Serial.println("Finished playback.");
  return true;
}

// ===================================
//  GEMINI API FUNCTIONS
// ===================================

/**
 * @brief Sends audio to Gemini, gets text response.
 */
String callGeminiForAnswer(uint8_t* audioBuffer, size_t audioSize) {
  // 1. Encode audio to Base64
  char* base64Data = new char[base64_enc_len(audioSize)];
  if (!base64Data) {
    Serial.println("Failed to allocate memory for base64 encoding.");
    return "";
  }
  Base64.encode(base64Data, (char*)audioBuffer, audioSize);
  String base64String = String(base64Data);
  delete[] base64Data;

  // 2. Create JSON Payload
  // Use ArduinoJson Assistant to calculate size:
  // https://arduinojson.org/v6/assistant/
  StaticJsonDocument<1024> doc; // Base size
  JsonDocument* payload = &doc;
  
  // For large audio, we must use DynamicJsonDocument
  // The payload will be ~1.33x the audio size + overhead
  size_t jsonCapacity = JSON_OBJECT_SIZE(1) + JSON_OBJECT_SIZE(1) + JSON_ARRAY_SIZE(1) + JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(2) + base64String.length() + 100;
  
  if (jsonCapacity > 1024) {
      Serial.println("Using DynamicJsonDocument for large payload.");
      payload = new DynamicJsonDocument(jsonCapacity);
  }
  
  JsonObject contents_0 = payload->createNestedObject("contents");
  JsonArray parts = contents_0.createNestedArray("parts");
  
  JsonObject parts_0 = parts.createNestedObject();
  parts_0["text"] = "Transcribe this audio and provide a concise answer.";

  JsonObject parts_1 = parts.createNestedObject();
  JsonObject inlineData = parts_1.createNestedObject("inlineData");
  inlineData["mimeType"] = "audio/l16;rate=16000"; // LINEAR16 PCM
  inlineData["data"] = base64String;

  String jsonPayload;
  serializeJson(*payload, jsonPayload);

  // Clear memory if dynamic
  if (jsonCapacity > 1024) {
      delete payload;
  }
  base64String = ""; // Free memory

  // 3. Make HTTPS POST request
  String responseText = "";
  http.begin(client, GEMINI_STT_LLM_URL);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(jsonPayload);
  
  if (httpCode > 0) {
    String response = http.getString();
    Serial.printf("STT/LLM API Response Code: %d\n", httpCode);
    
    // 4. Parse Response
    DynamicJsonDocument responseDoc(2048);
    deserializeJson(responseDoc, response);
    
    // Check for errors
    if (responseDoc.containsKey("error")) {
        Serial.print("API Error: ");
        serializeJson(responseDoc["error"]["message"], Serial);
        Serial.println();
    }
    // Get text
    else if (responseDoc.containsKey("candidates") && 
             responseDoc["candidates"][0].containsKey("content") &&
             responseDoc["candidates"][0]["content"].containsKey("parts") &&
             responseDoc["candidates"][0]["content"]["parts"][0].containsKey("text")) {
               
      responseText = responseDoc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
    } else {
      Serial.println("Could not parse Gemini answer from JSON.");
      Serial.println(response);
    }
    
  } else {
    Serial.printf("STT/LLM API request failed, error: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
  return responseText;
}

/**
 * @brief Sends text to Gemini TTS, gets audio response.
 * Stores audio in global `playbackBuffer`.
 */
bool callGeminiTTS(String text) {
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

  // 2. Make HTTPS POST request
  http.begin(client, GEMINI_TTS_URL);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(jsonPayload);
  bool success = false;

  if (httpCode > 0) {
    String response = http.getString();
    Serial.printf("TTS API Response Code: %d\n", httpCode);

    // 3. Parse Response
    // TTS response can be large.
    DynamicJsonDocument responseDoc(8192); // Start with 8k, may need more
    DeserializationError error = deserializeJson(responseDoc, response);

    if (error) {
      Serial.print(F("deserializeJson() failed: "));
      Serial.println(error.f_str());
      return false;
    }

    if (responseDoc.containsKey("candidates") &&
        responseDoc["candidates"][0]["content"]["parts"][0]["inlineData"]["data"]) {
          
      const char* base64Audio = responseDoc["candidates"][0]["content"]["parts"][0]["inlineData"]["data"];
      
      // 4. Decode Base64 Audio
      playbackBufferSize = base64_dec_len(base64Audio, strlen(base64Audio));
      playbackBuffer = (uint8_t*)malloc(playbackBufferSize);
      
      if (!playbackBuffer) {
        Serial.println("Failed to allocate memory for playback buffer!");
      } else {
        Base64.decode((char*)playbackBuffer, base64Audio, strlen(base64Audio));
        Serial.printf("Decoded %d bytes of audio.\n", playbackBufferSize);
        success = true;
      }
      
    } else {
      Serial.println("Could not parse TTS audio from JSON.");
      Serial.println(response);
    }
    
  } else {
    Serial.printf("TTS API request failed, error: %s\n", http.errorToString(httpCode).c_str());
  }

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
    if (currentState == STATE_IDLE) {
      buttonPressed = true;
    }
  }
  last_interrupt_time = interrupt_time;
}
