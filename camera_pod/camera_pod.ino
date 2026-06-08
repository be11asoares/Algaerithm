#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include "time.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "mbedtls/base64.h"

#include "board_config.h"

// WiFi
const char *ssid     = "YOUR_WIFI_NAME";
const char *password = "YOUR_WIFI_PASSWORD";

// Adafruit IO
const char *aioUsername   = "YOUR_AIO_USERNAME";
const char *aioKey        = "YOUR_AIO_KEY";
const char *aioFeedLatest = "presentation.image-latest";
const char *aioFeedTime   = "presentation.last-photo-time";

// UK time
const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec      = 0;
const int   daylightOffset_sec = 3600;

// Resolution for upload
#define AIO_FRAME_SIZE  FRAMESIZE_HVGA

// Non-blocking 10-second timer
unsigned long lastPhotoMillis = 0;
const unsigned long PHOTO_INTERVAL = 10000;  // 10 seconds

String serialBuffer = "";

void startCameraServer();
void setupLedFlash();

// -----------------------------------------------------------------------
const char* ordinalSuffix(int day) {
  if (day >= 11 && day <= 13) return "th";
  switch (day % 10) {
    case 1:  return "st";
    case 2:  return "nd";
    case 3:  return "rd";
    default: return "th";
  }
}

void uploadTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("uploadTimestamp: could not get time");
    return;
  }

  char monthYear[20];
  strftime(monthYear, sizeof(monthYear), "%B, %Y", &timeinfo);
  char hourMin[10];
  strftime(hourMin, sizeof(hourMin), "%H:%M", &timeinfo);

  char ts[50];
  snprintf(ts, sizeof(ts), "%d%s %s @ %s",
    timeinfo.tm_mday,
    ordinalSuffix(timeinfo.tm_mday),
    monthYear,
    hourMin);

  String url = "https://io.adafruit.com/api/v2/";
  url += aioUsername;
  url += "/feeds/";
  url += aioFeedTime;
  url += "/data";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-AIO-Key", aioKey);

  String payload = "{\"value\":\"";
  payload += ts;
  payload += "\"}";

  int httpCode = http.POST(payload);
  Serial.print("Timestamp upload response: ");
  Serial.println(httpCode);
  http.end();
}

// -----------------------------------------------------------------------
// Builds the JSON body directly into one malloc'd buffer to avoid
// holding the image in memory multiple times over.
bool uploadPhotoToAdafruitIO(camera_fb_t *fb, const char *feedName) {
  Serial.println("Preparing image upload to Adafruit IO...");

  // Work out base64 length
  size_t b64Len = 0;
  mbedtls_base64_encode(NULL, 0, &b64Len, fb->buf, fb->len);

  // Buffer = {"value":"<base64>"}  +  null terminator
  const char *prefix = "{\"value\":\"";
  const char *suffix = "\"}";
  size_t prefixLen = strlen(prefix);
  size_t suffixLen = strlen(suffix);
  size_t totalLen  = prefixLen + (b64Len - 1) + suffixLen + 1; // b64Len includes its own null

  char *body = (char *)malloc(totalLen);
  if (!body) {
    Serial.println("Body malloc failed");
    return false;
  }

  // Assemble: prefix + base64 + suffix
  memcpy(body, prefix, prefixLen);
  size_t written = 0;
  if (mbedtls_base64_encode((unsigned char *)(body + prefixLen),
                            b64Len, &written, fb->buf, fb->len) != 0) {
    Serial.println("Base64 encode failed");
    free(body);
    return false;
  }
  // written includes the null terminator position; overwrite from there
  memcpy(body + prefixLen + written, suffix, suffixLen);
  body[prefixLen + written + suffixLen] = '\0';

  size_t bodyLen = prefixLen + written + suffixLen;

  String url = "https://io.adafruit.com/api/v2/";
  url += aioUsername;
  url += "/feeds/";
  url += feedName;
  url += "/data";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-AIO-Key", aioKey);

  Serial.print("Upload size: ");
  Serial.println(bodyLen);

  int httpCode = http.POST((uint8_t *)body, bodyLen);
  Serial.print("Adafruit IO response code: ");
  Serial.println(httpCode);
  http.end();

  free(body);
  return httpCode > 0 && httpCode < 300;
}

void captureAndUpload() {
  Serial.printf("Free heap: %u | Largest block: %u\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  Serial.println("Capturing photo for Adafruit IO...");
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    return;
  }

  uploadPhotoToAdafruitIO(fb, aioFeedLatest);
  esp_camera_fb_return(fb);   // return frame ASAP, before timestamp upload

  uploadTimestamp();
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size   = AIO_FRAME_SIZE;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count     = 1;

  if (psramFound()) {
    config.fb_count  = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    Serial.println("WARNING: PSRAM not found.");
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }

#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  WiFi.begin(ssid, password);
  WiFi.setSleep(false);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
  } else {
    Serial.println("Time obtained");
  }

  startCameraServer();
  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");

  captureAndUpload();
  lastPhotoMillis = millis();

  Serial.println("Ready! Streaming every 10 seconds. Type 'snap' for a manual photo.");
}

void loop() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      serialBuffer.trim();
      if (serialBuffer.length() > 0) {
        if (serialBuffer.equalsIgnoreCase("snap")) {
          Serial.println("Manual snap triggered!");
          captureAndUpload();
        } else {
          Serial.print("Unknown command: ");
          Serial.println(serialBuffer);
        }
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
    }
  }

  if (millis() - lastPhotoMillis >= PHOTO_INTERVAL) {
    captureAndUpload();
    lastPhotoMillis = millis();
  }
}