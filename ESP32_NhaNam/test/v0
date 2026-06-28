#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ====================================================== //
// ESP32 GATEWAY FUVIAIR - TU DIEU KHIEN DUNG CHUNG ID
// - ESP32 làm cầu MQTT <-> UART cho STM32 trong cùng tủ điều khiển
// - ID chung của tủ điều khiển dùng cho ESP32 cầu và STM32
// - Nhận dữ liệu thiết bị Fuviair và chuyển xuống STM32 để xử lý tự động
// ====================================================== //

// ====================================================== //
// WIFI CONFIG
// ====================================================== //
const char* WIFI_SSID = "Fuvitech";
const char* WIFI_PASS = "fuvitech.vn";

// ====================================================== //
// MQTT CONFIG
// ====================================================== //
const char* MQTT_HOST = "mqtt.fuvitech.vn";
const uint16_t MQTT_PORT = 1883;

const char* MQTT_USER = "";
const char* MQTT_PASS = "";

// ====================================================== //
// GATEWAY CONFIG
// ====================================================== //
const char* CONTROL_ID = "ESP39";          // Mã tủ điều khiển dùng chung cho ESP32 cầu và STM32
const char* GW_ID = CONTROL_ID;             // Giữ tên GW_ID trong code, giá trị chính là CONTROL_ID
const char* FW_VERSION = "ESP32_GATEWAY_FUVIAIR_V10_COMPACT_SPLIT_UART_FIX";
const char* STM_ID = "STM39";

// ====================================================== //
// FLOW 2 /Farm CONFIG
// Node-RED Flow 2 dùng các topic:
//   nong_trai/auth/request  -> ESP32 kiểm tra device_id/device_key
//   nong_trai/auth/response <- ESP32 trả status ok/error
//   nong_trai/sensors       <- ESP32 publish dữ liệu cảm biến
// ====================================================== //
#define ENABLE_FLOW2_COMPAT          1
const char* FLOW2_DEVICE_ID  = CONTROL_ID;  // Tủ điều khiển dùng chung ID với ESP32 cầu
const char* FLOW2_LEGACY_DEVICE_ID = "ESP39001"; // Cho phép mã cũ khi cần chuyển đổi dữ liệu
const char* FLOW2_DEVICE_KEY = "393939";

const char* TOPIC_FLOW2_AUTH_REQUEST  = "nong_trai/auth/request";
const char* TOPIC_FLOW2_AUTH_RESPONSE = "nong_trai/auth/response";
const char* TOPIC_FLOW2_SENSOR_DATA   = "nong_trai/sensors";

// Bật 1 nếu muốn tự bắn sensor giả để test dashboard /Farm
#define ENABLE_FLOW2_DUMMY_SENSOR     0
#define FLOW2_DUMMY_SENSOR_MS         5000

// ESP32 RX nhận từ STM32 TX3 PB10
// ESP32 TX gửi đến STM32 RX3 PB11
#define STM32_RX_PIN 16
#define STM32_TX_PIN 17
#define STM32_BAUD   115200

// Gửi SET_TIME trước SET_TIMER bằng giờ NTP của ESP32
#define ENABLE_NTP_TIME_SYNC       1
#define SEND_TIME_BEFORE_TIMER     1
#define GMT_OFFSET_SEC             (7 * 3600)
#define DAYLIGHT_OFFSET_SEC        0

// Chống dồn frame UART vào STM32
#define STM_TX_QUEUE_SIZE          20
#define STM_TX_GAP_MS              35

// Pending ACK
#define MAX_PENDING                30
#define ACK_TIMEOUT_MS             7000

// Device list
#define MAX_DEVICES                30

// ====================================================== //
// TOPICS
// ====================================================== //
char TOPIC_DEVICE_REQ[128];
char TOPIC_DEVICE_RES[128];
char TOPIC_GATEWAY_ACK[128];
char TOPIC_GATEWAY_STATUS[128];

char TOPIC_STM32_CMD[128];
char TOPIC_STM32_CMD_WILDCARD[128];
char TOPIC_STM32_ACK[128];
char TOPIC_STM32_RELAY_STATE[128];
char TOPIC_STM32_STATUS[128];
char TOPIC_STM32_CONFIG_STATE[128];
char TOPIC_STM32_FULL_STATE[128];
char TOPIC_STM32_SYNC_REQUEST[128];
char TOPIC_STM32_SYNC_RESPONSE[128];
char TOPIC_STM32_FAN_STATE[128];
char TOPIC_STM32_BOOT[128];

char TOPIC_SERVER_ACK[128];

// ====================================================== //
// OBJECTS
// ====================================================== //
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ====================================================== //
// DEVICE LIST
// ====================================================== //
struct AirDevice {
  String id;
  String name;
  String topicNew;
  String topicOld;
};

AirDevice devices[MAX_DEVICES];
uint8_t deviceCount = 0;
uint32_t cachedCfgVersion = 0;

// ====================================================== //
// PENDING ACK
// ====================================================== //
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

// ====================================================== //
// UART RX/TX BUFFER
// ====================================================== //
String uartLine;
String stmTxQueue[STM_TX_QUEUE_SIZE];
uint8_t stmTxHead = 0;
uint8_t stmTxTail = 0;
uint8_t stmTxCount = 0;
uint32_t lastStmTxMs = 0;

// ====================================================== //
// UTILS
// ====================================================== //
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

  // Nhận lệnh cả 2 dạng:
  // maydokhongkhi/GW01/stm32/cmd
  // maydokhongkhi/GW01/stm32/cmd/anything
  snprintf(TOPIC_STM32_CMD, sizeof(TOPIC_STM32_CMD),
           "maydokhongkhi/%s/stm32/cmd", GW_ID);
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
           "maydokhongkhi/%s/stm32/config/sync/request", GW_ID);
  snprintf(TOPIC_STM32_SYNC_RESPONSE, sizeof(TOPIC_STM32_SYNC_RESPONSE),
           "maydokhongkhi/%s/stm32/config/sync/response", GW_ID);
  snprintf(TOPIC_STM32_FAN_STATE, sizeof(TOPIC_STM32_FAN_STATE),
           "maydokhongkhi/%s/stm32/fan/state", GW_ID);
  snprintf(TOPIC_STM32_BOOT, sizeof(TOPIC_STM32_BOOT),
           "maydokhongkhi/%s/stm32/boot", GW_ID);

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

  if (!mqtt.connected()) return false;
  return mqtt.publish(topic, payload.c_str(), retained);
}

bool publishJson(const char* topic, JsonDocument& doc, bool retained = false) {
  String out;
  serializeJson(doc, out);
  return publishText(topic, out, retained);
}

String getStr(JsonDocument& doc, const char* key, const char* def = "") {
  JsonVariant v = doc[key];
  if (!v.isNull()) return v.as<String>();

  JsonVariant data = doc["data"];
  if (data.is<JsonObject>()) {
    v = data[key];
    if (!v.isNull()) return v.as<String>();
  }

  return String(def);
}

String variantToString(JsonVariant v, const char* def = "") {
  if (v.isNull()) return String(def);
  if (v.is<const char*>()) return String(v.as<const char*>());
  if (v.is<bool>()) return v.as<bool>() ? "true" : "false";
  if (v.is<int>()) return String(v.as<int>());
  if (v.is<unsigned int>()) return String(v.as<unsigned int>());
  if (v.is<long>()) return String(v.as<long>());
  if (v.is<unsigned long>()) return String(v.as<unsigned long>());
  if (v.is<float>()) return String(v.as<float>(), 3);
  if (v.is<double>()) return String(v.as<double>(), 3);

  String out;
  serializeJson(v, out);
  out.trim();
  if (out.startsWith("\"") && out.endsWith("\"") && out.length() >= 2) {
    out.remove(0, 1);
    out.remove(out.length() - 1, 1);
  }
  return out.length() ? out : String(def);
}

String getAnyStr(JsonDocument& doc, const char* k1, const char* k2 = nullptr, const char* k3 = nullptr, const char* k4 = nullptr) {
  const char* keys[4] = {k1, k2, k3, k4};

  for (uint8_t i = 0; i < 4; i++) {
    if (!keys[i] || strlen(keys[i]) == 0) continue;
    JsonVariant v = doc[keys[i]];
    if (!v.isNull()) return variantToString(v, "");
  }

  JsonVariant data = doc["data"];
  if (data.is<JsonObject>()) {
    for (uint8_t i = 0; i < 4; i++) {
      if (!keys[i] || strlen(keys[i]) == 0) continue;
      JsonVariant v = data[keys[i]];
      if (!v.isNull()) return variantToString(v, "");
    }
  }

  return String("");
}

int getInt(JsonDocument& doc, const char* key, int def = 0) {
  JsonVariant v = doc[key];
  if (v.is<int>()) return v.as<int>();

  JsonVariant data = doc["data"];
  if (data.is<JsonObject>()) {
    v = data[key];
    if (v.is<int>()) return v.as<int>();
  }

  return def;
}

float getFloatAlias(JsonDocument& doc, const char* k1, const char* k2, const char* k3, float def = 0.0f) {
  if (!doc[k1].isNull()) return doc[k1].as<float>();
  if (k2 && strlen(k2) && !doc[k2].isNull()) return doc[k2].as<float>();
  if (k3 && strlen(k3) && !doc[k3].isNull()) return doc[k3].as<float>();

  JsonVariant data = doc["data"];
  if (data.is<JsonObject>()) {
    if (!data[k1].isNull()) return data[k1].as<float>();
    if (k2 && strlen(k2) && !data[k2].isNull()) return data[k2].as<float>();
    if (k3 && strlen(k3) && !data[k3].isNull()) return data[k3].as<float>();
  }

  return def;
}

bool getBoolAckReq(JsonDocument& doc, bool def = true) {
  JsonVariant v = doc["ack_req"];
  if (v.is<bool>()) return v.as<bool>();
  if (v.is<int>()) return v.as<int>() != 0;
  return def;
}

bool isStm32CmdTopic(const String& topic) {
  String base = String(TOPIC_STM32_CMD);
  return topic == base || topic.startsWith(base + "/");
}

// ====================================================== //
// UART TX QUEUE TO STM32
// ====================================================== //
bool enqueueLineToSTM32(const String& line) {
  if (stmTxCount >= STM_TX_QUEUE_SIZE) {
    Serial.println("[UART QUEUE FULL] Drop frame to STM32");
    Serial.println(line);
    return false;
  }

  stmTxQueue[stmTxTail] = line;
  stmTxTail = (stmTxTail + 1) % STM_TX_QUEUE_SIZE;
  stmTxCount++;
  return true;
}

void sendLineToSTM32(const String& line) {
  Serial.println();
  Serial.println("[UART QUEUE -> STM32]");
  Serial.println(line);
  enqueueLineToSTM32(line);
}

void sendJsonToSTM32(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  sendLineToSTM32(out);
}

void stmTxTask() {
  if (stmTxCount == 0) return;
  if (millis() - lastStmTxMs < STM_TX_GAP_MS) return;

  String line = stmTxQueue[stmTxHead];
  stmTxQueue[stmTxHead] = "";
  stmTxHead = (stmTxHead + 1) % STM_TX_QUEUE_SIZE;
  stmTxCount--;
  lastStmTxMs = millis();

  Serial.println();
  Serial.println("[UART SEND -> STM32]");
  Serial.println(line);

  Serial2.print(line);
  Serial2.print('\n');
}

// ====================================================== //
// PENDING ACK
// ====================================================== //
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

  // Nếu đã có cùng msg_id thì không thêm trùng
  for (int i = 0; i < MAX_PENDING; i++) {
    if (pendings[i].used && pendings[i].msgId == msgId) return true;
  }

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

PendingType getPendingType(const String& msgId) {
  for (int i = 0; i < MAX_PENDING; i++) {
    if (pendings[i].used && pendings[i].msgId == msgId) return pendings[i].type;
  }
  return PENDING_NONE;
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

bool removePendingByType(const String& msgId, PendingType type) {
  for (int i = 0; i < MAX_PENDING; i++) {
    if (pendings[i].used && pendings[i].msgId == msgId && pendings[i].type == type) {
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
  doc["control_id"] = CONTROL_ID;
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
  doc["control_id"] = CONTROL_ID;
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

// ====================================================== //
// STATUS / ACK / REQUEST
// ====================================================== //
void publishGatewayStatus(const char* state) {
  DynamicJsonDocument doc(768);
  doc["type"] = "GATEWAY_STATUS";
  doc["gateway_id"] = GW_ID;
  doc["control_id"] = CONTROL_ID;
  doc["cabinet_id"] = CONTROL_ID;
  doc["device_id"] = CONTROL_ID;
  doc["flow2_device_id"] = CONTROL_ID;
  doc["stm_id"] = STM_ID;
  doc["state"] = state;
  doc["fw"] = FW_VERSION;
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["device_count"] = deviceCount;
  doc["cachedCfgVersion"] = cachedCfgVersion;
  doc["stm_tx_queue"] = stmTxCount;
  doc["uptime_ms"] = millis();
  publishJson(TOPIC_GATEWAY_STATUS, doc, true);
}

void publishGatewayAck(const String& ackFor, const char* status, const char* message, const char* errorCode = "") {
  DynamicJsonDocument doc(512);
  doc["type"] = "ACK";
  doc["ack_for"] = ackFor;
  doc["gateway_id"] = GW_ID;
  doc["control_id"] = CONTROL_ID;
  doc["from"] = "ESP32";
  doc["status"] = status;
  doc["message"] = message;
  if (strlen(errorCode) > 0) doc["error_code"] = errorCode;
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
  doc["control_id"] = CONTROL_ID;
  doc["device_id"] = CONTROL_ID;
  doc["device_key"] = FLOW2_DEVICE_KEY;
  doc["stm_id"] = STM_ID;
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
  doc["control_id"] = CONTROL_ID;
  doc["device_id"] = CONTROL_ID;
  doc["device_key"] = FLOW2_DEVICE_KEY;
  doc["stm_id"] = STM_ID;
  doc["from"] = "ESP32";
  doc["localCfgVersion"] = cachedCfgVersion;
  doc["timestamp_ms"] = millis();
  publishJson(TOPIC_STM32_SYNC_REQUEST, doc, false);
}

// ====================================================== //
// NTP TIME -> STM32
// ====================================================== //
void setupNtpTime() {
#if ENABLE_NTP_TIME_SYNC
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.google.com", "time.nist.gov");
  Serial.println("[NTP] configTime GMT+7");
#endif
}

bool getLocalHourMinute(int& hh, int& mm) {
#if ENABLE_NTP_TIME_SYNC
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 80)) return false;
  hh = timeinfo.tm_hour;
  mm = timeinfo.tm_min;
  return true;
#else
  return false;
#endif
}

void queueSetTimeToSTM32(bool ackReq = false) {
  int hh = 0;
  int mm = 0;

  if (!getLocalHourMinute(hh, mm)) {
    Serial.println("[NTP] Time not ready, skip auto SET_TIME");
    return;
  }

  DynamicJsonDocument doc(256);
  doc["msg_id"] = makeMsgId("ESP_TIME");
  doc["type"] = "CMD";
  doc["cmd"] = "SET_TIME";
  doc["ack_req"] = ackReq;
  doc["gateway_id"] = GW_ID;

  doc["hour"] = hh;
  doc["minute"] = mm;

  if (ackReq) addPending(doc["msg_id"].as<String>(), PENDING_STM32_ACK);
  sendJsonToSTM32(doc);
}

// ====================================================== //
// DEVICE SUBSCRIBE
// ====================================================== //
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
  DynamicJsonDocument doc(2048);
  String msgId = makeMsgId("ESP_DEVLIST");

  doc["msg_id"] = msgId;
  doc["type"] = "DEVICE_LIST";
  doc["gateway_id"] = GW_ID;
  doc["ack_req"] = false;
  doc["count"] = deviceCount;

  JsonArray arr = doc.createNestedArray("devices");
  for (uint8_t i = 0; i < deviceCount; i++) {
    JsonObject d = arr.createNestedObject();
    d["id_device"] = devices[i].id;
    d["name"] = devices[i].name.length() ? devices[i].name : devices[i].id;
  }

  // Danh sách cảm biến gửi gọn, không kèm topic để tránh frame UART dài.
  sendJsonToSTM32(doc);
}

void handleDeviceListResponse(JsonDocument& doc) {
  String msgId = doc["msg_id"] | "";
  String status = doc["status"] | "OK";
  status.toUpperCase();
  if (status != "OK" && status != "SUCCESS") {
    publishGatewayAck(msgId, "ERR", doc["message"] | "device list rejected", doc["error_code"] | "DEVICE_LIST_REJECTED");
    return;
  }

  unsubscribeOldDevices();
  deviceCount = 0;

  JsonArray arr;
  if (doc["devices"].is<JsonArray>()) {
    arr = doc["devices"].as<JsonArray>();
  } else if (doc["data"]["devices"].is<JsonArray>()) {
    arr = doc["data"]["devices"].as<JsonArray>();
  } else {
    publishGatewayAck(msgId, "ERR", "devices array missing", "DEVICE_LIST_INVALID");
    return;
  }

  for (JsonObject d : arr) {
    if (deviceCount >= MAX_DEVICES) break;

    String id = d["id_device"] | "";
    if (id.length() == 0) id = d["id"] | "";
    String name = d["name"] | "";
    if (name.length() == 0) name = id;
    String topic = d["topic"] | "";
    if (topic.length() == 0) topic = d["topicNew"] | "";

    if (id.length() == 0) continue;
    if (topic.length() == 0) topic = "maydokhongkhi/" + id + "/data";

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

// ====================================================== //
// FLOW 2 /Farm HELPERS
// ====================================================== //
void handleFlow2AuthRequest(JsonDocument& doc) {
#if ENABLE_FLOW2_COMPAT
  String msgId = getAnyStr(doc, "msg_id", "request_id", "id", nullptr);
  String username = getAnyStr(doc, "username", "user", nullptr, nullptr);

  // Chấp nhận nhiều tên trường để khớp web cũ/mới:
  // device_id / id_device / control_id / gateway_id / deviceCode
  String deviceId = getAnyStr(doc, "device_id", "id_device", "control_id", "gateway_id");
  if (deviceId.length() == 0) deviceId = getAnyStr(doc, "deviceCode", "cabinet_id", nullptr, nullptr);

  // Chấp nhận nhiều tên khóa: device_key / deviceKey / key
  String deviceKey = getAnyStr(doc, "device_key", "deviceKey", "key", nullptr);

  bool idOk = false;
  if (deviceId == FLOW2_DEVICE_ID) idOk = true;
  if (deviceId == FLOW2_LEGACY_DEVICE_ID) idOk = true;
  if (deviceId == CONTROL_ID) idOk = true;
  if (deviceId == GW_ID) idOk = true;

  // Topic nong_trai/auth/request là topic chung cho nhiều tủ.
  // Nếu không đúng mã tủ thì ESP này không trả lời để tránh nhầm thiết bị khác.
  if (!idOk) {
    Serial.print("[AUTH REQUEST] Không phải mã tủ này, bỏ qua device_id=");
    Serial.println(deviceId);
    return;
  }

  bool ok = (deviceKey == FLOW2_DEVICE_KEY);

  DynamicJsonDocument res(768);
  res["type"] = "AUTH_RESPONSE";
  if (msgId.length()) {
    res["msg_id"] = msgId;
    res["ack_for"] = msgId;
  }
  res["username"] = username;
  res["device_id"] = CONTROL_ID;
  res["id_device"] = CONTROL_ID;
  res["control_id"] = CONTROL_ID;
  res["gateway_id"] = GW_ID;
  res["cabinet_id"] = CONTROL_ID;
  res["stm_id"] = STM_ID;
  res["status"] = ok ? "ok" : "error";
  res["success"] = ok;
  res["authenticated"] = ok;
  res["message"] = ok ? "Xác thực tủ điều khiển thành công" : "Sai khóa thiết bị";
  res["fw"] = FW_VERSION;
  res["ip"] = WiFi.localIP().toString();
  res["rssi"] = WiFi.RSSI();
  res["uptime_ms"] = millis();
  res["timestamp_ms"] = millis();

  // Trả về topic chung để Node-RED đang dùng bắt được.
  publishJson(TOPIC_FLOW2_AUTH_RESPONSE, res, false);

  // Trả thêm topic riêng theo mã tủ để dễ xem trên MQTT Explorer và tránh lẫn nhiều tủ.
  String topicByDevice = String(TOPIC_FLOW2_AUTH_RESPONSE) + "/" + CONTROL_ID;
  publishJson(topicByDevice.c_str(), res, false);

  Serial.print("[AUTH RESPONSE] ");
  Serial.println(ok ? "OK" : "ERROR");
#endif
}

void publishFlow2SensorData(const String& idDevice, JsonDocument& inDoc) {
#if ENABLE_FLOW2_COMPAT
  DynamicJsonDocument out(768);
  String dev = idDevice;
  if (dev.length() == 0) dev = FLOW2_DEVICE_ID;

  out["device_id"] = dev;
  out["id_device"] = dev;
  out["temperature"] = getFloatAlias(inDoc, "temperature", "temp", "t", 0.0f);
  out["humidity"]    = getFloatAlias(inDoc, "humidity", "humi", "h", 0.0f);
  out["co2"]         = getFloatAlias(inDoc, "co2", "CO2", "", 0.0f);
  out["light"]       = getFloatAlias(inDoc, "light", "lux", "", 0.0f);

  // Flow 2 bản Farm có thêm các cột này; nếu sensor không có thì gửi 0.
  out["voltage"]   = getFloatAlias(inDoc, "voltage", "volt", "v", 0.0f);
  out["current"]   = getFloatAlias(inDoc, "current", "amp", "a", 0.0f);
  out["frequency"] = getFloatAlias(inDoc, "frequency", "freq", "f", 0.0f);
  out["power"]     = getFloatAlias(inDoc, "power", "watt", "p", 0.0f);

  out["gateway_id"] = GW_ID;
  out["fw"] = FW_VERSION;
  out["timestamp_ms"] = millis();

  publishJson(TOPIC_FLOW2_SENSOR_DATA, out, false);
#endif
}

void flow2DummySensorTask() {
#if ENABLE_FLOW2_COMPAT && ENABLE_FLOW2_DUMMY_SENSOR
  static uint32_t last = 0;
  if (millis() - last < FLOW2_DUMMY_SENSOR_MS) return;
  last = millis();

  DynamicJsonDocument doc(512);
  doc["device_id"] = CONTROL_ID;
  doc["temperature"] = 28.0 + ((millis() / 1000) % 50) / 10.0;
  doc["humidity"] = 60.0;
  doc["co2"] = 420;
  doc["light"] = 700;
  doc["voltage"] = 220;
  doc["current"] = 0.5;
  doc["frequency"] = 50;
  doc["power"] = 110;

  publishFlow2SensorData(FLOW2_DEVICE_ID, doc);
#endif
}

// ====================================================== //
// SENSOR MQTT -> STM32
// ====================================================== //
void forwardSensorToSTM32(const String& topic, JsonDocument& inDoc) {
  DynamicJsonDocument out(512);

  String idDevice = inDoc["id_device"] | "";
  if (idDevice.length() == 0) idDevice = inDoc["device_id"] | "";
  if (idDevice.length() == 0) idDevice = inDoc["id"] | "";
  if (idDevice.length() == 0) {
    int p1 = topic.indexOf('/');
    int p2 = topic.indexOf('/', p1 + 1);
    if (p1 >= 0) {
      if (p2 > p1) idDevice = topic.substring(p1 + 1, p2);
      else idDevice = topic.substring(p1 + 1);
    }
  }

  // UART xuống STM32 dùng format gọn để không bị cắt frame:
  // t/h/c/l/v/a/f/p = nhiệt độ/độ ẩm/CO2/ánh sáng/điện áp/dòng/tần số/công suất.
  out["type"] = "SENSOR";
  out["id_device"] = idDevice;
  out["t"] = getFloatAlias(inDoc, "temperature", "temp", "t", 0.0f);
  out["h"] = getFloatAlias(inDoc, "humidity", "humi", "h", 0.0f);
  out["c"] = getFloatAlias(inDoc, "co2", "CO2", "c", 0.0f);
  out["l"] = getFloatAlias(inDoc, "light", "lux", "l", 0.0f);
  out["v"] = getFloatAlias(inDoc, "voltage", "volt", "v", 0.0f);
  out["a"] = getFloatAlias(inDoc, "current", "amp", "a", 0.0f);
  out["f"] = getFloatAlias(inDoc, "frequency", "freq", "f", 0.0f);
  out["p"] = getFloatAlias(inDoc, "power", "watt", "p", 0.0f);

  sendJsonToSTM32(out);

  // MQTT/DB vẫn giữ payload đầy đủ cho dashboard và lịch sử.
  publishFlow2SensorData(idDevice, inDoc);
}

// ====================================================== //
// MQTT SERVER -> STM32 COMMAND NORMALIZER
// ====================================================== //
void forwardPreparedToSTM32(DynamicJsonDocument& out) {
  String msgId = out["msg_id"] | "";
  if (msgId.length() == 0) {
    msgId = makeMsgId("MQTT_CMD");
    out["msg_id"] = msgId;
  }

  bool ackReq = getBoolAckReq(out, true);
  out["ack_req"] = ackReq;
  out["gateway_id"] = GW_ID;

  if (ackReq) addPending(msgId, PENDING_STM32_ACK);
  sendJsonToSTM32(out);
}

void forwardSetMode(JsonDocument& inDoc) {
  String msgId = inDoc["msg_id"] | "";
  if (msgId.length() == 0) msgId = makeMsgId("MQTT_MODE");
  bool ackReq = getBoolAckReq(inDoc, true);
  String mode = getStr(inDoc, "mode", "");
  if (mode.length() == 0) mode = getStr(inDoc, "value", "");

  mode.trim();
  mode.toUpperCase();

  if (!(mode == "MANUAL" || mode == "TIMER" || mode == "SENSOR")) {
    publishGatewayAck(msgId, "ERR", "mode invalid or missing", "MODE_INVALID");
    return;
  }

  DynamicJsonDocument out(512);
  out["msg_id"] = msgId;
  out["type"] = "CMD";
  out["cmd"] = "SET_MODE";
  out["ack_req"] = ackReq;
  out["mode"] = mode;

  forwardPreparedToSTM32(out);
}

void forwardSetRelay(JsonDocument& inDoc) {
  String msgId = inDoc["msg_id"] | "";
  if (msgId.length() == 0) msgId = makeMsgId("MQTT_RELAY");
  bool ackReq = getBoolAckReq(inDoc, true);

  int relay = getInt(inDoc, "relay", 0);
  int state = getInt(inDoc, "state", -1);

  if (relay <= 0 || state < 0) {
    publishGatewayAck(msgId, "ERR", "relay/state missing", "RELAY_PAYLOAD_INVALID");
    return;
  }

  DynamicJsonDocument out(768);
  out["msg_id"] = msgId;
  out["type"] = "CMD";
  out["cmd"] = "SET_RELAY";
  out["ack_req"] = ackReq;
  out["mode"] = "MANUAL";
  out["relay"] = relay;
  out["state"] = state ? 1 : 0;

  forwardPreparedToSTM32(out);
}

void sendOneCompactTimerToSTM32(const String& msgId, bool ackReq, int relay, int index, int enable, const String& onTime, const String& offTime) {
#if SEND_TIME_BEFORE_TIMER
  // Cập nhật giờ STM trước khi lưu timer. Ở UART 115200 + queue nên không vỡ frame.
  queueSetTimeToSTM32(false);
#endif

  DynamicJsonDocument out(512);
  out["msg_id"] = msgId;
  out["type"] = "CMD";
  out["cmd"] = "SET_TIMER";
  out["ack_req"] = ackReq;
  out["relay"] = relay;
  out["index"] = index;
  out["enable"] = enable ? 1 : 0;
  out["on"] = onTime;
  out["off"] = offTime;

  forwardPreparedToSTM32(out);
}

void forwardSetTimer(JsonDocument& inDoc) {
  String msgId = inDoc["msg_id"] | "";
  if (msgId.length() == 0) msgId = makeMsgId("MQTT_TIMER");
  bool ackReq = getBoolAckReq(inDoc, true);
  JsonVariant data = inDoc["data"];

  // Format cũ: data.schedules[] hoặc schedules[]
  JsonArray schedules;
  bool hasArray = false;

  if (data.is<JsonObject>() && data["schedules"].is<JsonArray>()) {
    schedules = data["schedules"].as<JsonArray>();
    hasArray = true;
  } else if (inDoc["schedules"].is<JsonArray>()) {
    schedules = inDoc["schedules"].as<JsonArray>();
    hasArray = true;
  }

  int relay = getInt(inDoc, "relay", 0);
  if (relay <= 0) {
    publishGatewayAck(msgId, "ERR", "relay missing", "RELAY_MISSING");
    return;
  }

  if (hasArray) {
    int count = schedules.size();
    if (count <= 0) {
      publishGatewayAck(msgId, "ERR", "schedules empty", "SCHEDULE_EMPTY");
      return;
    }

    int n = 0;
    for (JsonObject s : schedules) {
      n++;
      int index = s["index"] | n;
      int enable = s["enable"] | 0;
      String onTime = s["on"] | "00:00";
      String offTime = s["off"] | "00:00";

      String childMsgId = msgId;
      if (count > 1) childMsgId = msgId + "_" + String(index);

      sendOneCompactTimerToSTM32(childMsgId, ackReq, relay, index, enable, onTime, offTime);
    }

    if (count > 1) {
      publishGatewayAck(msgId, "OK", "TIMER_LIST_SPLIT_TO_COMPACT");
    }
    return;
  }

  // Format mới compact: relay/index/enable/on/off ở root hoặc data
  int index = getInt(inDoc, "index", 1);
  int enable = getInt(inDoc, "enable", 0);
  String onTime = getStr(inDoc, "on", "00:00");
  String offTime = getStr(inDoc, "off", "00:00");

  sendOneCompactTimerToSTM32(msgId, ackReq, relay, index, enable, onTime, offTime);
}

void forwardSetSensorRule(JsonDocument& inDoc) {
  String msgId = inDoc["msg_id"] | "";
  if (msgId.length() == 0) msgId = makeMsgId("MQTT_SENSOR_RULE");
  bool ackReq = getBoolAckReq(inDoc, true);

  int relay = getInt(inDoc, "relay", 0);
  String idDevice = getStr(inDoc, "id_device", "");
  if (idDevice.length() == 0) idDevice = getStr(inDoc, "device_id", "");
  String field = getStr(inDoc, "field", "temperature");
  String logic = getStr(inDoc, "logic", "");
  if (logic.length() == 0) logic = getStr(inDoc, "op", ">");

  logic.trim();
  logic.toUpperCase();
  if (logic == ">" || logic == ">=" || logic == "ABOVE") logic = "ABOVE";
  else if (logic == "<" || logic == "<=" || logic == "BELOW") logic = "BELOW";
  else logic = "ABOVE";

  int enable = getInt(inDoc, "enable", 1);

  // Hỗ trợ cả value đơn giản và ngưỡng hysteresis
  float onValue = 0;
  float offValue = 0;
  JsonVariant data = inDoc["data"];

  if (logic == "ABOVE") {
    if (!data.isNull() && !data["onAbove"].isNull()) onValue = data["onAbove"].as<float>();
    else if (!inDoc["onAbove"].isNull()) onValue = inDoc["onAbove"].as<float>();
    else if (!data.isNull() && !data["value"].isNull()) onValue = data["value"].as<float>();
    else if (!inDoc["value"].isNull()) onValue = inDoc["value"].as<float>();

    if (!data.isNull() && !data["offBelow"].isNull()) offValue = data["offBelow"].as<float>();
    else if (!inDoc["offBelow"].isNull()) offValue = inDoc["offBelow"].as<float>();
    else offValue = onValue - 5.0f;
  } else {
    if (!data.isNull() && !data["onBelow"].isNull()) onValue = data["onBelow"].as<float>();
    else if (!inDoc["onBelow"].isNull()) onValue = inDoc["onBelow"].as<float>();
    else if (!data.isNull() && !data["value"].isNull()) onValue = data["value"].as<float>();
    else if (!inDoc["value"].isNull()) onValue = inDoc["value"].as<float>();

    if (!data.isNull() && !data["offAbove"].isNull()) offValue = data["offAbove"].as<float>();
    else if (!inDoc["offAbove"].isNull()) offValue = inDoc["offAbove"].as<float>();
    else offValue = onValue + 5.0f;
  }

  DynamicJsonDocument out(1024);
  out["msg_id"] = msgId;
  out["type"] = "CMD";
  out["cmd"] = "SET_SENSOR_RULE";
  out["ack_req"] = ackReq;
  out["relay"] = relay;
  out["enable"] = enable ? 1 : 0;
  out["id_device"] = idDevice;
  out["device_id"] = idDevice;
  out["field"] = field;
  out["logic"] = logic;

  if (logic == "ABOVE") {
    out["onAbove"] = onValue;
    out["offBelow"] = offValue;
  } else {
    out["onBelow"] = onValue;
    out["offAbove"] = offValue;
  }

  forwardPreparedToSTM32(out);
}

void forwardGenericCmdToSTM32(JsonDocument& inDoc) {
  DynamicJsonDocument out(4096);
  out.set(inDoc);

  String msgId = out["msg_id"] | "";
  if (msgId.length() == 0) {
    msgId = makeMsgId("MQTT_CMD");
    out["msg_id"] = msgId;
  }

  out["type"] = "CMD";
  out["gateway_id"] = GW_ID;

  if (out["ack_req"].isNull()) out["ack_req"] = true;

  forwardPreparedToSTM32(out);
}

void forwardCmdToSTM32(JsonDocument& doc) {
  String cmd = getStr(doc, "cmd", "");
  cmd.trim();
  cmd.toUpperCase();

  if (cmd.length() == 0) {
    String msgId = doc["msg_id"] | "";
    publishGatewayAck(msgId, "ERR", "cmd missing", "CMD_MISSING");
    return;
  }

  if (cmd == "SET_MODE") {
    forwardSetMode(doc);
  } else if (cmd == "SET_RELAY") {
    forwardSetRelay(doc);
  } else if (cmd == "SET_TIMER") {
    forwardSetTimer(doc);
  } else if (cmd == "SET_SENSOR_RULE") {
    forwardSetSensorRule(doc);
  } else {
    forwardGenericCmdToSTM32(doc);
  }
}

void queueCompactTimerToSTM32(const String& msgId, int relay, int index, int enable, const String& onTime, const String& offTime) {
  DynamicJsonDocument out(384);
  out["msg_id"] = msgId;
  out["type"] = "CMD";
  out["cmd"] = "SET_TIMER";
  out["ack_req"] = false;
  out["gateway_id"] = GW_ID;
  out["relay"] = relay;
  out["index"] = index;
  out["enable"] = enable ? 1 : 0;
  out["on"] = onTime;
  out["off"] = offTime;
  sendJsonToSTM32(out);
}

void queueCompactSensorRuleToSTM32(const String& msgId, JsonObject item) {
  int relay = item["relay"] | 0;
  if (relay <= 0) return;

  String logic = item["logic"] | "ABOVE";
  logic.trim();
  logic.toUpperCase();
  if (!(logic == "ABOVE" || logic == "BELOW")) logic = "ABOVE";

  DynamicJsonDocument out(512);
  out["msg_id"] = msgId;
  out["type"] = "CMD";
  out["cmd"] = "SET_SENSOR_RULE";
  out["ack_req"] = false;
  out["gateway_id"] = GW_ID;
  out["relay"] = relay;
  out["enable"] = (int)(item["enable"] | 1) ? 1 : 0;
  const char* idDevice = item["id_device"] | "";
  if (!idDevice || strlen(idDevice) == 0) idDevice = item["device_id"] | "";
  out["id_device"] = idDevice;
  out["device_id"] = idDevice;
  out["field"] = item["field"] | "temperature";
  out["logic"] = logic;

  if (logic == "ABOVE") {
    out["onAbove"] = item["onAbove"] | item["onValue"] | 0.0f;
    out["offBelow"] = item["offBelow"] | item["offValue"] | 0.0f;
  } else {
    out["onBelow"] = item["onBelow"] | item["onValue"] | 0.0f;
    out["offAbove"] = item["offAbove"] | item["offValue"] | 0.0f;
  }
  sendJsonToSTM32(out);
}

void forwardSyncResponseToSTM32(JsonDocument& doc) {
  String parentMsgId = doc["msg_id"] | "";
  if (parentMsgId.length() == 0) parentMsgId = makeMsgId("NR_SYNCRESP");

  // Không gửi nguyên CONFIG_SYNC_RESPONSE dài xuống STM32.
  // ESP tách thành nhiều lệnh nhỏ: DEVICE_LIST, SET_TIMER, SET_SENSOR_RULE, SET_FAN, SET_MODE.
  if (doc["devices"].is<JsonArray>() || doc["data"]["devices"].is<JsonArray>()) {
    DynamicJsonDocument dl(2048);
    dl["msg_id"] = parentMsgId + "_DEV";
    dl["type"] = "DEVICE_LIST";
    dl["gateway_id"] = GW_ID;
    dl["ack_req"] = false;
    JsonArray src = doc["devices"].is<JsonArray>() ? doc["devices"].as<JsonArray>() : doc["data"]["devices"].as<JsonArray>();
    JsonArray arr = dl.createNestedArray("devices");
    int count = 0;
    for (JsonObject d : src) {
      JsonObject o = arr.createNestedObject();
      const char* id = d["id_device"] | "";
      if (!id || strlen(id) == 0) id = d["device_id"] | "";
      if (!id || strlen(id) == 0) id = d["id"] | "";
      const char* name = d["name"] | "";
      if (!name || strlen(name) == 0) name = d["device_name"] | "";
      if (!name || strlen(name) == 0) name = id;
      o["id_device"] = id;
      o["name"] = name;
      count++;
    }
    dl["count"] = count;
    sendJsonToSTM32(dl);
  }

  JsonArray timers;
  if (doc["timer_rules"].is<JsonArray>()) timers = doc["timer_rules"].as<JsonArray>();
  else if (doc["data"]["timer_rules"].is<JsonArray>()) timers = doc["data"]["timer_rules"].as<JsonArray>();

  if (!timers.isNull()) {
    int n = 0;
    for (JsonObject relayObj : timers) {
      int relay = relayObj["relay"] | 0;
      if (relay <= 0) continue;
      if (relayObj["schedules"].is<JsonArray>()) {
        for (JsonObject sc : relayObj["schedules"].as<JsonArray>()) {
          n++;
          int index = sc["index"] | n;
          int enable = sc["enable"] | 0;
          String onTime = sc["on"] | "00:00";
          String offTime = sc["off"] | "00:00";
          queueCompactTimerToSTM32(parentMsgId + "_T" + String(relay) + "_" + String(index), relay, index, enable, onTime, offTime);
        }
      } else {
        n++;
        int index = relayObj["index"] | 1;
        int enable = relayObj["enable"] | 0;
        String onTime = relayObj["on"] | "00:00";
        String offTime = relayObj["off"] | "00:00";
        queueCompactTimerToSTM32(parentMsgId + "_T" + String(relay) + "_" + String(index), relay, index, enable, onTime, offTime);
      }
    }
  }

  JsonArray rules;
  if (doc["sensor_rules"].is<JsonArray>()) rules = doc["sensor_rules"].as<JsonArray>();
  else if (doc["data"]["sensor_rules"].is<JsonArray>()) rules = doc["data"]["sensor_rules"].as<JsonArray>();

  if (!rules.isNull()) {
    int n = 0;
    for (JsonObject item : rules) {
      n++;
      queueCompactSensorRuleToSTM32(parentMsgId + "_S" + String(n), item);
    }
  }

  if (!doc["fanMode"].isNull() || !doc["fanOnTemp"].isNull() || !doc["fanOffTemp"].isNull()) {
    DynamicJsonDocument fan(384);
    fan["msg_id"] = parentMsgId + "_FAN";
    fan["type"] = "CMD";
    fan["cmd"] = "SET_FAN_THRESHOLD";
    fan["ack_req"] = false;
    fan["gateway_id"] = GW_ID;
    if (!doc["fanMode"].isNull()) fan["fanMode"] = doc["fanMode"];
    if (!doc["fanOnTemp"].isNull()) fan["fanOnTemp"] = doc["fanOnTemp"];
    if (!doc["fanOffTemp"].isNull()) fan["fanOffTemp"] = doc["fanOffTemp"];
    sendJsonToSTM32(fan);
  }

  String mode = doc["mode"] | "";
  if (mode.length()) {
    DynamicJsonDocument modeDoc(256);
    modeDoc["msg_id"] = parentMsgId + "_MODE";
    modeDoc["type"] = "CMD";
    modeDoc["cmd"] = "SET_MODE";
    modeDoc["ack_req"] = false;
    modeDoc["gateway_id"] = GW_ID;
    modeDoc["mode"] = mode;
    sendJsonToSTM32(modeDoc);
  }

  publishGatewayAck(parentMsgId, "OK", "CONFIG_SYNC_SPLIT_TO_SMALL_UART_FRAMES");
}

void forwardServerAckToSTM32(JsonDocument& doc) {
  String ackFor = doc["ack_for"] | "";
  if (!ackFor.length()) return;

  PendingType t = getPendingType(ackFor);

  // ACK từ Node-RED cho lệnh UI chỉ xác nhận server đã nhận lệnh.
  // Không được xóa pending đang chờ STM32, cũng không gửi ACK này xuống STM32.
  // Nếu gửi xuống STM32, lệnh SET_RELAY/SET_MODE có thể bị che bởi ACK server và UI tưởng đã chạy dù STM chưa phản hồi.
  if (t == PENDING_STM32_ACK) {
    Serial.print("[SERVER ACK IGNORED - WAIT STM32] ");
    Serial.println(ackFor);
    return;
  }

  // Chỉ chuyển ACK server xuống STM32 khi đó là ACK cho dữ liệu do STM32 gửi lên
  // như RELAY_STATE, FULL_STATE, CONFIG_STATE, CONFIG_SYNC_REQUEST.
  if (t == PENDING_SERVER_ACK) {
    removePendingByType(ackFor, PENDING_SERVER_ACK);
    sendJsonToSTM32(doc);
    return;
  }

  Serial.print("[SERVER ACK IGNORED - NO PENDING] ");
  Serial.println(ackFor);
}

// ====================================================== //
// STM32 -> MQTT
// ====================================================== //
void handleSTM32Ack(JsonDocument& doc) {
  String ackFor = doc["ack_for"] | "";
  if (ackFor.length()) removePendingByType(ackFor, PENDING_STM32_ACK);

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

  if (ackReq && msgId.length()) addPending(msgId, PENDING_SERVER_ACK);
}

void handleSTM32Status(JsonDocument& doc) {
  uint32_t v = doc["cfgVersion"] | 0;
  if (v > cachedCfgVersion) cachedCfgVersion = v;

  doc["gateway_id"] = GW_ID;
  doc["timestamp_ms"] = millis();
  publishJson(TOPIC_STM32_STATUS, doc, false);

  // Nếu STATUS có active_relays, tách thêm từng relay thành topic relay/state
  // để UI nhận được thông tin nguồn bật relay mà không cần STATUS quá dài.
  if (doc["active_relays"].is<JsonArray>()) {
    for (JsonObject item : doc["active_relays"].as<JsonArray>()) {
      DynamicJsonDocument r(512);
      r["type"] = "RELAY_STATE";
      r["gateway_id"] = GW_ID;
      r["control_id"] = CONTROL_ID;
      r["mode"] = doc["mode"] | "";
      r["relay"] = item["relay"] | 0;
      r["state"] = item["state"] | 1;
      r["source"] = item["source"] | "";
      r["schedule_index"] = item["schedule_index"] | 0;
      if (!item["id_device"].isNull()) r["id_device"] = item["id_device"];
      r["field"] = item["field"] | "temperature";
      r["value"] = item["value"] | 0.0f;
      r["timestamp_ms"] = millis();
      publishJson(TOPIC_STM32_RELAY_STATE, r, false);
    }
  }
}

void handleSTM32ConfigState(JsonDocument& doc) {
  String msgId = doc["msg_id"] | "";
  bool ackReq = doc["ack_req"] | false;

  doc["gateway_id"] = GW_ID;
  doc["timestamp_ms"] = millis();
  publishJson(TOPIC_STM32_CONFIG_STATE, doc, false);

  if (ackReq && msgId.length()) addPending(msgId, PENDING_SERVER_ACK);
}

void handleSTM32FullState(JsonDocument& doc) {
  String msgId = doc["msg_id"] | "";
  bool ackReq = doc["ack_req"] | false;

  uint32_t v = doc["cfgVersion"] | 0;
  if (v > cachedCfgVersion) cachedCfgVersion = v;

  doc["gateway_id"] = GW_ID;
  doc["timestamp_ms"] = millis();
  publishJson(TOPIC_STM32_FULL_STATE, doc, false);

  if (ackReq && msgId.length()) addPending(msgId, PENDING_SERVER_ACK);
}

void handleSTM32SyncRequest(JsonDocument& doc) {
  String msgId = doc["msg_id"] | "";
  bool ackReq = doc["ack_req"] | false;

  doc["gateway_id"] = GW_ID;
  doc["from"] = "STM32";
  doc["timestamp_ms"] = millis();
  publishJson(TOPIC_STM32_SYNC_REQUEST, doc, false);

  if (ackReq && msgId.length()) addPending(msgId, PENDING_SERVER_ACK);
}

void handleSTM32Boot(JsonDocument& doc) {
  doc["gateway_id"] = GW_ID;
  doc["timestamp_ms"] = millis();
  publishJson(TOPIC_STM32_BOOT, doc, false);

  if (deviceCount > 0) {
    Serial.println("[STM32 BOOT] resend cached device list");
    sendDeviceListToSTM32();
  }
  requestConfigSyncFromServer();
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
  } else if (type == "RELAY_STATE" || type == "EVENT") {
    handleSTM32RelayState(doc);
  } else if (type == "STATUS") {
    handleSTM32Status(doc);
  } else if (type == "CONFIG_STATE") {
    handleSTM32ConfigState(doc);
  } else if (type == "FULL_STATE") {
    handleSTM32FullState(doc);
  } else if (type == "CONFIG_SYNC_REQUEST") {
    handleSTM32SyncRequest(doc);
  } else if (type == "FAN_STATE") {
    doc["gateway_id"] = GW_ID;
    doc["timestamp_ms"] = millis();
    publishJson(TOPIC_STM32_FAN_STATE, doc, false);
  } else if (type == "BOOT") {
    handleSTM32Boot(doc);
  } else {
    // Không bỏ dữ liệu lạ, publish vào status để debug
    handleSTM32Status(doc);
  }
}

void readSTM32Uart() {
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    if (c == '\r') continue;

    if (c == '\n') {
      uartLine.trim();
      if (uartLine.length()) processSTM32Line(uartLine);
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

// ====================================================== //
// MQTT CALLBACK
// ====================================================== //
void mqttCallback(char* topicRaw, byte* payload, unsigned int length) {
  String topic = String(topicRaw);
  String payloadStr;
  payloadStr.reserve(length + 1);

  for (unsigned int i = 0; i < length; i++) payloadStr += (char)payload[i];

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
    publishGatewayAck("", "ERR", "MQTT JSON invalid", "MQTT_JSON_INVALID");
    return;
  }

  if (topic == TOPIC_FLOW2_AUTH_REQUEST) {
    handleFlow2AuthRequest(doc);
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

  if (isStm32CmdTopic(topic)) {
    forwardCmdToSTM32(doc);
    return;
  }

  if (isDeviceTopic(topic)) {
    forwardSensorToSTM32(topic, doc);
    return;
  }

  Serial.println("[MQTT] Topic ignored");
}

// ====================================================== //
// MQTT CONNECT
// ====================================================== //
void mqttSubscribeBaseTopics() {
  mqtt.subscribe(TOPIC_DEVICE_RES);
  Serial.print("[SUB] ");
  Serial.println(TOPIC_DEVICE_RES);

  mqtt.subscribe(TOPIC_STM32_CMD);
  Serial.print("[SUB] ");
  Serial.println(TOPIC_STM32_CMD);

  mqtt.subscribe(TOPIC_STM32_CMD_WILDCARD);
  Serial.print("[SUB] ");
  Serial.println(TOPIC_STM32_CMD_WILDCARD);

  mqtt.subscribe(TOPIC_SERVER_ACK);
  Serial.print("[SUB] ");
  Serial.println(TOPIC_SERVER_ACK);

  mqtt.subscribe(TOPIC_STM32_SYNC_RESPONSE);
  Serial.print("[SUB] ");
  Serial.println(TOPIC_STM32_SYNC_RESPONSE);

#if ENABLE_FLOW2_COMPAT
  mqtt.subscribe(TOPIC_FLOW2_AUTH_REQUEST);
  Serial.print("[SUB FLOW2 AUTH] ");
  Serial.println(TOPIC_FLOW2_AUTH_REQUEST);
#endif

  subscribeDeviceTopics();
}

void ensureAuthRequestSubscribedTask() {
#if ENABLE_FLOW2_COMPAT
  static uint32_t lastSubMs = 0;
  if (!mqtt.connected()) return;
  if (millis() - lastSubMs < 30000UL) return;
  lastSubMs = millis();
  mqtt.subscribe(TOPIC_FLOW2_AUTH_REQUEST);
#endif
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
      ok = mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS,
                        TOPIC_GATEWAY_STATUS, 0, true, willPayload.c_str());
    } else {
      ok = mqtt.connect(clientId.c_str(),
                        TOPIC_GATEWAY_STATUS, 0, true, willPayload.c_str());
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

// ====================================================== //
// WIFI
// ====================================================== //
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

// ====================================================== //
// HEARTBEAT
// ====================================================== //
void heartbeatTask() {
  static uint32_t last = 0;
  if (millis() - last >= 10000) {
    last = millis();
    if (mqtt.connected()) publishGatewayStatus("ONLINE");
  }
}

void periodicTimeSyncTask() {
#if ENABLE_NTP_TIME_SYNC
  static uint32_t last = 0;
  if (millis() - last >= 60000UL) {
    last = millis();
    queueSetTimeToSTM32(false);
  }
#endif
}

// ====================================================== //
// SETUP / LOOP
// ====================================================== //
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("======================================");
  Serial.println("ESP32 GATEWAY FUVIAIR V10 AUTH REQUEST START");
  Serial.println("======================================");

  buildTopics();
  clearPendings();

  Serial2.begin(STM32_BAUD, SERIAL_8N1, STM32_RX_PIN, STM32_TX_PIN);

  wifiConnect();
  setupNtpTime();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(8192);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(10);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnect();
  }

  mqttReconnect();
  mqtt.loop();
  ensureAuthRequestSubscribedTask();

  readSTM32Uart();
  stmTxTask();

  checkPendingTimeouts();
  heartbeatTask();
  periodicTimeSyncTask();
  flow2DummySensorTask();
}
