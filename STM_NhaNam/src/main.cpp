#include <Arduino.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

// ======================================================
// STM32 F103C8 - FUVIAIR RELAY CONTROLLER V2
// Có:
// - FULL_STATE
// - CONFIG_SYNC_REQUEST
// - CONFIG_SYNC_RESPONSE
// - SAVE_FLASH
// - cfgVersion
// - MANUAL / TIMER / SENSOR
// - ESP relay sync disabled: relay chỉ điều khiển local/HMI, không đồng bộ state lên ESP
// ======================================================

// ======================================================
// SERIAL CONFIG - STM32F103C8
// Không dùng Serial1/Serial2/Serial3 mặc định để tránh lỗi undefined reference.
// Khai báo HardwareSerial theo chân thật: HardwareSerial(RX, TX)
//
// DEBUG UART2: PA2 TX2, PA3 RX2
// HMI   UART1: PA9 TX1, PA10 RX1, baud 9600
// ESP32 UART3: PB10 TX3, PB11 RX3
// ======================================================
#define DEBUG_RX_PIN       PA3
#define DEBUG_TX_PIN       PA2
#define DEBUG_BAUD         115200

#define HMI_RX_PIN         PA10
#define HMI_TX_PIN         PA9
#define HMI_BAUD           9600

#define ESP_RX_PIN         PB11
#define ESP_TX_PIN         PB10
#define ESP_BAUD           115200

HardwareSerial DebugSerial(DEBUG_RX_PIN, DEBUG_TX_PIN);
HardwareSerial HMISerial(HMI_RX_PIN, HMI_TX_PIN);
HardwareSerial ESPSerial(ESP_RX_PIN, ESP_TX_PIN);

#define DEBUG_SERIAL DebugSerial
#define HMI_SERIAL   HMISerial
#define ESP_SERIAL   ESPSerial

// ======================================================
// RELAY CONFIG
// ======================================================
#define RELAY_COUNT 10

const uint8_t relayPins[RELAY_COUNT] = {
  PA0,   // K1
  PA1,   // K2
  PA4,   // K3
  PA5,   // K4
  PA6,   // K5
  PA7,   // K6
  PA8,   // K7
  PB1,   // K8
  PB5,   // K9
  PB8    // K10
};

// Nếu relay kích LOW thì đổi false
const bool RELAY_ACTIVE_HIGH = true;

// ======================================================
// LM35 + FAN + EMERGENCY CONFIG
// ======================================================
// LM35: analog input PB0, đo nhiệt độ tủ
// Fan: digital output PB3, điều khiển quạt làm mát
// Emergency: digital input PB12, ngắt khẩn cấp (LOW = kích hoạt)
#define LM35_PIN           PB0
#define FAN_PIN            PB3
#define EMERGENCY_PIN      PB12
#define FAN_ACTIVE_HIGH    true
#define EMERGENCY_ACTIVE   LOW

// Fan mode
#define FAN_OFF  0
#define FAN_AUTO 1
#define FAN_ON   2

#define LM35_READ_INTERVAL_MS  2000
#define EMERGENCY_CHECK_MS     500

// ======================================================
// SYSTEM CONFIG
// ======================================================
const char* STM_ID = "STM32_01";
const char* GW_ID  = "GW01";

#define MAX_DEVICES       20
#define MAX_SCHEDULES     10
#define MAX_PENDING_ACK   20
#define ACK_TIMEOUT_MS    5000
#define SENSOR_TIMEOUT_MS 30000

// ======================================================
// FLASH CONFIG
// ======================================================
#define CONFIG_MAGIC       0x46555633UL   // FUV3 (bump to reset flash after bugfix)
#define CONFIG_ADDR        0

// ======================================================
// MODE
// ======================================================
enum WorkMode : uint8_t {
  MODE_MANUAL = 0,
  MODE_TIMER  = 1,
  MODE_SENSOR = 2
};

// ======================================================
// SENSOR FIELD / LOGIC
// ======================================================
enum SensorField : uint8_t {
  FIELD_TEMP  = 0,
  FIELD_HUMI  = 1,
  FIELD_CO2   = 2,
  FIELD_LIGHT = 3
};

enum SensorLogic : uint8_t {
  LOGIC_ABOVE = 0,
  LOGIC_BELOW = 1
};

// ======================================================
// TIMER / SENSOR RULE
// ======================================================
struct __attribute__((packed)) TimerSchedule {
  uint8_t enable;
  uint16_t onMin;
  uint16_t offMin;
};

struct __attribute__((packed)) SensorRule {
  uint8_t enable;
  char idDevice[20];
  uint8_t field;
  uint8_t logic;
  float onValue;
  float offValue;
};

// ======================================================
// SAVED CONFIG
// ======================================================
struct __attribute__((packed)) SavedConfig {
  uint32_t magic;
  uint32_t cfgVersion;
  uint8_t mode;
  uint8_t relayState[RELAY_COUNT];
  TimerSchedule timerRules[RELAY_COUNT][MAX_SCHEDULES];
  SensorRule sensorRules[RELAY_COUNT];
  // Fan config
  uint8_t fanMode;       // 0=OFF, 1=AUTO, 2=ON
  float fanOnTemp;       // ngưỡng nhiệt độ bật quạt (°C)
  float fanOffTemp;      // ngưỡng nhiệt độ tắt quạt (°C)
  uint32_t checksum;
};

SavedConfig cfg;

// ======================================================
// RUNTIME DEVICE/SENSOR
// ======================================================
struct DeviceInfo {
  char id[20];
  char name[24];
};

struct SensorData {
  char id[20];
  float temperature;
  float humidity;
  float co2;
  float light;
  uint32_t lastMs;
  bool valid;
};

DeviceInfo deviceList[MAX_DEVICES];
SensorData sensorList[MAX_DEVICES];
uint8_t deviceCount = 0;

// ======================================================
// RUNTIME
// ======================================================
String espLine;
String hmiLine;

bool timeValid = false;
uint32_t baseEpoch = 0;
uint32_t baseMillis = 0;

struct PendingAck {
  bool used;
  String msgId;
  uint32_t startMs;
};

PendingAck pendingAcks[MAX_PENDING_ACK];

// Fan runtime
float cabinetTemp = 0.0f;
bool fanRunning = false;
uint8_t fanModeRuntime = FAN_AUTO;   // mirror cfg.fanMode hoặc override từ HMI/SRV
bool emergencyActive = false;

// ======================================================
// UTILS
// ======================================================
void safeCopy(char* dst, size_t len, const char* src) {
  if (!dst || len == 0) return;
  if (!src) src = "";
  strncpy(dst, src, len - 1);
  dst[len - 1] = '\0';
}

String makeMsgId(const char* prefix) {
  static uint32_t counter = 0;
  counter++;
  return String(prefix) + "-" + String(millis()) + "-" + String(counter);
}

const char* modeToString(uint8_t m) {
  switch (m) {
    case MODE_MANUAL: return "MANUAL";
    case MODE_TIMER:  return "TIMER";
    case MODE_SENSOR: return "SENSOR";
    default: return "UNKNOWN";
  }
}

uint8_t parseMode(const char* s) {
  if (!s) return cfg.mode;
  if (strcmp(s, "MANUAL") == 0 || strcmp(s, "manual") == 0) return MODE_MANUAL;
  if (strcmp(s, "TIMER") == 0 || strcmp(s, "timer") == 0) return MODE_TIMER;
  if (strcmp(s, "SENSOR") == 0 || strcmp(s, "sensor") == 0) return MODE_SENSOR;
  return cfg.mode;
}

SensorField parseField(const char* s) {
  if (!s) return FIELD_TEMP;
  if (strcmp(s, "temperature") == 0 || strcmp(s, "temp") == 0) return FIELD_TEMP;
  if (strcmp(s, "humidity") == 0 || strcmp(s, "humi") == 0) return FIELD_HUMI;
  if (strcmp(s, "co2") == 0) return FIELD_CO2;
  if (strcmp(s, "light") == 0) return FIELD_LIGHT;
  return FIELD_TEMP;
}

const char* fieldToString(uint8_t f) {
  switch (f) {
    case FIELD_TEMP:  return "temperature";
    case FIELD_HUMI:  return "humidity";
    case FIELD_CO2:   return "co2";
    case FIELD_LIGHT: return "light";
    default: return "temperature";
  }
}

SensorLogic parseLogic(const char* s) {
  if (!s) return LOGIC_ABOVE;
  if (strcmp(s, "BELOW") == 0 || strcmp(s, "below") == 0) return LOGIC_BELOW;
  return LOGIC_ABOVE;
}

const char* logicToString(uint8_t l) {
  return l == LOGIC_BELOW ? "BELOW" : "ABOVE";
}

int relayIndexFromNumber(int relay) {
  if (relay < 1 || relay > RELAY_COUNT) return -1;
  return relay - 1;
}

uint16_t parseHHMM(const char* t) {
  if (!t || strlen(t) < 4) return 0;

  int hh = 0;
  int mm = 0;

  sscanf(t, "%d:%d", &hh, &mm);

  if (hh < 0) hh = 0;
  if (hh > 23) hh = 23;
  if (mm < 0) mm = 0;
  if (mm > 59) mm = 59;

  return (uint16_t)(hh * 60 + mm);
}

void formatHHMM(uint16_t minOfDay, char* out, size_t len) {
  uint8_t hh = minOfDay / 60;
  uint8_t mm = minOfDay % 60;
  snprintf(out, len, "%02u:%02u", hh, mm);
}

// ======================================================
// CHECKSUM
// ======================================================
uint32_t calcChecksum(const SavedConfig& c) {
  const uint8_t* p = (const uint8_t*)&c;
  uint32_t sum = 0;

  size_t len = sizeof(SavedConfig) - sizeof(uint32_t);

  for (size_t i = 0; i < len; i++) {
    sum = (sum * 31) + p[i];
  }

  return sum;
}

// ======================================================
// FLASH SAVE / LOAD
// ======================================================
void defaultConfig() {
  memset(&cfg, 0, sizeof(cfg));

  cfg.magic = CONFIG_MAGIC;
  cfg.cfgVersion = 1;
  cfg.mode = MODE_MANUAL;

  for (int i = 0; i < RELAY_COUNT; i++) {
    cfg.relayState[i] = 0;
  }

  for (int r = 0; r < RELAY_COUNT; r++) {
    for (int s = 0; s < MAX_SCHEDULES; s++) {
      cfg.timerRules[r][s].enable = 0;
      cfg.timerRules[r][s].onMin = 0;
      cfg.timerRules[r][s].offMin = 0;
    }

    cfg.sensorRules[r].enable = 0;
    safeCopy(cfg.sensorRules[r].idDevice, sizeof(cfg.sensorRules[r].idDevice), "");
    cfg.sensorRules[r].field = FIELD_TEMP;
    cfg.sensorRules[r].logic = LOGIC_ABOVE;
    cfg.sensorRules[r].onValue = 0;
    cfg.sensorRules[r].offValue = 0;
  }

  // Fan defaults
  cfg.fanMode = FAN_AUTO;
  cfg.fanOnTemp = 35.0f;
  cfg.fanOffTemp = 30.0f;

  cfg.checksum = calcChecksum(cfg);
}

bool loadConfigFromFlash() {
  EEPROM.get(CONFIG_ADDR, cfg);

  if (cfg.magic != CONFIG_MAGIC) {
    DEBUG_SERIAL.println("[FLASH] Invalid magic");
    defaultConfig();
    return false;
  }

  uint32_t chk = calcChecksum(cfg);

  if (chk != cfg.checksum) {
    DEBUG_SERIAL.println("[FLASH] Invalid checksum");
    defaultConfig();
    return false;
  }

  if (cfg.mode > MODE_SENSOR) {
    cfg.mode = MODE_MANUAL;
  }

  DEBUG_SERIAL.print("[FLASH] Loaded cfgVersion=");
  DEBUG_SERIAL.println(cfg.cfgVersion);

  return true;
}

void saveConfigToFlash(bool increaseVersion) {
  if (increaseVersion) {
    cfg.cfgVersion++;
  }

  cfg.magic = CONFIG_MAGIC;
  cfg.checksum = calcChecksum(cfg);

  EEPROM.put(CONFIG_ADDR, cfg);

  DEBUG_SERIAL.print("[FLASH] Saved cfgVersion=");
  DEBUG_SERIAL.println(cfg.cfgVersion);
}

// ======================================================
// CLOCK
// ======================================================
uint32_t nowEpoch() {
  if (!timeValid) return millis() / 1000;
  return baseEpoch + ((millis() - baseMillis) / 1000);
}

uint16_t minuteOfDayNow() {
  uint32_t sec = nowEpoch() % 86400UL;
  return sec / 60;
}

void setClockFromEpoch(uint32_t epoch) {
  baseEpoch = epoch;
  baseMillis = millis();
  timeValid = true;
}

void setClockFromHM(uint8_t hh, uint8_t mm) {
  baseEpoch = (uint32_t)hh * 3600UL + (uint32_t)mm * 60UL;
  baseMillis = millis();
  timeValid = true;
}

// ======================================================
// JSON SEND
// ======================================================
void sendJsonToESP(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);

  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println("[STM32 -> ESP32]");
  DEBUG_SERIAL.println(out);

  ESP_SERIAL.print(out);
  ESP_SERIAL.print('\n');
}

void sendJsonToHMI(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);

  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println("[STM32 -> HMI]");
  DEBUG_SERIAL.println(out);

  HMI_SERIAL.print(out);
  HMI_SERIAL.print('\n');
}

void sendAckToESP(const String& ackFor, const char* status, const char* message, const char* errorCode = "") {
  DynamicJsonDocument doc(512);

  doc["type"] = "ACK";
  doc["ack_for"] = ackFor;
  doc["stm_id"] = STM_ID;
  doc["gateway_id"] = GW_ID;
  doc["status"] = status;
  doc["message"] = message;

  if (strlen(errorCode) > 0) {
    doc["error_code"] = errorCode;
  }

  doc["cfgVersion"] = cfg.cfgVersion;
  doc["timestamp_ms"] = millis();

  sendJsonToESP(doc);
}

void sendAckToHMI(const String& ackFor, const char* status, const char* message, const char* errorCode = "") {
  DynamicJsonDocument doc(512);

  doc["type"] = "ACK";
  doc["ack_for"] = ackFor;
  doc["stm_id"] = STM_ID;
  doc["status"] = status;
  doc["message"] = message;

  if (strlen(errorCode) > 0) {
    doc["error_code"] = errorCode;
  }

  doc["cfgVersion"] = cfg.cfgVersion;
  doc["timestamp_ms"] = millis();

  sendJsonToHMI(doc);
}

// ======================================================
// PENDING ACK
// ======================================================
void clearPendingAcks() {
  for (int i = 0; i < MAX_PENDING_ACK; i++) {
    pendingAcks[i].used = false;
    pendingAcks[i].msgId = "";
    pendingAcks[i].startMs = 0;
  }
}

bool addPendingAck(const String& msgId) {
  if (msgId.length() == 0) return false;

  for (int i = 0; i < MAX_PENDING_ACK; i++) {
    if (!pendingAcks[i].used) {
      pendingAcks[i].used = true;
      pendingAcks[i].msgId = msgId;
      pendingAcks[i].startMs = millis();

      DEBUG_SERIAL.print("[PENDING ADD] ");
      DEBUG_SERIAL.println(msgId);
      return true;
    }
  }

  DEBUG_SERIAL.println("[PENDING FULL]");
  return false;
}

bool removePendingAck(const String& msgId) {
  for (int i = 0; i < MAX_PENDING_ACK; i++) {
    if (pendingAcks[i].used && pendingAcks[i].msgId == msgId) {
      pendingAcks[i].used = false;

      DEBUG_SERIAL.print("[PENDING REMOVE] ");
      DEBUG_SERIAL.println(msgId);
      return true;
    }
  }

  return false;
}

void checkPendingAckTimeouts() {
  uint32_t now = millis();

  for (int i = 0; i < MAX_PENDING_ACK; i++) {
    if (!pendingAcks[i].used) continue;

    if (now - pendingAcks[i].startMs >= ACK_TIMEOUT_MS) {
      String id = pendingAcks[i].msgId;
      pendingAcks[i].used = false;

      DynamicJsonDocument doc(512);
      doc["type"] = "SERVER_ACK_TIMEOUT";
      doc["ack_for"] = id;
      doc["status"] = "TIMEOUT";
      doc["message"] = "Server did not ACK";
      doc["cfgVersion"] = cfg.cfgVersion;
      doc["timestamp_ms"] = millis();

      sendJsonToHMI(doc);
    }
  }
}

// ======================================================
// RELAY
// ======================================================
void relayWriteRaw(int idx, bool on) {
  if (idx < 0 || idx >= RELAY_COUNT) return;

  bool level = RELAY_ACTIVE_HIGH ? on : !on;
  digitalWrite(relayPins[idx], level ? HIGH : LOW);

  cfg.relayState[idx] = on ? 1 : 0;
}

void sendRelayStateToHMI(int idx) {
  DynamicJsonDocument doc(512);

  doc["type"] = "RELAY_STATE";
  doc["mode"] = modeToString(cfg.mode);
  doc["relay"] = idx + 1;
  doc["state"] = cfg.relayState[idx];
  doc["pin"] = relayPins[idx];
  doc["gpioLevel"] = digitalRead(relayPins[idx]) ? 1 : 0;
  doc["activeHigh"] = RELAY_ACTIVE_HIGH ? 1 : 0;
  doc["cfgVersion"] = cfg.cfgVersion;
  doc["timestamp_ms"] = millis();

  sendJsonToHMI(doc);
}

void sendRelayStateToESP(int idx, const char* source, bool ackReq) {
  DynamicJsonDocument doc(768);

  String msgId = makeMsgId("STM_RELAY");

  doc["msg_id"] = msgId;
  doc["type"] = "RELAY_STATE";
  doc["ack_req"] = ackReq;
  doc["stm_id"] = STM_ID;
  doc["gateway_id"] = GW_ID;
  doc["mode"] = modeToString(cfg.mode);
  doc["relay"] = idx + 1;
  doc["state"] = cfg.relayState[idx];
  doc["source"] = source;
  doc["cfgVersion"] = cfg.cfgVersion;
  doc["timestamp_ms"] = millis();

  if (ackReq) {
    addPendingAck(msgId);
  }

  sendJsonToESP(doc);
}

void setRelay(int idx, bool on, const char* source, bool syncServer, bool serverAckReq, bool saveFlashNow) {
  if (idx < 0 || idx >= RELAY_COUNT) return;

  bool old = cfg.relayState[idx] ? true : false;

  // Luôn ghi lại GPIO thật, kể cả trạng thái phần mềm đang giống nhau.
  // Việc này giúp tránh trường hợp log đổi nhưng chân relay chưa được refresh.
  relayWriteRaw(idx, on);

  DEBUG_SERIAL.print("[RELAY] R");
  DEBUG_SERIAL.print(idx + 1);
  DEBUG_SERIAL.print(" old=");
  DEBUG_SERIAL.print(old ? 1 : 0);
  DEBUG_SERIAL.print(" new=");
  DEBUG_SERIAL.print(on ? 1 : 0);
  DEBUG_SERIAL.print(" pin=");
  DEBUG_SERIAL.print(relayPins[idx]);
  DEBUG_SERIAL.print(" gpioLevel=");
  DEBUG_SERIAL.println(digitalRead(relayPins[idx]) ? 1 : 0);

  if (old != on && saveFlashNow) {
    saveConfigToFlash(true);
  }

  // HMI luôn được nhận trạng thái relay.
  sendRelayStateToHMI(idx);

  if (syncServer) {
    sendRelayStateToESP(idx, source, serverAckReq);
  }
}

void applyRelayOutputsFromConfig() {
  for (int i = 0; i < RELAY_COUNT; i++) {
    bool on = cfg.relayState[i] == 1;
    bool level = RELAY_ACTIVE_HIGH ? on : !on;
    digitalWrite(relayPins[i], level ? HIGH : LOW);
  }
}

void relayPhysicalTest(uint8_t relayNumber) {
  int idx = relayIndexFromNumber(relayNumber);

  if (idx < 0) {
    DEBUG_SERIAL.println("[TEST_RELAY] relay invalid");
    return;
  }

  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println("========== RELAY PHYSICAL TEST ==========");
  DEBUG_SERIAL.print("Relay: ");
  DEBUG_SERIAL.println(relayNumber);
  DEBUG_SERIAL.print("Pin: ");
  DEBUG_SERIAL.println(relayPins[idx]);

  for (int i = 0; i < 5; i++) {
    relayWriteRaw(idx, true);
    sendRelayStateToHMI(idx);
    delay(400);

    relayWriteRaw(idx, false);
    sendRelayStateToHMI(idx);
    delay(400);
  }

  DEBUG_SERIAL.println("========== RELAY TEST DONE ==========");
}

// ======================================================
// DEVICE / SENSOR
// ======================================================
int findDeviceIndex(const char* id) {
  if (!id) return -1;

  for (uint8_t i = 0; i < deviceCount; i++) {
    if (strcmp(deviceList[i].id, id) == 0) return i;
  }

  return -1;
}

int findSensorIndex(const char* id) {
  if (!id) return -1;

  for (uint8_t i = 0; i < deviceCount; i++) {
    if (strcmp(sensorList[i].id, id) == 0) return i;
  }

  return -1;
}

void sendDeviceListToHMI() {
  DynamicJsonDocument doc(2048);

  doc["type"] = "DEVICE_LIST";
  doc["count"] = deviceCount;

  JsonArray arr = doc.createNestedArray("devices");

  for (uint8_t i = 0; i < deviceCount; i++) {
    JsonObject d = arr.createNestedObject();
    d["id"] = deviceList[i].id;
    d["name"] = deviceList[i].name;
  }

  sendJsonToHMI(doc);
}

float getSensorValue(const SensorData& s, uint8_t f) {
  switch (f) {
    case FIELD_TEMP:  return s.temperature;
    case FIELD_HUMI:  return s.humidity;
    case FIELD_CO2:   return s.co2;
    case FIELD_LIGHT: return s.light;
    default: return 0;
  }
}

// ======================================================
// FULL STATE
// ======================================================
void buildFullState(JsonDocument& doc, const String& msgId, bool ackReq) {
  doc["msg_id"] = msgId;
  doc["type"] = "FULL_STATE";
  doc["ack_req"] = ackReq;
  doc["stm_id"] = STM_ID;
  doc["gateway_id"] = GW_ID;
  doc["mode"] = modeToString(cfg.mode);
  doc["cfgVersion"] = cfg.cfgVersion;
  doc["time_valid"] = timeValid;
  doc["minute_of_day"] = minuteOfDayNow();
  doc["device_count"] = deviceCount;
  doc["uptime_ms"] = millis();

  JsonArray relays = doc.createNestedArray("relays");
  for (int i = 0; i < RELAY_COUNT; i++) {
    relays.add(cfg.relayState[i]);
  }

  JsonArray timers = doc.createNestedArray("timer_rules");

  for (int r = 0; r < RELAY_COUNT; r++) {
    JsonObject relayObj = timers.createNestedObject();
    relayObj["relay"] = r + 1;

    JsonArray schedules = relayObj.createNestedArray("schedules");

    for (int s = 0; s < MAX_SCHEDULES; s++) {
      char onBuf[8];
      char offBuf[8];

      formatHHMM(cfg.timerRules[r][s].onMin, onBuf, sizeof(onBuf));
      formatHHMM(cfg.timerRules[r][s].offMin, offBuf, sizeof(offBuf));

      JsonObject item = schedules.createNestedObject();
      item["index"] = s + 1;
      item["enable"] = cfg.timerRules[r][s].enable;
      item["on"] = onBuf;
      item["off"] = offBuf;
    }
  }

  JsonArray rules = doc.createNestedArray("sensor_rules");

  for (int r = 0; r < RELAY_COUNT; r++) {
    JsonObject item = rules.createNestedObject();

    item["relay"] = r + 1;
    item["enable"] = cfg.sensorRules[r].enable;
    item["id_device"] = cfg.sensorRules[r].idDevice;
    item["field"] = fieldToString(cfg.sensorRules[r].field);
    item["logic"] = logicToString(cfg.sensorRules[r].logic);
    item["onValue"] = cfg.sensorRules[r].onValue;
    item["offValue"] = cfg.sensorRules[r].offValue;
  }

  JsonArray devices = doc.createNestedArray("devices");

  for (int i = 0; i < deviceCount; i++) {
    JsonObject d = devices.createNestedObject();
    d["id"] = deviceList[i].id;
    d["name"] = deviceList[i].name;
  }

  // Fan status
  doc["fan"] = fanRunning ? 1 : 0;
  doc["fanMode"] = cfg.fanMode;
  doc["cabinetTemp"] = cabinetTemp;
  doc["fanOnTemp"] = cfg.fanOnTemp;
  doc["fanOffTemp"] = cfg.fanOffTemp;
  doc["emergency"] = emergencyActive ? 1 : 0;
}

void sendFullStateToESP(bool ackReq) {
  DynamicJsonDocument doc(16384);

  String msgId = makeMsgId("STM_FULL");

  buildFullState(doc, msgId, ackReq);

  if (ackReq) {
    addPendingAck(msgId);
  }

  sendJsonToESP(doc);
}

void sendFullStateToHMI() {
  DynamicJsonDocument doc(16384);

  String msgId = makeMsgId("STM_FULL_HMI");

  buildFullState(doc, msgId, false);

  sendJsonToHMI(doc);
}

// ======================================================
// SYNC REQUEST / RESPONSE
// ======================================================
void sendConfigSyncRequestToESP() {
  DynamicJsonDocument doc(1024);

  String msgId = makeMsgId("STM_SYNCREQ");

  doc["msg_id"] = msgId;
  doc["type"] = "CONFIG_SYNC_REQUEST";
  doc["cmd"] = "GET_CONFIG";
  doc["ack_req"] = true;
  doc["stm_id"] = STM_ID;
  doc["gateway_id"] = GW_ID;
  doc["localCfgVersion"] = cfg.cfgVersion;
  doc["mode"] = modeToString(cfg.mode);
  doc["timestamp_ms"] = millis();

  JsonArray need = doc.createNestedArray("need");
  need.add("mode");
  // Không need relay_state — relay state chỉ do STM32 kiểm soát, Server không sync xuống
  need.add("timer_rules");
  need.add("sensor_rules");
  need.add("device_list");
  need.add("fan_config");

  addPendingAck(msgId);

  sendJsonToESP(doc);
}

void applySyncResponse(JsonDocument& doc) {
  uint32_t serverVersion = doc["cfgVersion"] | 0;

  if (serverVersion == 0) {
    sendAckToESP(doc["msg_id"] | "", "ERR", "cfgVersion missing", "CFG_VERSION_MISSING");
    sendFullStateToESP(true);
    return;
  }

  if (serverVersion < cfg.cfgVersion) {
    sendAckToESP(doc["msg_id"] | "", "ERR", "server config older", "CFG_OLDER");
    sendFullStateToESP(true);
    return;
  }

  if (serverVersion == cfg.cfgVersion) {
    sendAckToESP(doc["msg_id"] | "", "OK", "CONFIG_ALREADY_LATEST");
    sendFullStateToESP(true);
    return;
  }

  const char* mode = doc["mode"] | nullptr;
  if (mode) {
    cfg.mode = parseMode(mode);
  }

  // KHÔNG apply relay state từ sync response.
  // Relay state chỉ do STM32 kiểm soát (HMI, timer, sensor, SET_RELAY cmd).
  // Server chỉ được đọc relay state, không được ghi đè.

  if (doc["timer_rules"].is<JsonArray>()) {
    JsonArray timers = doc["timer_rules"].as<JsonArray>();

    for (JsonObject relayObj : timers) {
      int relay = relayObj["relay"] | 0;
      int r = relayIndexFromNumber(relay);
      if (r < 0) continue;

      if (!relayObj["schedules"].is<JsonArray>()) continue;

      JsonArray schedules = relayObj["schedules"].as<JsonArray>();

      for (JsonObject item : schedules) {
        int index = item["index"] | 0;
        int s = index - 1;

        if (s < 0 || s >= MAX_SCHEDULES) continue;

        cfg.timerRules[r][s].enable = (int)(item["enable"] | 0) == 1 ? 1 : 0;
        cfg.timerRules[r][s].onMin = parseHHMM(item["on"] | "00:00");
        cfg.timerRules[r][s].offMin = parseHHMM(item["off"] | "00:00");
      }
    }
  }

  if (doc["sensor_rules"].is<JsonArray>()) {
    JsonArray rules = doc["sensor_rules"].as<JsonArray>();

    for (JsonObject item : rules) {
      int relay = item["relay"] | 0;
      int r = relayIndexFromNumber(relay);
      if (r < 0) continue;

      cfg.sensorRules[r].enable = (int)(item["enable"] | 0) == 1 ? 1 : 0;
      safeCopy(cfg.sensorRules[r].idDevice, sizeof(cfg.sensorRules[r].idDevice), item["id_device"] | "");
      cfg.sensorRules[r].field = parseField(item["field"] | "temperature");
      cfg.sensorRules[r].logic = parseLogic(item["logic"] | "ABOVE");
      cfg.sensorRules[r].onValue = item["onValue"] | item["onAbove"] | item["onBelow"] | 0.0;
      cfg.sensorRules[r].offValue = item["offValue"] | item["offBelow"] | item["offAbove"] | 0.0;
    }
  }

  // Fan config from sync
  if (doc["fanMode"].is<uint8_t>()) {
    cfg.fanMode = doc["fanMode"] | FAN_AUTO;
  }
  if (doc["fanOnTemp"].is<float>()) {
    cfg.fanOnTemp = doc["fanOnTemp"] | 35.0f;
  }
  if (doc["fanOffTemp"].is<float>()) {
    cfg.fanOffTemp = doc["fanOffTemp"] | 30.0f;
  }

  cfg.cfgVersion = serverVersion;

  applyRelayOutputsFromConfig();

  saveConfigToFlash(false);

  sendAckToESP(doc["msg_id"] | "", "OK", "CONFIG_SYNC_APPLIED");

  sendFullStateToESP(true);
  sendFullStateToHMI();
}

// ======================================================
// STATUS
// ======================================================
void sendStatusToESP(bool ackReq) {
  DynamicJsonDocument doc(1024);

  String msgId = makeMsgId("STM_STATUS");

  doc["msg_id"] = msgId;
  doc["type"] = "STATUS";
  doc["ack_req"] = ackReq;
  doc["stm_id"] = STM_ID;
  doc["gateway_id"] = GW_ID;
  doc["mode"] = modeToString(cfg.mode);
  doc["cfgVersion"] = cfg.cfgVersion;
  doc["device_count"] = deviceCount;
  doc["time_valid"] = timeValid;
  doc["minute_of_day"] = minuteOfDayNow();
  doc["uptime_ms"] = millis();

  // Relay state — luôn gửi trong status để Server biết trạng thái thật
  JsonArray relays = doc.createNestedArray("relays");

  for (int i = 0; i < RELAY_COUNT; i++) {
    relays.add(cfg.relayState[i]);
  }

  doc["fan"] = fanRunning ? 1 : 0;
  doc["fanMode"] = cfg.fanMode;
  doc["cabinetTemp"] = cabinetTemp;
  doc["fanOnTemp"] = cfg.fanOnTemp;
  doc["fanOffTemp"] = cfg.fanOffTemp;
  doc["emergency"] = emergencyActive ? 1 : 0;

  if (ackReq) {
    addPendingAck(msgId);
  }

  sendJsonToESP(doc);
}

void sendStatusToHMI() {
  DynamicJsonDocument doc(1024);

  doc["type"] = "STATUS";
  doc["stm_id"] = STM_ID;
  doc["gateway_id"] = GW_ID;
  doc["mode"] = modeToString(cfg.mode);
  doc["cfgVersion"] = cfg.cfgVersion;
  doc["device_count"] = deviceCount;
  doc["time_valid"] = timeValid;
  doc["minute_of_day"] = minuteOfDayNow();
  doc["uptime_ms"] = millis();

  JsonArray relays = doc.createNestedArray("relays");

  for (int i = 0; i < RELAY_COUNT; i++) {
    relays.add(cfg.relayState[i]);
  }

  doc["fan"] = fanRunning ? 1 : 0;
  doc["fanMode"] = cfg.fanMode;
  doc["cabinetTemp"] = cabinetTemp;
  doc["fanOnTemp"] = cfg.fanOnTemp;
  doc["fanOffTemp"] = cfg.fanOffTemp;
  doc["emergency"] = emergencyActive ? 1 : 0;

  sendJsonToHMI(doc);
}

// ======================================================
// DEVICE LIST / SENSOR DATA
// ======================================================
void handleDeviceList(JsonDocument& doc) {
  String msgId = doc["msg_id"] | "";

  if (!doc["devices"].is<JsonArray>()) {
    sendAckToESP(msgId, "ERR", "devices array missing", "DEVICE_LIST_INVALID");
    return;
  }

  JsonArray arr = doc["devices"].as<JsonArray>();
  deviceCount = 0;

  for (JsonObject d : arr) {
    if (deviceCount >= MAX_DEVICES) break;

    const char* id = d["id"] | d["id_device"] | "";
    const char* name = d["name"] | id;

    if (strlen(id) == 0) continue;

    safeCopy(deviceList[deviceCount].id, sizeof(deviceList[deviceCount].id), id);
    safeCopy(deviceList[deviceCount].name, sizeof(deviceList[deviceCount].name), name);

    safeCopy(sensorList[deviceCount].id, sizeof(sensorList[deviceCount].id), id);
    sensorList[deviceCount].temperature = 0;
    sensorList[deviceCount].humidity = 0;
    sensorList[deviceCount].co2 = 0;
    sensorList[deviceCount].light = 0;
    sensorList[deviceCount].lastMs = 0;
    sensorList[deviceCount].valid = false;

    deviceCount++;
  }

  sendAckToESP(msgId, "OK", "DEVICE_LIST_SAVED");
  sendDeviceListToHMI();
  sendFullStateToESP(true);
}

void handleSensorData(JsonDocument& doc) {
  const char* id = doc["id_device"] | "";

  if (strlen(id) == 0) return;

  int idx = findSensorIndex(id);

  if (idx < 0) {
    if (deviceCount >= MAX_DEVICES) return;

    idx = deviceCount;

    safeCopy(deviceList[idx].id, sizeof(deviceList[idx].id), id);
    safeCopy(deviceList[idx].name, sizeof(deviceList[idx].name), id);
    safeCopy(sensorList[idx].id, sizeof(sensorList[idx].id), id);

    deviceCount++;
    sendDeviceListToHMI();
  }

  sensorList[idx].temperature = doc["temperature"] | sensorList[idx].temperature;
  sensorList[idx].humidity    = doc["humidity"]    | sensorList[idx].humidity;
  sensorList[idx].co2         = doc["co2"]         | sensorList[idx].co2;
  sensorList[idx].light       = doc["light"]       | sensorList[idx].light;
  sensorList[idx].lastMs      = millis();
  sensorList[idx].valid       = true;

  DynamicJsonDocument hmi(512);

  hmi["type"] = "SENSOR";
  hmi["id_device"] = sensorList[idx].id;
  hmi["temperature"] = sensorList[idx].temperature;
  hmi["humidity"] = sensorList[idx].humidity;
  hmi["co2"] = sensorList[idx].co2;
  hmi["light"] = sensorList[idx].light;
  hmi["timestamp_ms"] = millis();

  sendJsonToHMI(hmi);
}

// ======================================================
// TIMER TASK
// ======================================================
bool scheduleActive(uint16_t nowMin, uint16_t onMin, uint16_t offMin) {
  if (onMin == offMin) return false;

  if (onMin < offMin) {
    return nowMin >= onMin && nowMin < offMin;
  }

  return nowMin >= onMin || nowMin < offMin;
}

void timerTask() {
  if (cfg.mode != MODE_TIMER) return;

  static uint32_t lastCheck = 0;

  if (millis() - lastCheck < 1000) return;
  lastCheck = millis();

  uint16_t nowMin = minuteOfDayNow();

  for (int r = 0; r < RELAY_COUNT; r++) {
    bool shouldOn = false;

    for (int s = 0; s < MAX_SCHEDULES; s++) {
      if (!cfg.timerRules[r][s].enable) continue;

      if (scheduleActive(nowMin, cfg.timerRules[r][s].onMin, cfg.timerRules[r][s].offMin)) {
        shouldOn = true;
        break;
      }
    }

    setRelay(r, shouldOn, "TIMER", true, false, false);
  }
}

// ======================================================
// SENSOR TASK
// ======================================================
void evaluateOneSensorRule(int r) {
  if (r < 0 || r >= RELAY_COUNT) return;
  if (!cfg.sensorRules[r].enable) return;

  int sidx = findSensorIndex(cfg.sensorRules[r].idDevice);
  if (sidx < 0) return;

  SensorData& sd = sensorList[sidx];

  if (!sd.valid) return;

  if (millis() - sd.lastMs > SENSOR_TIMEOUT_MS) return;

  float value = getSensorValue(sd, cfg.sensorRules[r].field);

  if (cfg.sensorRules[r].logic == LOGIC_ABOVE) {
    if (value >= cfg.sensorRules[r].onValue) {
      setRelay(r, true, "SENSOR", true, false, false);
    } else if (value <= cfg.sensorRules[r].offValue) {
      setRelay(r, false, "SENSOR", true, false, false);
    }
  } else {
    if (value <= cfg.sensorRules[r].onValue) {
      setRelay(r, true, "SENSOR", true, false, false);
    } else if (value >= cfg.sensorRules[r].offValue) {
      setRelay(r, false, "SENSOR", true, false, false);
    }
  }
}

void sensorTask() {
  if (cfg.mode != MODE_SENSOR) return;

  static uint32_t lastCheck = 0;

  if (millis() - lastCheck < 1000) return;
  lastCheck = millis();

  for (int r = 0; r < RELAY_COUNT; r++) {
    evaluateOneSensorRule(r);
  }
}

// ======================================================
// LM35 & FAN
// ======================================================
float readLM35() {
  int raw = analogRead(LM35_PIN);
  // LM35: 10mV/°C. STM32 ADC 12-bit = 0..4095, Vref = 3.3V
  // voltage = raw * 3.3 / 4095.0
  // temp = voltage / 0.01 = raw * 3.3 / 40.95
  float tempC = raw * 3.3f / 40.95f;
  return tempC;
}

void applyFan(bool on) {
  if (on == fanRunning) return;
  fanRunning = on;
  bool level = FAN_ACTIVE_HIGH ? on : !on;
  digitalWrite(FAN_PIN, level ? HIGH : LOW);
  DEBUG_SERIAL.print("[FAN] ");
  DEBUG_SERIAL.println(on ? "ON" : "OFF");
}

void evaluateFan() {
  // Fan mode priority: OFF (always off), ON (always on), AUTO (by threshold)
  if (cfg.fanMode == FAN_OFF) {
    applyFan(false);
    return;
  }
  if (cfg.fanMode == FAN_ON) {
    applyFan(true);
    return;
  }

  // AUTO mode — hysteresis
  if (!fanRunning && cabinetTemp >= cfg.fanOnTemp) {
    applyFan(true);
  } else if (fanRunning && cabinetTemp <= cfg.fanOffTemp) {
    applyFan(false);
  }
}

void sendFanStateToHMI() {
  DynamicJsonDocument doc(256);
  doc["type"] = "FAN_STATE";
  doc["fan"] = fanRunning ? 1 : 0;
  doc["fanMode"] = cfg.fanMode;
  doc["cabinetTemp"] = cabinetTemp;
  doc["fanOnTemp"] = cfg.fanOnTemp;
  doc["fanOffTemp"] = cfg.fanOffTemp;
  doc["timestamp_ms"] = millis();
  sendJsonToHMI(doc);
}

void sendFanStateToESP() {
  DynamicJsonDocument doc(384);
  String msgId = makeMsgId("STM_FAN");
  doc["msg_id"] = msgId;
  doc["type"] = "FAN_STATE";
  doc["stm_id"] = STM_ID;
  doc["gateway_id"] = GW_ID;
  doc["fan"] = fanRunning ? 1 : 0;
  doc["fanMode"] = cfg.fanMode;
  doc["cabinetTemp"] = cabinetTemp;
  doc["fanOnTemp"] = cfg.fanOnTemp;
  doc["fanOffTemp"] = cfg.fanOffTemp;
  doc["cfgVersion"] = cfg.cfgVersion;
  doc["timestamp_ms"] = millis();
  sendJsonToESP(doc);
}

void readEmergency() {
  static uint32_t lastEmergCheck = 0;
  static bool lastEmerg = false;

  if (millis() - lastEmergCheck < EMERGENCY_CHECK_MS) return;
  lastEmergCheck = millis();

  bool cur = (digitalRead(EMERGENCY_PIN) == EMERGENCY_ACTIVE);

  if (cur != lastEmerg) {
    lastEmerg = cur;
    emergencyActive = cur;

    if (cur) {
      DEBUG_SERIAL.println("[EMERGENCY] ACTIVATED — All relays OFF, Fan ON");
      // Tắt tất cả relay
      for (int i = 0; i < RELAY_COUNT; i++) {
        relayWriteRaw(i, false);
      }
      // Bật quạt
      if (!fanRunning) applyFan(true);
    } else {
      DEBUG_SERIAL.println("[EMERGENCY] CLEARED — Restoring relay states");
      // Khôi phục relay từ config
      applyRelayOutputsFromConfig();
      // Để evaluateFan tự quyết định chế độ quạt
      evaluateFan();
    }

    sendFanStateToHMI();
    sendFanStateToESP();
    sendStatusToESP(false);
    sendFullStateToESP(true);
  }
}

void lm35FanTask() {
  static uint32_t lastRead = 0;
  static uint32_t lastSend = 0;

  readEmergency();

  // Khi emergency, không chạy đọc LM35 + evaluateFan nữa
  if (emergencyActive) {
    if (millis() - lastSend >= 3000) {
      lastSend = millis();
      sendFanStateToHMI();
    }
    return;
  }

  if (millis() - lastRead >= LM35_READ_INTERVAL_MS) {
    lastRead = millis();
    cabinetTemp = readLM35();
    DEBUG_SERIAL.print("[LM35] Temp=");
    DEBUG_SERIAL.print(cabinetTemp);
    DEBUG_SERIAL.println(" C");
  }

  evaluateFan();

  // Send fan state to HMI every 3s
  if (millis() - lastSend >= 3000) {
    lastSend = millis();
    sendFanStateToHMI();
  }
}

// ======================================================
// CONFIG STATE
// ======================================================
void sendConfigStateToESP(const char* cmd, int relayIdx, bool ackReq) {
  DynamicJsonDocument doc(4096);

  String msgId = makeMsgId("STM_CFG");

  doc["msg_id"] = msgId;
  doc["type"] = "CONFIG_STATE";
  doc["cmd"] = cmd;
  doc["ack_req"] = ackReq;
  doc["stm_id"] = STM_ID;
  doc["gateway_id"] = GW_ID;
  doc["mode"] = modeToString(cfg.mode);
  doc["cfgVersion"] = cfg.cfgVersion;
  doc["timestamp_ms"] = millis();

  if (relayIdx >= 0) {
    doc["relay"] = relayIdx + 1;
  }

  if (strcmp(cmd, "SET_TIMER") == 0 && relayIdx >= 0) {
    JsonArray arr = doc.createNestedArray("schedules");

    for (int i = 0; i < MAX_SCHEDULES; i++) {
      char onBuf[8];
      char offBuf[8];

      formatHHMM(cfg.timerRules[relayIdx][i].onMin, onBuf, sizeof(onBuf));
      formatHHMM(cfg.timerRules[relayIdx][i].offMin, offBuf, sizeof(offBuf));

      JsonObject s = arr.createNestedObject();
      s["index"] = i + 1;
      s["enable"] = cfg.timerRules[relayIdx][i].enable;
      s["on"] = onBuf;
      s["off"] = offBuf;
    }
  }

  if (strcmp(cmd, "SET_SENSOR_RULE") == 0 && relayIdx >= 0) {
    SensorRule& r = cfg.sensorRules[relayIdx];

    doc["enable"] = r.enable;
    doc["id_device"] = r.idDevice;
    doc["field"] = fieldToString(r.field);
    doc["logic"] = logicToString(r.logic);
    doc["onValue"] = r.onValue;
    doc["offValue"] = r.offValue;
  }

  if (ackReq) {
    addPendingAck(msgId);
  }

  sendJsonToESP(doc);
}

// ======================================================
// COMMAND HANDLERS
// ======================================================
void ackBySource(const char* source, const String& msgId, const char* status, const char* message, const char* errorCode = "") {
  if (strcmp(source, "ESP") == 0) {
    sendAckToESP(msgId, status, message, errorCode);
  } else {
    sendAckToHMI(msgId, status, message, errorCode);
  }
}

void handleSetMode(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";
  JsonVariant data = doc["data"];

  const char* modeStr = nullptr;

  if (!data.isNull()) {
    modeStr = data["mode"] | nullptr;
  }

  if (!modeStr) {
    modeStr = doc["mode"] | nullptr;
  }

  if (!modeStr) {
    ackBySource(source, msgId, "ERR", "mode missing", "MODE_MISSING");
    return;
  }

  cfg.mode = parseMode(modeStr);

  saveConfigToFlash(true);

  ackBySource(source, msgId, "OK", "MODE_SET");

  sendStatusToHMI();
  sendConfigStateToESP("SET_MODE", -1, strcmp(source, "HMI") == 0);
  sendFullStateToESP(true);
}

void handleSetRelay(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";

  JsonVariant data = doc["data"];

  int relay = data["relay"] | doc["relay"] | 0;

  // Dùng .is<int>() thay vì | để tránh 0 bị coi là falsy
  int state = -1;
  if (data["state"].is<int>()) state = data["state"].as<int>();
  else if (doc["state"].is<int>()) state = doc["state"].as<int>();

  const char* modeStr = data["mode"] | doc["mode"] | nullptr;

  if (modeStr) {
    cfg.mode = parseMode(modeStr);
  }

  int idx = relayIndexFromNumber(relay);

  if (idx < 0) {
    ackBySource(source, msgId, "ERR", "relay invalid", "RELAY_INVALID");
    return;
  }

  if (state != 0 && state != 1) {
    ackBySource(source, msgId, "ERR", "state invalid", "STATE_INVALID");
    return;
  }

  if (cfg.mode != MODE_MANUAL) {
    ackBySource(source, msgId, "ERR", "not MANUAL mode", "MODE_NOT_MANUAL");
    return;
  }

  setRelay(idx, state == 1, source, true, strcmp(source, "HMI") == 0, true);

  ackBySource(source, msgId, "OK", state == 1 ? "RELAY_ON" : "RELAY_OFF");

  // Báo full_state lên Server sau khi relay thay đổi
  sendFullStateToESP(true);
}

void handleTestRelay(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";
  JsonVariant data = doc["data"];

  int relay = data["relay"] | doc["relay"] | 0;
  int idx = relayIndexFromNumber(relay);

  if (idx < 0) {
    ackBySource(source, msgId, "ERR", "relay invalid", "RELAY_INVALID");
    return;
  }

  ackBySource(source, msgId, "OK", "RELAY_TEST_START");
  relayPhysicalTest((uint8_t)relay);
  ackBySource(source, msgId, "OK", "RELAY_TEST_DONE");
}

void handleSetTimer(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";
  JsonVariant data = doc["data"];

  int relay = data["relay"] | doc["relay"] | 0;
  int idx = relayIndexFromNumber(relay);

  if (idx < 0) {
    ackBySource(source, msgId, "ERR", "relay invalid", "RELAY_INVALID");
    return;
  }

  JsonArray arr;

  if (data["schedules"].is<JsonArray>()) {
    arr = data["schedules"].as<JsonArray>();
  } else if (doc["schedules"].is<JsonArray>()) {
    arr = doc["schedules"].as<JsonArray>();
  } else {
    ackBySource(source, msgId, "ERR", "schedules missing", "SCHEDULE_MISSING");
    return;
  }

  for (int i = 0; i < MAX_SCHEDULES; i++) {
    cfg.timerRules[idx][i].enable = 0;
    cfg.timerRules[idx][i].onMin = 0;
    cfg.timerRules[idx][i].offMin = 0;
  }

  for (JsonObject s : arr) {
    int index = s["index"] | 0;
    int si = index - 1;

    if (si < 0 || si >= MAX_SCHEDULES) continue;

    cfg.timerRules[idx][si].enable = (int)(s["enable"] | 0) == 1 ? 1 : 0;
    cfg.timerRules[idx][si].onMin = parseHHMM(s["on"] | "00:00");
    cfg.timerRules[idx][si].offMin = parseHHMM(s["off"] | "00:00");
  }

  saveConfigToFlash(true);

  ackBySource(source, msgId, "OK", "TIMER_SAVED");

  sendConfigStateToESP("SET_TIMER", idx, strcmp(source, "HMI") == 0);
  sendFullStateToESP(true);
}

void handleSetSensorRule(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";
  JsonVariant data = doc["data"];

  int relay = data["relay"] | doc["relay"] | 0;
  int idx = relayIndexFromNumber(relay);

  if (idx < 0) {
    ackBySource(source, msgId, "ERR", "relay invalid", "RELAY_INVALID");
    return;
  }

  const char* idDevice = data["id_device"] | doc["id_device"] | "";
  const char* field = data["field"] | doc["field"] | "temperature";
  const char* logic = data["logic"] | doc["logic"] | "ABOVE";

  if (strlen(idDevice) == 0) {
    ackBySource(source, msgId, "ERR", "device missing", "DEVICE_MISSING");
    return;
  }

  if (findDeviceIndex(idDevice) < 0) {
    ackBySource(source, msgId, "ERR", "device not found", "DEVICE_NOT_FOUND");
    return;
  }

  SensorRule& r = cfg.sensorRules[idx];

  // enable: 0=off, 1=on — dùng .is<int>() để 0 không bị nuốt
  if (data["enable"].is<int>()) r.enable = data["enable"].as<int>() ? 1 : 0;
  else if (doc["enable"].is<int>()) r.enable = doc["enable"].as<int>() ? 1 : 0;
  else r.enable = 1; // default enabled

  safeCopy(r.idDevice, sizeof(r.idDevice), idDevice);
  r.field = parseField(field);
  r.logic = parseLogic(logic);

  if (r.logic == LOGIC_ABOVE) {
    r.onValue = data["onAbove"] | doc["onAbove"] | data["onValue"] | doc["onValue"] | 0.0;
    r.offValue = data["offBelow"] | doc["offBelow"] | data["offValue"] | doc["offValue"] | 0.0;
  } else {
    r.onValue = data["onBelow"] | doc["onBelow"] | data["onValue"] | doc["onValue"] | 0.0;
    r.offValue = data["offAbove"] | doc["offAbove"] | data["offValue"] | doc["offValue"] | 0.0;
  }

  saveConfigToFlash(true);

  ackBySource(source, msgId, "OK", "SENSOR_RULE_SAVED");

  sendConfigStateToESP("SET_SENSOR_RULE", idx, strcmp(source, "HMI") == 0);
  sendFullStateToESP(true);
}

void handleSetTime(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";
  JsonVariant data = doc["data"];

  if (!data.isNull() && data["epoch"].is<uint32_t>()) {
    uint32_t epoch = data["epoch"] | 0;
    setClockFromEpoch(epoch);
  } else if (doc["epoch"].is<uint32_t>()) {
    uint32_t epoch = doc["epoch"] | 0;
    setClockFromEpoch(epoch);
  } else {
    // Dùng .is<int>() thay vì | -1 để 0 (00:00) không bị nuốt
    int hh = -1; int mm = -1;
    if (data["hour"].is<int>()) hh = data["hour"].as<int>();
    else if (doc["hour"].is<int>()) hh = doc["hour"].as<int>();
    if (data["minute"].is<int>()) mm = data["minute"].as<int>();
    else if (doc["minute"].is<int>()) mm = doc["minute"].as<int>();

    if (hh < 0 || mm < 0) {
      ackBySource(source, msgId, "ERR", "time missing", "TIME_MISSING");
      return;
    }

    setClockFromHM(hh, mm);
  }

  ackBySource(source, msgId, "OK", "TIME_SET");

  sendStatusToHMI();
  sendStatusToESP(false);
}

void handleSetFan(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";
  JsonVariant data = doc["data"];

  // fanMode: 0=OFF, 1=AUTO, 2=ON
  // Dùng .is<int>() thay vì | để tránh 0 bị coi là falsy
  int fm = -1;
  if (data["fanMode"].is<int>()) fm = data["fanMode"].as<int>();
  else if (doc["fanMode"].is<int>()) fm = doc["fanMode"].as<int>();

  int fanCmd = -1;
  if (data["fan"].is<int>()) fanCmd = data["fan"].as<int>();
  else if (doc["fan"].is<int>()) fanCmd = doc["fan"].as<int>();

  // If fan field present, treat as direct ON/OFF override
  if (fanCmd == 0) {
    cfg.fanMode = FAN_OFF;
  } else if (fanCmd == 1) {
    cfg.fanMode = FAN_ON;
  } else if (fm >= FAN_OFF && fm <= FAN_ON) {
    cfg.fanMode = fm;
  } else {
    ackBySource(source, msgId, "ERR", "fanMode or fan missing", "FAN_MODE_MISSING");
    return;
  }

  saveConfigToFlash(true);

  // Immediately apply
  if (cfg.fanMode == FAN_OFF) applyFan(false);
  if (cfg.fanMode == FAN_ON) applyFan(true);

  ackBySource(source, msgId, "OK", "FAN_SET");

  sendFanStateToHMI();
  sendFanStateToESP();
  sendStatusToESP(strcmp(source, "HMI") == 0);
}

void handleSetFanThreshold(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";
  JsonVariant data = doc["data"];

  // Dùng .is<float>() cho an toàn với giá trị 0.0
  float onTemp = -999.0f; float offTemp = -999.0f;
  if (data["fanOnTemp"].is<float>()) onTemp = data["fanOnTemp"].as<float>();
  else if (doc["fanOnTemp"].is<float>()) onTemp = doc["fanOnTemp"].as<float>();
  if (data["fanOffTemp"].is<float>()) offTemp = data["fanOffTemp"].as<float>();
  else if (doc["fanOffTemp"].is<float>()) offTemp = doc["fanOffTemp"].as<float>();

  if (onTemp < -100.0f || offTemp < -100.0f) {
    ackBySource(source, msgId, "ERR", "threshold missing", "THRESHOLD_MISSING");
    return;
  }

  if (onTemp <= offTemp) {
    ackBySource(source, msgId, "ERR", "onTemp must be > offTemp", "THRESHOLD_INVALID");
    return;
  }

  cfg.fanOnTemp = onTemp;
  cfg.fanOffTemp = offTemp;

  saveConfigToFlash(true);

  ackBySource(source, msgId, "OK", "FAN_THRESHOLD_SAVED");

  sendFanStateToHMI();
  sendFanStateToESP();
  sendStatusToESP(strcmp(source, "HMI") == 0);
}

void handleSaveFlash(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";

  saveConfigToFlash(false);

  ackBySource(source, msgId, "OK", "FLASH_SAVED");

  sendFullStateToESP(true);
}

void handleGetFullState(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";

  ackBySource(source, msgId, "OK", "FULL_STATE_SENT");

  if (strcmp(source, "ESP") == 0) {
    sendFullStateToESP(true);
  }

  sendFullStateToHMI();
}

void handleCommand(JsonDocument& doc, const char* source) {
  const char* cmd = doc["cmd"] | "";

  if (strlen(cmd) == 0) {
    ackBySource(source, doc["msg_id"] | "", "ERR", "cmd missing", "CMD_MISSING");
    return;
  }

  if (strcmp(cmd, "SET_MODE") == 0) {
    handleSetMode(doc, source);
  }
  else if (strcmp(cmd, "SET_RELAY") == 0) {
    handleSetRelay(doc, source);
  }
  else if (strcmp(cmd, "TEST_RELAY") == 0) {
    handleTestRelay(doc, source);
  }
  else if (strcmp(cmd, "SET_TIMER") == 0) {
    handleSetTimer(doc, source);
  }
  else if (strcmp(cmd, "SET_SENSOR_RULE") == 0) {
    handleSetSensorRule(doc, source);
  }
  else if (strcmp(cmd, "SET_TIME") == 0) {
    handleSetTime(doc, source);
  }
  else if (strcmp(cmd, "SAVE_FLASH") == 0) {
    handleSaveFlash(doc, source);
  }
  else if (strcmp(cmd, "GET_FULL_STATE") == 0) {
    handleGetFullState(doc, source);
  }
  else if (strcmp(cmd, "GET_STATUS") == 0) {
    ackBySource(source, doc["msg_id"] | "", "OK", "STATUS_SENT");
    sendStatusToHMI();

    if (strcmp(source, "ESP") == 0) {
      sendStatusToESP(false);
    }
  }
  else if (strcmp(cmd, "GET_DEVICE_LIST") == 0) {
    ackBySource(source, doc["msg_id"] | "", "OK", "DEVICE_LIST_SENT");
    sendDeviceListToHMI();
  }
  else if (strcmp(cmd, "SET_FAN") == 0) {
    handleSetFan(doc, source);
  }
  else if (strcmp(cmd, "SET_FAN_THRESHOLD") == 0) {
    handleSetFanThreshold(doc, source);
  }
  else if (strcmp(cmd, "GET_FAN") == 0) {
    ackBySource(source, doc["msg_id"] | "", "OK", "FAN_STATE_SENT");
    sendFanStateToHMI();
    sendFanStateToESP();
  }
  else if (strcmp(cmd, "CONFIG_SYNC_REQUEST") == 0 || strcmp(cmd, "GET_CONFIG") == 0) {
    sendConfigSyncRequestToESP();
    ackBySource(source, doc["msg_id"] | "", "OK", "SYNC_REQUEST_SENT");
  }
  else {
    ackBySource(source, doc["msg_id"] | "", "ERR", "unknown cmd", "CMD_UNKNOWN");
  }
}

// ======================================================
// ACK FROM SERVER
// ======================================================
void handleAckFromESP(JsonDocument& doc) {
  String ackFor = doc["ack_for"] | "";

  if (ackFor.length()) {
    removePendingAck(ackFor);
  }

  DynamicJsonDocument hmi(512);

  hmi["type"] = "SERVER_ACK";
  hmi["ack_for"] = ackFor;
  hmi["status"] = doc["status"] | "";
  hmi["message"] = doc["message"] | "";
  hmi["error_code"] = doc["error_code"] | "";
  hmi["cfgVersion"] = cfg.cfgVersion;
  hmi["timestamp_ms"] = millis();

  sendJsonToHMI(hmi);
}

// ======================================================
// PROCESS ESP / HMI LINE
// ======================================================
void processESPLine(const String& line) {
  if (line.length() == 0) return;

  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println("[ESP32 -> STM32]");
  DEBUG_SERIAL.println(line);

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, line);

  if (err) {
    DEBUG_SERIAL.print("[ESP JSON ERR] ");
    DEBUG_SERIAL.println(err.c_str());
    return;
  }

  const char* type = doc["type"] | "";

  if (strcmp(type, "DEVICE_LIST") == 0) {
    handleDeviceList(doc);
  }
  else if (strcmp(type, "SENSOR") == 0) {
    handleSensorData(doc);
  }
  else if (strcmp(type, "CMD") == 0 || strcmp(type, "CONFIG") == 0 || doc["cmd"].is<const char*>()) {
    handleCommand(doc, "ESP");
  }
  else if (strcmp(type, "ACK") == 0) {
    handleAckFromESP(doc);
  }
  else if (strcmp(type, "CONFIG_SYNC_RESPONSE") == 0 || strcmp(type, "SYNC_RESPONSE") == 0) {
    applySyncResponse(doc);
  }
  else {
    sendAckToESP(doc["msg_id"] | "", "ERR", "unknown type", "TYPE_UNKNOWN");
  }
}

void processHMILine(const String& line) {
  if (line.length() == 0) return;

  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println("[HMI -> STM32]");
  DEBUG_SERIAL.println(line);

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, line);

  if (err) {
    DEBUG_SERIAL.print("[HMI JSON ERR] ");
    DEBUG_SERIAL.println(err.c_str());

    DynamicJsonDocument out(256);
    out["type"] = "ACK";
    out["status"] = "ERR";
    out["error_code"] = "JSON_INVALID";
    out["message"] = err.c_str();

    sendJsonToHMI(out);
    return;
  }

  const char* type = doc["type"] | "";

  if (strcmp(type, "CMD") == 0 || strcmp(type, "CONFIG") == 0 || doc["cmd"].is<const char*>()) {
    handleCommand(doc, "HMI");
  } else {
    sendAckToHMI(doc["msg_id"] | "", "ERR", "unknown hmi type", "TYPE_UNKNOWN");
  }
}

// ======================================================
// READ UART
// ======================================================
void readESPSerial() {
  while (ESP_SERIAL.available()) {
    char c = (char)ESP_SERIAL.read();

    if (c == '\r') continue;

    if (c == '\n') {
      espLine.trim();

      if (espLine.length()) {
        processESPLine(espLine);
      }

      espLine = "";
    } else {
      espLine += c;

      if (espLine.length() > 8192) {
        espLine = "";
      }
    }
  }
}

void readHMISerial() {
  while (HMI_SERIAL.available()) {
    char c = (char)HMI_SERIAL.read();

    if (c == '\r') continue;

    if (c == '\n') {
      hmiLine.trim();

      if (hmiLine.length()) {
        processHMILine(hmiLine);
      }

      hmiLine = "";
    } else {
      hmiLine += c;

      if (hmiLine.length() > 8192) {
        hmiLine = "";
      }
    }
  }
}

// ======================================================
// INIT RELAY
// ======================================================
void initFan() {
  pinMode(FAN_PIN, OUTPUT);
  pinMode(EMERGENCY_PIN, INPUT_PULLUP);  // nút nhấn nối GND khi nhấn
  // Start fan off, let evaluateFan() decide
  applyFan(false);
  fanModeRuntime = cfg.fanMode;
}

void initRelays() {
  for (int i = 0; i < RELAY_COUNT; i++) {
    pinMode(relayPins[i], OUTPUT);

    bool on = cfg.relayState[i] == 1;
    bool level = RELAY_ACTIVE_HIGH ? on : !on;

    digitalWrite(relayPins[i], level ? HIGH : LOW);
  }
}

// ======================================================
// PERIODIC TASK
// ======================================================
void periodicStatusTask() {
  static uint32_t last = 0;

  if (millis() - last >= 10000) {
    last = millis();
    sendStatusToESP(false);
  }
}

void bootSyncTask() {
  static bool done = false;
  static uint32_t bootMs = 0;

  if (done) return;

  if (bootMs == 0) {
    bootMs = millis();
  }

  if (millis() - bootMs >= 2000) {
    done = true;

    sendConfigSyncRequestToESP();
    sendFullStateToESP(true);
  }
}

// ======================================================
// SETUP / LOOP
// ======================================================
void setup() {
  DEBUG_SERIAL.begin(DEBUG_BAUD);
  delay(500);

  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println("=====================================");
  DEBUG_SERIAL.println("STM32 F103C8 FUVIAIR RELAY V2 START");
  DEBUG_SERIAL.println("=====================================");

  // EEPROM auto-init by STM32 core, no begin() needed
  // (uses internal flash emulation)

  bool ok = loadConfigFromFlash();

  if (!ok) {
    saveConfigToFlash(false);
  }

  ESP_SERIAL.begin(ESP_BAUD);
  HMI_SERIAL.begin(HMI_BAUD);

  clearPendingAcks();

  initRelays();
  initFan();

  // Read initial LM35 temp
  cabinetTemp = readLM35();
  DEBUG_SERIAL.print("[LM35] Initial temp=");
  DEBUG_SERIAL.println(cabinetTemp);

  sendStatusToHMI();
  sendStatusToESP(false);
}

void loop() {
  readESPSerial();
  readHMISerial();

  timerTask();
  sensorTask();
  lm35FanTask();

  checkPendingAckTimeouts();
  periodicStatusTask();
  bootSyncTask();
}