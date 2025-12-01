#include <WiFi.h>
#include <WiFiClient.h>
#include "esp_camera.h"
#include <ArduinoJson.h>

// ================= НАСТРОЙКИ =================
const char* WIFI_SSID = "galaxy s24+";
const char* WIFI_PASS = "renat2006";

// IP ноутбука, где запущен gateway (uvicorn) на 8080
const char* GATEWAY_HOST = "192.168.50.23";
const uint16_t GATEWAY_PORT = 8080;
const char* GATEWAY_PATH = "/check";

// Должно совпадать с DEVICE_SHARED_KEY в app.py
const char* DEVICE_KEY = "my_esp32_secret";

// Интервал отправки
const uint32_t SEND_INTERVAL_MS = 5000;

// Настройки камеры (стабильный старт — QVGA)
framesize_t FRAME_SIZE = FRAMESIZE_QVGA;
int JPEG_QUALITY = 15;
int FB_COUNT = 1;

// HIGH если known=true
const int MATCH_PIN = 12;
// Сколько держать HIGH (мс)
const uint32_t MATCH_PULSE_MS = 1000;

// Вспышка как индикатор (AI Thinker обычно GPIO4)
#define FLASH_LED_GPIO 4
// =============================================


// ======= AI THINKER ESP32-CAM PIN MAP =======
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
// ============================================

unsigned long lastSend = 0;

static void flashBlink(int times, int onMs=80, int offMs=80) {
  pinMode(FLASH_LED_GPIO, OUTPUT);
  for (int i=0;i<times;i++){
    digitalWrite(FLASH_LED_GPIO, HIGH); delay(onMs);
    digitalWrite(FLASH_LED_GPIO, LOW);  delay(offMs);
  }
}

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("WiFi connecting");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
    tries++;
    if (tries > 60) {
      Serial.println("\nWiFi connect failed, restarting...");
      ESP.restart();
    }
  }
  Serial.println("\nWiFi connected!");
  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());
}

static bool initCamera() {
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
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = FRAME_SIZE;
    config.jpeg_quality = JPEG_QUALITY;
    config.fb_count = FB_COUNT;
  } else {
    config.frame_size = FRAMESIZE_QQVGA;
    config.jpeg_quality = 20;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  Serial.println("Camera init OK");
  return true;
}

static int parseHttpStatus(const String& resp) {
  int sp1 = resp.indexOf(' ');
  if (sp1 < 0) return -1;
  int sp2 = resp.indexOf(' ', sp1 + 1);
  if (sp2 < 0) return -1;
  return resp.substring(sp1 + 1, sp2).toInt();
}

static bool sendPhotoAndReadJson(String &jsonBody, int &httpStatus) {
  jsonBody = "";
  httpStatus = -1;

  camera_fb_t * fb = esp_camera_fb_get();  // <-- фото делается здесь
  if (!fb) {
    Serial.println("Camera capture failed");
    return false;
  }

  String boundary = "----ESP32CamBoundary7MA4YWxkTrZu0gW";
  String head =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"frame.jpg\"\r\n"
    "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  uint32_t contentLength = head.length() + fb->len + tail.length();

  WiFiClient client;
  if (!client.connect(GATEWAY_HOST, GATEWAY_PORT)) {
    Serial.println("Gateway connection failed");
    esp_camera_fb_return(fb);
    return false;
  }

  client.print(String("POST ") + GATEWAY_PATH + " HTTP/1.1\r\n");
  client.print(String("Host: ") + GATEWAY_HOST + "\r\n");
  client.print("Connection: close\r\n");
  client.print(String("x-device-key: ") + DEVICE_KEY + "\r\n");
  client.print(String("Content-Type: multipart/form-data; boundary=") + boundary + "\r\n");
  client.print(String("Content-Length: ") + contentLength + "\r\n\r\n");

  client.print(head);
  client.write(fb->buf, fb->len);
  client.print(tail);

  esp_camera_fb_return(fb);

  unsigned long timeout = millis();
  String response;
  while (client.connected() && millis() - timeout < 12000) {
    while (client.available()) {
      response += (char)client.read();
      timeout = millis();
    }
    delay(5);
  }
  client.stop();

  httpStatus = parseHttpStatus(response);

  int bodyIndex = response.indexOf("\r\n\r\n");
  if (bodyIndex < 0) {
    Serial.println("Bad HTTP response (no header/body split)");
    return false;
  }

  jsonBody = response.substring(bodyIndex + 4);
  jsonBody.trim();
  return true;
}

static bool parseGatewayJson(
  const String &body,
  bool &known,
  String &subject,
  float &similarity,
  String &reason
) {
  known = false;
  subject = "";
  similarity = -1.0f;
  reason = "";

  StaticJsonDocument<1024> doc;
  auto err = deserializeJson(doc, body);
  if (err) return false;

  known = doc["known"] | false;
  const char* subj = doc["subject"] | "";
  subject = String(subj);
  similarity = doc["similarity"] | -1.0;
  const char* rsn = doc["reason"] | "";
  reason = String(rsn);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(FLASH_LED_GPIO, OUTPUT);
  digitalWrite(FLASH_LED_GPIO, LOW);

  pinMode(MATCH_PIN, OUTPUT);
  digitalWrite(MATCH_PIN, LOW);

  connectWiFi();

  if (!initCamera()) {
    Serial.println("Camera init failed -> restart in 3s");
    delay(3000);
    ESP.restart();
  }

  flashBlink(2);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    connectWiFi();
  }

  unsigned long now = millis();
  if (now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;

    Serial.println("\n=== Capturing & sending photo... ===");

    String body;
    int status = -1;

    if (!sendPhotoAndReadJson(body, status)) {
      Serial.println("Send/read failed");
      digitalWrite(MATCH_PIN, LOW);
      flashBlink(1, 250, 250);
      return;
    }

    Serial.printf("HTTP status: %d\n", status);
    Serial.print("JSON raw: ");
    Serial.println(body);

    bool known;
    String subject;
    float similarity;
    String reason;

    if (!parseGatewayJson(body, known, subject, similarity, reason)) {
      Serial.println("JSON parse error");
      digitalWrite(MATCH_PIN, LOW);
      flashBlink(2, 200, 200);
      return;
    }

    Serial.printf("Parsed -> known=%s, subject='%s', similarity=%.5f, reason='%s'\n",
                  known ? "true" : "false",
                  subject.c_str(),
                  similarity,
                  reason.c_str());

    if (known) {
      digitalWrite(MATCH_PIN, HIGH);
      flashBlink(3, 80, 80);
      delay(MATCH_PULSE_MS);
      digitalWrite(MATCH_PIN, LOW);
    } else {
      digitalWrite(MATCH_PIN, LOW);
      flashBlink(1, 120, 120);
    }
  }

  delay(10);
}
