#include <WiFi.h>
#include <WiFiClient.h>
#include "esp_camera.h"
#include <ArduinoJson.h>

/*
  Скетч для ESP32-CAM:
  - Делает фото при нажатии кнопки (GPIO13, вход с подтяжкой INPUT_PULLUP, нажата = LOW).
  - Во время съёмки включает встроенный светодиод-вспышку.
  - Отправляет фото POST-запросом на сервер (формат совместим с CompreFace).
  - Обрабатывает JSON-ответ: выводит его в Serial Monitor.
  - Если лицо распознано (known=true в ответе), устанавливает MATCH_PIN (GPIO12) в HIGH на 1 секунду.
*/

// ===== Параметры Wi-Fi и сервера =====
const char* WIFI_SSID = "*****";
const char* WIFI_PASS = "*****";

// Адрес и параметры HTTP-сервера (CompreFace gateway)
const char* GATEWAY_HOST = "*****";
const uint16_t GATEWAY_PORT = 8080;
const char* GATEWAY_PATH = "/check";
// Ключ устройства (должен совпадать с DEVICE_SHARED_KEY на сервере)
const char* DEVICE_KEY = "my_esp32_secret";

// Интервал проверки кнопки и отправки снимка (мс)
const uint32_t SEND_INTERVAL_MS = 5000;

// ===== Настройки камеры =====
framesize_t FRAME_SIZE = FRAMESIZE_QVGA;  // размер кадра (QVGA для стабильности)
int JPEG_QUALITY = 15;                   // качество JPEG (0-63, чем меньше тем лучше качество)
int FB_COUNT = 1;                        // число буферов кадров

// ===== Пины для индикации и управления =====
const int MATCH_PIN = 12;    // выход (HIGH, если лицо известно)
const uint32_t MATCH_PULSE_MS = 1000;  // длительность импульса на MATCH_PIN (мс)
const int BUTTON_PIN = 13;   // кнопка (GPIO13, подключена к GND, поэтому используем INPUT_PULLUP)
#define FLASH_LED_GPIO 4     // вспышка (встроенный LED на AI-Thinker ESP32-CAM обычно подключён к GPIO4)

// ===== Пины камеры (для модуля AI-Thinker ESP32-CAM) =====
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

unsigned long lastSend = 0;  // время последнего снимка (для интервала)

// Функция мигания вспышкой (FLASH_LED_GPIO) указанное число раз
static void flashBlink(int times, int onMs = 80, int offMs = 80) {
  pinMode(FLASH_LED_GPIO, OUTPUT);
  for (int i = 0; i < times; i++) {
    digitalWrite(FLASH_LED_GPIO, HIGH);
    delay(onMs);
    digitalWrite(FLASH_LED_GPIO, LOW);
    delay(offMs);
  }
}

// Подключение к Wi-Fi с переподключением при неудаче
static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
    if (++tries > 60) {
      Serial.println("\nНе удалось подключиться к WiFi, перезагрузка...");
      ESP.restart();
    }
  }
  Serial.println("\nWiFi connected!");
  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());
}

// Инициализация камеры
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
  config.xclk_freq_hz = 20000020;  // частота тактирования камеры (20 MHz)
  config.pixel_format = PIXFORMAT_JPEG;
  if (psramFound()) {
    config.frame_size = FRAME_SIZE;
    config.jpeg_quality = JPEG_QUALITY;
    config.fb_count = FB_COUNT;
  } else {
    // Если PSRAM нет, уменьшаем настройки для экономии памяти
    config.frame_size = FRAMESIZE_QQVGA;
    config.jpeg_quality = 20;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  // Инициализируем камеру
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Ошибка инициализации камеры: 0x%x\n", err);
    return false;
  }
  Serial.println("Camera init OK");
  return true;
}

// Разбор первой строки HTTP-ответа, чтобы получить код состояния
static int parseHttpStatus(const String& resp) {
  int sp1 = resp.indexOf(' ');
  if (sp1 < 0) return -1;
  int sp2 = resp.indexOf(' ', sp1 + 1);
  if (sp2 < 0) return -1;
  return resp.substring(sp1 + 1, sp2).toInt();
}

// Делает фото, отправляет POST-запрос на сервер и получает JSON-ответ
static bool sendPhotoAndReadJson(String &jsonBody, int &httpStatus) {
  jsonBody = "";
  httpStatus = -1;

  // Включаем вспышку на время фотографирования
  digitalWrite(FLASH_LED_GPIO, HIGH);
  camera_fb_t * fb = esp_camera_fb_get();  // делаем фото с камеры
  digitalWrite(FLASH_LED_GPIO, LOW);
  if (!fb) {
    Serial.println("Camera capture failed");
    return false;
  }

  // Формируем тело POST-запроса (multipart/form-data) с JPEG-фото
  String boundary = "----ESP32CamBoundary7MA4YWxkTrZu0gW";
  String head =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"frame.jpg\"\r\n"
    "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";
  uint32_t contentLength = head.length() + fb->len + tail.length();

  // Подключаемся к серверу
  WiFiClient client;
  if (!client.connect(GATEWAY_HOST, GATEWAY_PORT)) {
    Serial.println("Ошибка подключения к серверу");
    esp_camera_fb_return(fb);
    return false;
  }

  // Отправляем HTTP-заголовки запроса
  client.print(String("POST ") + GATEWAY_PATH + " HTTP/1.1\r\n");
  client.print(String("Host: ") + GATEWAY_HOST + "\r\n");
  client.print("Connection: close\r\n");
  client.print(String("x-device-key: ") + DEVICE_KEY + "\r\n");
  client.print(String("Content-Type: multipart/form-data; boundary=") + boundary + "\r\n");
  client.print(String("Content-Length: ") + contentLength + "\r\n\r\n");

  // Отправляем тело запроса (фото)
  client.print(head);
  client.write(fb->buf, fb->len);
  client.print(tail);

  esp_camera_fb_return(fb);  // освобождаем буфер камеры

  // Читаем ответ от сервера (ожидаем JSON)
  unsigned long timeout = millis();
  String response = "";
  while (client.connected() && millis() - timeout < 12000) {
    while (client.available()) {
      char c = client.read();
      response += c;
      timeout = millis();
    }
    delay(5);
  }
  client.stop();

  // Выделяем код состояния и тело ответа (JSON)
  httpStatus = parseHttpStatus(response);
  int bodyIndex = response.indexOf("\r\n\r\n");
  if (bodyIndex < 0) {
    Serial.println("Bad HTTP response (no JSON body)");
    return false;
  }
  jsonBody = response.substring(bodyIndex + 4);
  jsonBody.trim();
  return true;
}

// Разбирает JSON-ответ от сервера (CompreFace) на поля known, subject, similarity, reason
static bool parseGatewayJson(const String &body, bool &known, String &subject, float &similarity, String &reason) {
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
  // Настраиваем пины
  pinMode(FLASH_LED_GPIO, OUTPUT);
  digitalWrite(FLASH_LED_GPIO, LOW);     // выключаем вспышку (пока не нужна)
  pinMode(MATCH_PIN, OUTPUT);
  digitalWrite(MATCH_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);     // кнопка с подтяжкой, нажата = LOW

  connectWiFi();

  // Инициализируем камеру; при ошибке перезагружаем модуль
  if (!initCamera()) {
    Serial.println("Camera init failed -> перезагрузка через 3 сек...");
    delay(3000);
    ESP.restart();
  }

  flashBlink(2);  // двойная вспышка для индикации успешного запуска
}

void loop() {
  // Проверка Wi-Fi подключения; повторное подключение при разрыве
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi потерял соединение, повторное подключение...");
    connectWiFi();
  }

  // Чтение состояния кнопки (с программным отслеживанием фронта)
  static int lastButtonState = HIGH;
  int buttonState = digitalRead(BUTTON_PIN);
  unsigned long now = millis();

  // Каждые 5 сек проверяем: если кнопка только что нажата (переход HIGH->LOW) и интервал выдержан
  if (lastButtonState == HIGH && buttonState == LOW && (now - lastSend >= SEND_INTERVAL_MS)) {
    lastButtonState = buttonState;  // фиксируем состояние "кнопка нажата"
    lastSend = now;

    Serial.println("\n=== Capturing & sending photo... ===");

    String body;
    int status = -1;

    // Делаем фото и отправляем его на сервер, получая ответ
    if (!sendPhotoAndReadJson(body, status)) {
      Serial.println("Send/read failed (ошибка отправки или получения)");
      digitalWrite(MATCH_PIN, LOW);
      flashBlink(1, 250, 250);  // одна короткая вспышка - сигнал ошибки
      // Выход из функции loop, ждать следующей итерации
      return;
    }

    // Выводим HTTP-статус и JSON-ответ от сервера
    Serial.printf("HTTP status: %d\n", status);
    Serial.print("JSON raw: ");
    Serial.println(body);

    bool known;
    String subject;
    float similarity;
    String reason;

    // Парсим JSON (извлекаем поля known, subject, similarity, reason)
    if (!parseGatewayJson(body, known, subject, similarity, reason)) {
      Serial.println("JSON parse error (ошибка разбора ответа)");
      digitalWrite(MATCH_PIN, LOW);
      flashBlink(2, 200, 200);  // два коротких мигания вспышкой - ошибка парсинга
      return;
    }

    // Выводим распознанные данные в Serial
    Serial.printf("Parsed -> known=%s, subject='%s', similarity=%.5f, reason='%s'\n",
                  known ? "true" : "false",
                  subject.c_str(),
                  similarity,
                  reason.c_str());

    // Действия в зависимости от результата распознавания
    if (known) {
      // Лицо известно (known=true)
      digitalWrite(MATCH_PIN, HIGH);    // подаём сигнал на выход (например, индикатор или реле)
      flashBlink(3, 80, 80);           // три быстрых мигания вспышкой (индикация успешного распознавания)
      delay(MATCH_PULSE_MS);
      digitalWrite(MATCH_PIN, LOW);
    } else {
      // Лицо не распознано (known=false)
      digitalWrite(MATCH_PIN, LOW);
      flashBlink(1, 120, 120);         // одно мигание вспышкой (индикация неизвестного лица)
    }

    // Ждём, пока кнопку отпустят, прежде чем разрешить следующий снимок
    while (digitalRead(BUTTON_PIN) == LOW) {
      delay(10);
    }
  }

  // Обновляем сохранённое состояние кнопки для отслеживания нажатия
  lastButtonState = buttonState;

  delay(10);  // небольшая задержка для антидребезга и снижения нагрузки
}
