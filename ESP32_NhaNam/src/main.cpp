#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ======================================================
// WIFI CONFIG
// ======================================================
const char* WIFI_SSID = "Fuvitech";
const char* WIFI_PASS = "fuvitech.vn";

// ======================================================
// MQTT CONFIG
// ======================================================
const char* MQTT_HOST = "mqtt.fuvitech.vn";
const uint16_t MQTT_PORT = 1883;

const char* MQTT_USER = "";
const char* MQTT_PASS = "";

// ======================================================
// GATEWAY CONFIG
// ======================================================
const char* GW_ID = "GW01";
const char* FW_VERSION = "ESP32_GATEWAY_FUVIAIR_V2";

// ESP32 RX nhận từ STM32 TX
// ESP32 TX gửi đến STM32 RX
#define STM32_RX_PIN 16
#define STM32_TX_PIN 17
#define STM32_BAUD   115200

// ======================================================
// TOPICS
// ======================================================
char TOPIC_DEVICE_REQ[128];
char TOPIC_DEVICE_RES[128];
char TOPIC_GATEWAY_ACK[128];
char TOPIC_GATEWAY_STATUS[128];

char TOPIC_STM32_CMD_WILDCARD[128];
char TOPIC_STM32_ACK[128];
char TOPIC_STM32_RELAY_STATE[128];
char TOPIC_STM32_STATUS[128];
char TOPIC_STM32_CONFIG_STATE[128];
char TOPIC_STM32_FULL_STATE[128];
char TOPIC_STM32_SYNC_REQUEST[128];
char TOPIC_STM32_SYNC_RESPONSE[128];
char TOPIC_STM32_FAN_STATE[128];

char TOPIC_SERVER_ACK[128];

// ======================================================
// OBJECTS
// ======================================================
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ======================================================
// DEVICE LIST
// ======================================================
#define MAX_DEVICES 30

struct AirDevice {
  String id;
  String name;
  String topicNew;
  String topicOld;
};

AirDevice devices[MAX_DEVICES];
uint8_t deviceCount = 0;

// Cache latest cfgVersion from STM32 FULL_STATE
uint32_t cachedCfgVersion = 0;

// ======================================================
// PENDING ACK
// ======================================================
#define MAX_PENDING 25
#define ACK_TIMEOUT_MS 5000

enum PendingType {
  PENDING_NONE = 0,
  PENDING_STM32_ACK,
  PENDING_SERVER_ACK
};

struct PendingAck {
  bool used;
  String msgId;
  PendingType type;
  uint32_t startMs;
};

PendingAck pendings[MAX_PENDING];

// ======================================================
// UART BUFFER
// ======================================================
String uartLine;

// ======================================================
// UTILS
// ======================================================
String makeMsgId(const char* prefix) {
  static uint32_t counter = 0;
  counter++;
  return String(prefix) + "-" + String(millis()) + "-" + String(counter);
}

void buildTopics() {
  snprintf(TOPIC_DEVICE_REQ, sizeof(TOPIC_DEVICE_REQ),
           "maydokhongkhi/%s/gateway/devices/request", GW_ID);

  snprintf(TOPIC_DEVICE_RES, sizeof(TOPIC_DEVICE_RES),
           "maydokhongkhi/%s/gateway/devices/response", GW_ID);

  snprintf(TOPIC_GATEWAY_ACK, sizeof(TOPIC_GATEWAY_ACK),
           "maydokhongkhi/%s/gateway/ack", GW_ID);

  snprintf(TOPIC_GATEWAY_STATUS, sizeof(TOPIC_GATEWAY_STATUS),
           "maydokhongkhi/%s/gateway/status", GW_ID);

  snprintf(TOPIC_STM32_CMD_WILDCARD, sizeof(TOPIC_STM32_CMD_WILDCARD),
           "maydokhongkhi/%s/stm32/cmd/+", GW_ID);

  snprintf(TOPIC_STM32_ACK, sizeof(TOPIC_STM32_ACK),
           "maydokhongkhi/%s/stm32/ack", GW_ID);

  snprintf(TOPIC_STM32_RELAY_STATE, sizeof(TOPIC_STM32_RELAY_STATE),
           "maydokhongkhi/%s/stm32/relay/state", GW_ID);

  snprintf(TOPIC_STM32_STATUS, sizeof(TOPIC_STM32_STATUS),
           "maydokhongkhi/%s/stm32/status", GW_ID);

  snprintf(TOPIC_STM32_CONFIG_STATE, sizeof(TOPIC_STM32_CONFIG_STATE),
           "maydokhongkhi/%s/stm32/config/state", GW_ID);

  snprintf(TOPIC_STM32_FULL_STATE, sizeof(TOPIC_STM32_FULL_STATE),
           "maydokhongkhi/%s/stm32/full_state", GW_ID);

  snprintf(TOPIC_STM32_SYNC_REQUEST, sizeof(TOPIC_STM32_SYNC_REQUEST),
           "maydokhongkhi/%s/stm32/sync/request", GW_ID);

  snprintf(TOPIC_STM32_SYNC_RESPONSE, sizeof(TOPIC_STM32_SYNC_RESPONSE),
           "maydokhongkhi/%s/stm32/sync/response", GW_ID);

  snprintf(TOPIC_STM32_FAN_STATE, sizeof(TOPIC_STM32_FAN_STATE),
           "maydokhongkhi/%s/stm32/fan/state", GW_ID);

  snprintf(TOPIC_SERVER_ACK, sizeof(TOPIC_SERVER_ACK),
           "maydokhongkhi/%s/server/ack", GW_ID);
}

bool publishText(const char* topic, const String& payload, bool retained = false) {
  Serial.println();
  Serial.println("[MQTT PUB]");
  Serial.print("Topic: ");
  Serial.println(topic);
  Serial.print("Payload: ");
  Serial.println(payload);
  return mqtt.publish(topic, payload.c_str(), retained);
}

bool publishJson(const char* topic, JsonDocument& doc, bool retained = false) {
  String out;
  serializeJson(doc, out);
  return publishText(topic, out, retained);
}

void sendLineToSTM32(const String& line) {
  Serial.println();
  Serial.println("[UART -> STM32]");
  Serial.println(line);

  Serial2.print(line);
  Serial2.print('\n');
}

void sendJsonToSTM32(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  sendLineToSTM32(out);
}

// ======================================================
// PENDING ACK
// ======================================================
void clearPendings() {
  for (int i = 0; i < MAX_PENDING; i++) {
    pendings[i].used = false;
    pendings[i].msgId = "";
    pendings[i].type = PENDING_NONE;
    pendings[i].startMs = 0;
  }
}

bool addPending(const String& msgId, PendingType type) {
  if (msgId.length() == 0) return false;

  for (int i = 0; i < MAX_PENDING; i++) {
    if (!pendings[i].used) {
      pendings[i].used = true;
      pendings[i].msgId = msgId;
      pendings[i].type = type;
      pendings[i].startMs = millis();

      Serial.print("[PENDING ADD] ");
      Serial.println(msgId);
      return true;
    }
  }

  Serial.println("[PENDING FULL]");
  return false;
}

bool removePending(const String& msgId) {
  for (int i = 0; i < MAX_PENDING; i++) {
    if (pendings[i].used && pendings[i].msgId == msgId) {
      pendings[i].used = false;

      Serial.print("[PENDING REMOVE] ");
      Serial.println(msgId);
      return true;
    }
  }
  return false;
}

void publishTimeoutAckToServer(const String& ackFor, const char* errorCode, const char* message) {
  DynamicJsonDocument doc(512);

  doc["type"] = "ACK";
  doc["ack_for"] = ackFor;
  doc["gateway_id"] = GW_ID;
  doc["from"] = "ESP32";
  doc["status"] = "TIMEOUT";
  doc["error_code"] = errorCode;
  doc["message"] = message;
  doc["timestamp_ms"] = millis();

  publishJson(TOPIC_STM32_ACK, doc, false);
}

void sendTimeoutAckToSTM32(const String& ackFor, const char* errorCode, const char* message) {
  DynamicJsonDocument doc(512);

  doc["type"] = "ACK";
  doc["ack_for"] = ackFor;
  doc["gateway_id"] = GW_ID;
  doc["from"] = "ESP32";
  doc["status"] = "TIMEOUT";
  doc["error_code"] = errorCode;
  doc["message"] = message;
  doc["timestamp_ms"] = millis();

  sendJsonToSTM32(doc);
}

void checkPendingTimeouts() {
  uint32_t now = millis();

  for (int i = 0; i < MAX_PENDING; i++) {
    if (!pendings[i].used) continue;

    if (now - pendings[i].startMs >= ACK_TIMEOUT_MS) {
      String msgId = pendings[i].msgId;
      PendingType type = pendings[i].type;

      pendings[i].used = false;

      Serial.print("[ACK TIMEOUT] ");
      Serial.println(msgId);

      if (type == PENDING_STM32_ACK) {
        publishTimeoutAckToServer(msgId, "STM32_NO_ACK", "STM32 did not respond");
      } else if (type == PENDING_SERVER_ACK) {
        sendTimeoutAckToSTM32(msgId, "SERVER_NO_ACK", "Server did not respond");
      }
    }
  }
}

// ======================================================
// STATUS / REQUEST
// ======================================================
void publishGatewayStatus(const char* state) {
  DynamicJsonDocument doc(768);

  doc["type"] = "GATEWAY_STATUS";
  doc["gateway_id"] = GW_ID;
  doc["state"] = state;
  doc["fw"] = FW_VERSION;
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["device_count"] = deviceCount;
  doc["uptime_ms"] = millis();

  publishJson(TOPIC_GATEWAY_STATUS, doc, true);
}

void publishGatewayAck(const String& ackFor, const char* status, const char* message) {
  DynamicJsonDocument doc(512);

  doc["type"] = "ACK";
  doc["ack_for"] = ackFor;
  doc["gateway_id"] = GW_ID;
  doc["from"] = "ESP32";
  doc["status"] = status;
  doc["message"] = message;
  doc["timestamp_ms"] = millis();

  publishJson(TOPIC_GATEWAY_ACK, doc, false);
}

void requestDeviceList() {
  DynamicJsonDocument doc(512);

  String msgId = makeMsgId("ESP_DEVREQ");

  doc["msg_id"] = msgId;
  doc["type"] = "DEVICE_LIST_REQUEST";
  doc["cmd"] = "GET_DEVICE_LIST";
  doc["ack_req"] = true;
  doc["gateway_id"] = GW_ID;
  doc["timestamp_ms"] = millis();

  publishJson(TOPIC_DEVICE_REQ, doc, false);
}

void requestConfigSyncFromServer() {
  DynamicJsonDocument doc(512);

  String msgId = makeMsgId("ESP_SYNCREQ");

  doc["msg_id"] = msgId;
  doc["type"] = "CONFIG_SYNC_REQUEST";
  doc["cmd"] = "GET_CONFIG";
  doc["ack_req"] = true;
  doc["gateway_id"] = GW_ID;
  doc["from"] = "ESP32";
  doc["localCfgVersion"] = cachedCfgVersion;
  doc["timestamp_ms"] = millis();

  publishJson(TOPIC_STM32_SYNC_REQUEST, doc, false);
}

// ======================================================
// DEVICE SUBSCRIBE
// ======================================================
bool isDeviceTopic(const String& topic) {
  for (uint8_t i = 0; i < deviceCount; i++) {
    if (devices[i].topicNew == topic || devices[i].topicOld == topic) return true;
  }
  return false;
}

void unsubscribeOldDevices() {
  for (uint8_t i = 0; i < deviceCount; i++) {
    if (devices[i].topicNew.length()) mqtt.unsubscribe(devices[i].topicNew.c_str());
    if (devices[i].topicOld.length()) mqtt.unsubscribe(devices[i].topicOld.c_str());
  }
}

void subscribeDeviceTopics() {
  for (uint8_t i = 0; i < deviceCount; i++) {
    if (devices[i].topicNew.length()) {
      mqtt.subscribe(devices[i].topicNew.c_str());
      Serial.print("[SUB SENSOR NEW] ");
      Serial.println(devices[i].topicNew);
    }

    if (devices[i].topicOld.length()) {
      mqtt.subscribe(devices[i].topicOld.c_str());
      Serial.print("[SUB SENSOR OLD] ");
      Serial.println(devices[i].topicOld);
    }
  }
}

void sendDeviceListToSTM32() {
  DynamicJsonDocument doc(4096);

  String msgId = makeMsgId("ESP_DEVLIST");

  doc["msg_id"] = msgId;
  doc["type"] = "DEVICE_LIST";
  doc["gateway_id"] = GW_ID;
  doc["ack_req"] = true;
  doc["count"] = deviceCount;

  JsonArray arr = doc.createNestedArray("devices");

  for (uint8_t i = 0; i < deviceCount; i++) {
    JsonObject d = arr.createNestedObject();
    d["id"] = devices[i].id;
    d["name"] = devices[i].name;
    d["topic"] = devices[i].topicNew;
  }

  addPending(msgId, PENDING_STM32_ACK);
  sendJsonToSTM32(doc);
}

void handleDeviceListResponse(JsonDocument& doc) {
  String msgId = doc["msg_id"] | "";

  unsubscribeOldDevices();
  deviceCount = 0;

  if (!doc["devices"].is<JsonArray>()) {
    publishGatewayAck(msgId, "ERR", "devices array missing");
    return;
  }

  JsonArray arr = doc["devices"].as<JsonArray>();

  for (JsonObject d : arr) {
    if (deviceCount >= MAX_DEVICES) break;

    String id = d["id_device"] | d["id"] | "";
    String name = d["name"] | id;
    String topic = d["topic"] | "";

    if (id.length() == 0) continue;

    if (topic.length() == 0) {
      topic = "maydokhongkhi/" + id + "/data";
    }

    devices[deviceCount].id = id;
    devices[deviceCount].name = name;
    devices[deviceCount].topicNew = topic;
    devices[deviceCount].topicOld = "maydokhongkhi/" + id;

    deviceCount++;
  }

  subscribeDeviceTopics();

  publishGatewayAck(msgId, "OK", "DEVICE_LIST_RECEIVED_AND_SUBSCRIBED");

  sendDeviceListToSTM32();

  publishGatewayStatus("ONLINE");
}

// ======================================================
// SENSOR MQTT -> STM32
// ======================================================
void forwardSensorToSTM32(const String& topic, JsonDocument& inDoc) {
  DynamicJsonDocument out(2048);

  String idDevice = inDoc["id_device"] | "";

  if (idDevice.length() == 0) {
    int p1 = topic.indexOf('/');
    int p2 = topic.indexOf('/', p1 + 1);

    if (p1 >= 0) {
      if (p2 > p1) idDevice = topic.substring(p1 + 1, p2);
      else idDevice = topic.substring(p1 + 1);
    }
  }

  out["type"] = "SENSOR";
  out["id_device"] = idDevice;
  out["temperature"] = inDoc["temperature"] | 0.0;
  out["humidity"] = inDoc["humidity"] | 0.0;
  out["co2"] = inDoc["co2"] | 0.0;
  out["light"] = inDoc["light"] | 0.0;
  out["device"] = inDoc["device"] | "FUVIAIR";
  out["timestamp"] = inDoc["timestamp"] | "";
  out["src_topic"] = topic;

  sendJsonToSTM32(out);
}

// ======================================================
// MQTT SERVER -> STM32
// ======================================================
void forwardCmdToSTM32(JsonDocument& doc) {
  String msgId = doc["msg_id"] | "";
  bool ackReq = doc["ack_req"] | false;

  if (msgId.length() == 0) {
    msgId = makeMsgId("NR_NOID");
    doc["msg_id"] = msgId;
  }

  doc["gateway_id"] = GW_ID;

  if (ackReq) {
    addPending(msgId, PENDING_STM32_ACK);
  }

  sendJsonToSTM32(doc);
}

void forwardSyncResponseToSTM32(JsonDocument& doc) {
  String msgId = doc["msg_id"] | "";
  if (msgId.length() == 0) {
    msgId = makeMsgId("NR_SYNCRESP");
    doc["msg_id"] = msgId;
  }

  doc["type"] = "CONFIG_SYNC_RESPONSE";
  doc["gateway_id"] = GW_ID;

  sendJsonToSTM32(doc);
}

void forwardServerAckToSTM32(JsonDocument& doc) {
  String ackFor = doc["ack_for"] | "";

  if (ackFor.length()) {
    removePending(ackFor);
  }

  sendJsonToSTM32(doc);
}

// ======================================================
// STM32 -> MQTT
// ======================================================
void handleSTM32Ack(JsonDocument& doc) {
  String ackFor = doc["ack_for"] | "";

  if (ackFor.length()) {
    removePending(ackFor);
  }

  doc["gateway_id"] = GW_ID;
  doc["from"] = "STM32";
  doc["timestamp_ms"] = millis();

  publishJson(TOPIC_STM32_ACK, doc, false);
}

void handleSTM32RelayState(JsonDocument& doc) {
  String msgId = doc["msg_id"] | "";
  bool ackReq = doc["ack_req"] | false;

  doc["gateway_id"] = GW_ID;
  doc["timestamp_ms"] = millis();

  publishJson(TOPIC_STM32_RELAY_STATE, doc, false);

  if (ackReq && msgId.length()) {
    addPending(msgId, PENDING_SERVER_ACK);
  }
}

void handleSTM32Status(JsonDocument& doc) {
  // Cache cfgVersion from status too
  uint32_t v = doc["cfgVersion"] | 0;
  if (v > cachedCfgVersion) cachedCfgVersion = v;

  doc["gateway_id"] = GW_ID;
  doc["timestamp_ms"] = millis();

  publishJson(TOPIC_STM32_STATUS, doc, false);
}

void handleSTM32ConfigState(JsonDocument& doc) {
  String msgId = doc["msg_id"] | "";
  bool ackReq = doc["ack_req"] | false;

  doc["gateway_id"] = GW_ID;
  doc["timestamp_ms"] = millis();

  publishJson(TOPIC_STM32_CONFIG_STATE, doc, false);

  if (ackReq && msgId.length()) {
    addPending(msgId, PENDING_SERVER_ACK);
  }
}

void handleSTM32FullState(JsonDocument& doc) {
  String msgId = doc["msg_id"] | "";
  bool ackReq = doc["ack_req"] | false;

  // Cache cfgVersion for future sync requests
  uint32_t v = doc["cfgVersion"] | 0;
  if (v > cachedCfgVersion) cachedCfgVersion = v;

  doc["gateway_id"] = GW_ID;
  doc["timestamp_ms"] = millis();

  publishJson(TOPIC_STM32_FULL_STATE, doc, false);

  if (ackReq && msgId.length()) {
    addPending(msgId, PENDING_SERVER_ACK);
  }
}

void handleSTM32SyncRequest(JsonDocument& doc) {
  String msgId = doc["msg_id"] | "";
  bool ackReq = doc["ack_req"] | false;

  doc["gateway_id"] = GW_ID;
  doc["from"] = "STM32";
  doc["timestamp_ms"] = millis();

  publishJson(TOPIC_STM32_SYNC_REQUEST, doc, false);

  if (ackReq && msgId.length()) {
    addPending(msgId, PENDING_SERVER_ACK);
  }
}

void processSTM32Line(const String& line) {
  if (line.length() == 0) return;

  Serial.println();
  Serial.println("[UART <- STM32]");
  Serial.println(line);

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, line);

  if (err) {
    Serial.print("[UART JSON ERR] ");
    Serial.println(err.c_str());
    return;
  }

  String type = doc["type"] | "";

  if (type == "ACK") {
    handleSTM32Ack(doc);
  }
  else if (type == "RELAY_STATE" || type == "EVENT") {
    handleSTM32RelayState(doc);
  }
  else if (type == "STATUS") {
    handleSTM32Status(doc);
  }
  else if (type == "CONFIG_STATE") {
    handleSTM32ConfigState(doc);
  }
  else if (type == "FULL_STATE") {
    handleSTM32FullState(doc);
  }
  else if (type == "CONFIG_SYNC_REQUEST") {
    handleSTM32SyncRequest(doc);
  }
  else if (type == "FAN_STATE") {
    // Forward fan state to MQTT
    doc["gateway_id"] = GW_ID;
    doc["timestamp_ms"] = millis();
    publishJson(TOPIC_STM32_FAN_STATE, doc, false);
  }
  else {
    handleSTM32Status(doc);
  }
}

void readSTM32Uart() {
  while (Serial2.available()) {
    char c = (char)Serial2.read();

    if (c == '\r') continue;

    if (c == '\n') {
      uartLine.trim();

      if (uartLine.length()) {
        processSTM32Line(uartLine);
      }

      uartLine = "";
    } else {
      uartLine += c;

      if (uartLine.length() > 8192) {
        Serial.println("[UART] line too long, clear");
        uartLine = "";
      }
    }
  }
}

// ======================================================
// MQTT CALLBACK
// ======================================================
void mqttCallback(char* topicRaw, byte* payload, unsigned int length) {
  String topic = String(topicRaw);

  String payloadStr;
  payloadStr.reserve(length + 1);

  for (unsigned int i = 0; i < length; i++) {
    payloadStr += (char)payload[i];
  }

  Serial.println();
  Serial.println("[MQTT RX]");
  Serial.print("Topic: ");
  Serial.println(topic);
  Serial.print("Payload: ");
  Serial.println(payloadStr);

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, payloadStr);

  if (err) {
    Serial.print("[MQTT JSON ERR] ");
    Serial.println(err.c_str());
    return;
  }

  if (topic == TOPIC_DEVICE_RES) {
    handleDeviceListResponse(doc);
    return;
  }

  if (topic == TOPIC_SERVER_ACK) {
    forwardServerAckToSTM32(doc);
    return;
  }

  if (topic == TOPIC_STM32_SYNC_RESPONSE) {
    forwardSyncResponseToSTM32(doc);
    return;
  }

  if (topic.startsWith(String("maydokhongkhi/") + GW_ID + "/stm32/cmd/")) {
    forwardCmdToSTM32(doc);
    return;
  }

  if (isDeviceTopic(topic)) {
    forwardSensorToSTM32(topic, doc);
    return;
  }

  Serial.println("[MQTT] Topic ignored");
}

// ======================================================
// MQTT CONNECT
// ======================================================
void mqttSubscribeBaseTopics() {
  mqtt.subscribe(TOPIC_DEVICE_RES);
  Serial.print("[SUB] ");
  Serial.println(TOPIC_DEVICE_RES);

  mqtt.subscribe(TOPIC_STM32_CMD_WILDCARD);
  Serial.print("[SUB] ");
  Serial.println(TOPIC_STM32_CMD_WILDCARD);

  mqtt.subscribe(TOPIC_SERVER_ACK);
  Serial.print("[SUB] ");
  Serial.println(TOPIC_SERVER_ACK);

  mqtt.subscribe(TOPIC_STM32_SYNC_RESPONSE);
  Serial.print("[SUB] ");
  Serial.println(TOPIC_STM32_SYNC_RESPONSE);

  subscribeDeviceTopics();
}

void mqttReconnect() {
  if (mqtt.connected()) return;

  while (!mqtt.connected()) {
    Serial.print("[MQTT] Connecting... ");

    String clientId = String("ESP32_GATEWAY_") + GW_ID + "_" + String((uint32_t)ESP.getEfuseMac(), HEX);

    DynamicJsonDocument willDoc(512);
    willDoc["type"] = "GATEWAY_STATUS";
    willDoc["gateway_id"] = GW_ID;
    willDoc["state"] = "OFFLINE";
    willDoc["fw"] = FW_VERSION;

    String willPayload;
    serializeJson(willDoc, willPayload);

    bool ok;

    if (strlen(MQTT_USER) > 0) {
      ok = mqtt.connect(
        clientId.c_str(),
        MQTT_USER,
        MQTT_PASS,
        TOPIC_GATEWAY_STATUS,
        0,
        true,
        willPayload.c_str()
      );
    } else {
      ok = mqtt.connect(
        clientId.c_str(),
        TOPIC_GATEWAY_STATUS,
        0,
        true,
        willPayload.c_str()
      );
    }

    if (ok) {
      Serial.println("OK");

      publishGatewayStatus("ONLINE");

      mqttSubscribeBaseTopics();

      requestDeviceList();

      requestConfigSyncFromServer();

    } else {
      Serial.print("FAILED rc=");
      Serial.println(mqtt.state());
      delay(2000);
    }
  }
}

// ======================================================
// WIFI
// ======================================================
void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("[WIFI] Connecting");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("[WIFI] OK IP=");
  Serial.println(WiFi.localIP());
}

// ======================================================
// HEARTBEAT
// ======================================================
void heartbeatTask() {
  static uint32_t last = 0;

  if (millis() - last >= 10000) {
    last = millis();

    if (mqtt.connected()) {
      publishGatewayStatus("ONLINE");
    }
  }
}

// ======================================================
// SETUP / LOOP
// ======================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("======================================");
  Serial.println("ESP32 GATEWAY FUVIAIR V2 START");
  Serial.println("======================================");

  buildTopics();
  clearPendings();

  Serial2.begin(STM32_BAUD, SERIAL_8N1, STM32_RX_PIN, STM32_TX_PIN);

  wifiConnect();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(8192);
  mqtt.setKeepAlive(30);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnect();
  }

  mqttReconnect();
  mqtt.loop();

  readSTM32Uart();

  checkPendingTimeouts();
  heartbeatTask();
}