#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// WIFI
const char* WIFI_SSID = "Fuvitech";
const char* WIFI_PASS = "fuvitech.vn";

// MQTT
const char* MQTT_HOST = "mqtt.fuvitech.vn";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER = "";
const char* MQTT_PASS = "";

// ID
const char* CONTROL_ID = "ESP39";
const char* STM_ID = "STM39";

// UART STM32
#define STM32_RX_PIN 16
#define STM32_TX_PIN 17
#define STM32_BAUD   115200

WiFiClient espClient;
PubSubClient mqtt(espClient);

String uartLine;

char TOPIC_UART_RAW[128];
char TOPIC_UART_JSON[128];
char TOPIC_STATUS[128];

void buildTopics() {
  snprintf(TOPIC_UART_RAW, sizeof(TOPIC_UART_RAW),
           "maydokhongkhi/%s/uart/raw", CONTROL_ID);

  snprintf(TOPIC_UART_JSON, sizeof(TOPIC_UART_JSON),
           "maydokhongkhi/%s/uart/test", CONTROL_ID);

  snprintf(TOPIC_STATUS, sizeof(TOPIC_STATUS),
           "maydokhongkhi/%s/uart/status", CONTROL_ID);
}

void mqttPublish(const char* topic, const String& payload, bool retained = false) {
  Serial.println();
  Serial.println("[MQTT PUB]");
  Serial.print("Topic: ");
  Serial.println(topic);
  Serial.print("Payload: ");
  Serial.println(payload);

  if (mqtt.connected()) {
    mqtt.publish(topic, payload.c_str(), retained);
  }
}

void connectWiFi() {
  Serial.print("WiFi connecting");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("WiFi OK, IP: ");
  Serial.println(WiFi.localIP());
}

void reconnectMqtt() {
  while (!mqtt.connected()) {
    String clientId = String("ESP32_UART_TEST_") + CONTROL_ID + "_" + String(random(1000, 9999));

    Serial.print("MQTT connecting... ");

    bool ok;
    if (strlen(MQTT_USER) > 0) {
      ok = mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS);
    } else {
      ok = mqtt.connect(clientId.c_str());
    }

    if (ok) {
      Serial.println("OK");

      StaticJsonDocument<256> doc;
      doc["type"] = "UART_TEST_STATUS";
      doc["control_id"] = CONTROL_ID;
      doc["stm_id"] = STM_ID;
      doc["state"] = "ONLINE";
      doc["ip"] = WiFi.localIP().toString();
      doc["uptime_ms"] = millis();

      String out;
      serializeJson(doc, out);
      mqttPublish(TOPIC_STATUS, out, true);
    } else {
      Serial.print("FAILED, rc=");
      Serial.println(mqtt.state());
      delay(2000);
    }
  }
}

void sendAckToSTM32(const String& ackFor, const char* status, const char* message) {
  StaticJsonDocument<256> doc;

  doc["type"] = "ACK";
  doc["ack_for"] = ackFor;
  doc["from"] = "ESP32";
  doc["control_id"] = CONTROL_ID;
  doc["status"] = status;
  doc["message"] = message;
  doc["timestamp_ms"] = millis();

  String out;
  serializeJson(doc, out);

  Serial2.print(out);
  Serial2.print('\n');

  Serial.print("[ESP32 -> STM32 ACK] ");
  Serial.println(out);
}

void handleUartLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  Serial.println();
  Serial.print("[UART RX FROM STM32] ");
  Serial.println(line);

  // Publish raw trước để debug
  mqttPublish(TOPIC_UART_RAW, line, false);

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, line);

  if (err) {
    StaticJsonDocument<256> errDoc;
    errDoc["type"] = "UART_JSON_ERROR";
    errDoc["control_id"] = CONTROL_ID;
    errDoc["error"] = err.c_str();
    errDoc["raw"] = line;
    errDoc["uptime_ms"] = millis();

    String out;
    serializeJson(errDoc, out);

    mqttPublish(TOPIC_UART_JSON, out, false);
    sendAckToSTM32("UNKNOWN", "ERR", "JSON_INVALID");
    return;
  }

  String msgId = doc["msg_id"] | "";

  // Bổ sung thông tin ESP trước khi post MQTT
  doc["gateway_id"] = CONTROL_ID;
  doc["esp_rx_ms"] = millis();
  doc["mqtt_topic"] = TOPIC_UART_JSON;

  String out;
  serializeJson(doc, out);

  mqttPublish(TOPIC_UART_JSON, out, false);

  if (msgId.length() > 0) {
    sendAckToSTM32(msgId, "OK", "UART_RECEIVED_AND_MQTT_PUBLISHED");
  } else {
    sendAckToSTM32("NO_MSG_ID", "OK", "UART_RECEIVED_AND_MQTT_PUBLISHED");
  }
}

void uartTask() {
  while (Serial2.available()) {
    char c = Serial2.read();

    if (c == '\n') {
      handleUartLine(uartLine);
      uartLine = "";
    } else if (c != '\r') {
      uartLine += c;

      if (uartLine.length() > 1024) {
        Serial.println("[UART BUFFER OVERFLOW] clear buffer");
        uartLine = "";
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial2.begin(STM32_BAUD, SERIAL_8N1, STM32_RX_PIN, STM32_TX_PIN);

  buildTopics();

  Serial.println();
  Serial.println("ESP32 UART TO MQTT TEST START");

  connectWiFi();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  reconnectMqtt();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (!mqtt.connected()) {
    reconnectMqtt();
  }

  mqtt.loop();
  uartTask();
}