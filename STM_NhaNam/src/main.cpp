#include <Arduino.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

// ======================================================
// STM32 F103C8 - FUVIAIR RELAY CONTROLLER V8 CORRECT ARCH
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

// PA0 dùng cho còi báo lỗi khẩn cấp, không dùng làm relay.
// Relay được dời lên 1 chân; relay cuối dùng PB9.
const uint8_t relayPins[RELAY_COUNT] = {
  PA1, PA4, PA5, PA6, PA7, PA8, PB1, PB5, PB8, PB9
};

const bool RELAY_ACTIVE_HIGH = true;

#define ALLOW_ESP_RELAY_CONTROL 1
#define ENABLE_RELAY_STATE_SYNC_TO_ESP 1
#define ENABLE_CONFIG_SYNC_TO_ESP 1

// ======================================================
// LM35 + FAN + EMERGENCY CONFIG
// ======================================================
#define LM35_PIN           PB0
#define FAN_PIN            PB3
#define EMERGENCY_PIN      PB12
#define BUZZER_PIN         PA0
#define FAN_ACTIVE_HIGH    true
#define BUZZER_ACTIVE_HIGH true
#define EMERGENCY_ACTIVE   HIGH

#define FAN_OFF  0
#define FAN_AUTO 1
#define FAN_ON   2

#define LM35_READ_INTERVAL_MS  2000
#define EMERGENCY_CHECK_MS     500

// ======================================================
// SYSTEM CONFIG
// ======================================================
const char* STM_ID = "STM39";
const char* CONTROL_ID = "ESP39";
const char* GW_ID  = CONTROL_ID;
const char* FW_VERSION = "STM32_V10_HMI_MIN";

// Thông tin hiển thị trên pageINFO của Nextion
const char* HMI_DEVICE_NAME = "TRAM DIEU KHIEN";
const char* HMI_DEVICE_PASS = "393939";
const char* HMI_DISPLAY_VER = "0.0.001";
const char* HMI_DISPLAY_DATE = "2026";

#define MAX_DEVICES       20
#define MAX_SCHEDULES     10
#define MAX_PENDING_ACK   20
#define ACK_TIMEOUT_MS    5000
#define SENSOR_TIMEOUT_MS 30000

// ======================================================
// FLASH CONFIG
// ======================================================
#define CONFIG_MAGIC       0x46555633UL
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
  FIELD_TEMP      = 0,
  FIELD_HUMI      = 1,
  FIELD_CO2       = 2,
  FIELD_LIGHT     = 3,
  FIELD_VOLTAGE   = 4,
  FIELD_CURRENT   = 5,
  FIELD_FREQUENCY = 6,
  FIELD_POWER     = 7
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
  uint8_t fanMode;
  float fanOnTemp;
  float fanOffTemp;
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
  float voltage;
  float current;
  float frequency;
  float power;
  uint32_t lastMs;
  bool valid;
};

DeviceInfo deviceList[MAX_DEVICES];
SensorData sensorList[MAX_DEVICES];
uint8_t deviceCount = 0;

struct RelayRunInfo {
  char source[12];
  uint8_t scheduleIndex;
  char idDevice[20];
  uint8_t field;
  float value;
  float onValue;
  float offValue;
  uint32_t changedMs;
};

RelayRunInfo relayRun[RELAY_COUNT];

// ======================================================
// RUNTIME
// ======================================================
String espLine;
String hmiLine;
uint8_t hmiCurrentPage = 0;
uint32_t hmiRefreshMs = 0;
uint8_t hmiRefreshCount = 0;

bool timeValid = false;
uint32_t baseEpoch = 0;
uint32_t baseMillis = 0;

struct PendingAck {
  bool used;
  String msgId;
  uint32_t startMs;
};

PendingAck pendingAcks[MAX_PENDING_ACK];

float cabinetTemp = 0.0f;
bool fanRunning = false;
bool buzzerRunning = false;
uint8_t fanModeRuntime = FAN_AUTO;
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
  if (strcmp(s, "TIMER") == 0  || strcmp(s, "timer") == 0)  return MODE_TIMER;
  if (strcmp(s, "SENSOR") == 0 || strcmp(s, "sensor") == 0) return MODE_SENSOR;
  return cfg.mode;
}

SensorField parseField(const char* s) {
  if (!s) return FIELD_TEMP;
  if (strcmp(s, "temperature") == 0 || strcmp(s, "temp") == 0 || strcmp(s, "t") == 0) return FIELD_TEMP;
  if (strcmp(s, "humidity") == 0    || strcmp(s, "humi") == 0 || strcmp(s, "h") == 0) return FIELD_HUMI;
  if (strcmp(s, "co2") == 0         || strcmp(s, "CO2") == 0)                          return FIELD_CO2;
  if (strcmp(s, "light") == 0       || strcmp(s, "lux") == 0)                          return FIELD_LIGHT;
  if (strcmp(s, "voltage") == 0     || strcmp(s, "volt") == 0 || strcmp(s, "v") == 0)  return FIELD_VOLTAGE;
  if (strcmp(s, "current") == 0     || strcmp(s, "amp") == 0  || strcmp(s, "a") == 0)  return FIELD_CURRENT;
  if (strcmp(s, "frequency") == 0   || strcmp(s, "freq") == 0 || strcmp(s, "f") == 0)  return FIELD_FREQUENCY;
  if (strcmp(s, "power") == 0       || strcmp(s, "watt") == 0 || strcmp(s, "p") == 0)  return FIELD_POWER;
  return FIELD_TEMP;
}

const char* fieldToString(uint8_t f) {
  switch (f) {
    case FIELD_TEMP:      return "temperature";
    case FIELD_HUMI:      return "humidity";
    case FIELD_CO2:       return "co2";
    case FIELD_LIGHT:     return "light";
    case FIELD_VOLTAGE:   return "voltage";
    case FIELD_CURRENT:   return "current";
    case FIELD_FREQUENCY: return "frequency";
    case FIELD_POWER:     return "power";
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

void markRelayRunInfo(int idx, const char* source, uint8_t scheduleIndex = 0, const char* idDevice = "", uint8_t field = FIELD_TEMP, float value = 0, float onValue = 0, float offValue = 0) {
  if (idx < 0 || idx >= RELAY_COUNT) return;
  safeCopy(relayRun[idx].source, sizeof(relayRun[idx].source), source);
  relayRun[idx].scheduleIndex = scheduleIndex;
  safeCopy(relayRun[idx].idDevice, sizeof(relayRun[idx].idDevice), idDevice);
  relayRun[idx].field = field;
  relayRun[idx].value = value;
  relayRun[idx].onValue = onValue;
  relayRun[idx].offValue = offValue;
  relayRun[idx].changedMs = millis();
}

void initRelayRunInfo() {
  for (int i = 0; i < RELAY_COUNT; i++) markRelayRunInfo(i, "SYSTEM");
}

int relayIndexFromNumber(int relay) {
  if (relay < 1 || relay > RELAY_COUNT) return -1;
  return relay - 1;
}

uint16_t parseHHMM(const char* t) {
  if (!t || strlen(t) < 4) return 0;
  int hh = 0, mm = 0;
  sscanf(t, "%d:%d", &hh, &mm);
  if (hh < 0) hh = 0; if (hh > 23) hh = 23;
  if (mm < 0) mm = 0; if (mm > 59) mm = 59;
  return (uint16_t)(hh * 60 + mm);
}

void formatHHMM(uint16_t minOfDay, char* out, size_t len) {
  snprintf(out, len, "%02u:%02u", minOfDay / 60, minOfDay % 60);
}

// ======================================================
// CHECKSUM
// ======================================================
uint32_t calcChecksum(const SavedConfig& c) {
  const uint8_t* p = (const uint8_t*)&c;
  uint32_t sum = 0;
  size_t len = sizeof(SavedConfig) - sizeof(uint32_t);
  for (size_t i = 0; i < len; i++) sum = (sum * 31) + p[i];
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
  for (int i = 0; i < RELAY_COUNT; i++) cfg.relayState[i] = 0;
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
  cfg.fanMode = FAN_AUTO;
  cfg.fanOnTemp = 35.0f;
  cfg.fanOffTemp = 30.0f;
  cfg.checksum = calcChecksum(cfg);
}

bool loadConfigFromFlash() {
  EEPROM.get(CONFIG_ADDR, cfg);
  if (cfg.magic != CONFIG_MAGIC) {
    DEBUG_SERIAL.println(F("[FLASH] Invalid magic"));
    defaultConfig();
    return false;
  }
  uint32_t chk = calcChecksum(cfg);
  if (chk != cfg.checksum) {
    DEBUG_SERIAL.println(F("[FLASH] Invalid checksum"));
    defaultConfig();
    return false;
  }
  if (cfg.mode > MODE_SENSOR) cfg.mode = MODE_MANUAL;
  DEBUG_SERIAL.print(F("[FLASH] Loaded cfgVersion="));
  DEBUG_SERIAL.println(cfg.cfgVersion);
  return true;
}

void saveConfigToFlash(bool increaseVersion) {
  if (increaseVersion) cfg.cfgVersion++;
  cfg.magic = CONFIG_MAGIC;
  cfg.checksum = calcChecksum(cfg);
  EEPROM.put(CONFIG_ADDR, cfg);
  DEBUG_SERIAL.print(F("[FLASH] Saved cfgVersion="));
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
  return (nowEpoch() % 86400UL) / 60;
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
// FORWARD DECLARATIONS
// ======================================================
void sendConfigStateToESP(const char* cmd, int relayIdx, bool ackReq, int scheduleIdx = -1);
void sendStatusToESP(bool ackReq);
void sendStatusToHMI();
void sendRelayStateToESP(int idx, const char* source, bool ackReq);
void setRelay(int idx, bool on, const char* source, bool syncServer, bool serverAckReq, bool saveFlashNow);
void saveConfigToFlash(bool bumpVersion);
void ackBySource(const char* source, const String& msgId, const char* status, const char* message, const char* errorCode = "");
void rejectEmergencyCommand(const char* source, const String& msgId);
bool isEmergencyReadOnlyCmd(const char* cmd);

// ======================================================
// JSON SEND
// ======================================================
void sendJsonToESP(JsonDocument& doc) {
  size_t n = measureJson(doc);
  DEBUG_SERIAL.println();
  DEBUG_SERIAL.print(F("[STM32->ESP32] bytes="));
  DEBUG_SERIAL.println(n);
  serializeJson(doc, DEBUG_SERIAL);
  DEBUG_SERIAL.println();
  serializeJson(doc, ESP_SERIAL);
  ESP_SERIAL.print('\n');
}

void sendJsonToHMI(JsonDocument& doc) {
  (void)doc;
}

// ======================================================
// HMI / NEXTION MINIMAL
// ======================================================
void hmiEnd() {
  HMI_SERIAL.write(0xFF);
  HMI_SERIAL.write(0xFF);
  HMI_SERIAL.write(0xFF);
}

void hmiSetTextC(const char* obj, const char* txt) {
  if (!obj || !txt) return;
  HMI_SERIAL.print(obj);
  HMI_SERIAL.print(F(".txt=\""));
  HMI_SERIAL.print(txt);
  HMI_SERIAL.print("\"");
  hmiEnd();
}

void hmiSetTextF(const char* obj, float value, uint8_t digits) {
  char buf[18];
  if (isnan(value) || isinf(value)) strcpy(buf, "--");
  else dtostrf(value, 0, digits, buf);
  hmiSetTextC(obj, buf);
}

void hmiSetBco(const char* obj, uint16_t color565) {
  if (!obj) return;
  HMI_SERIAL.print(obj);
  HMI_SERIAL.print(F(".bco="));
  HMI_SERIAL.print(color565);
  hmiEnd();
}

void hmiUpdateRelayOne(int idx) {
  if (idx < 0 || idx >= RELAY_COUNT) return;

  // pageHOME:
  // K01..K10 là nhãn cố định, STM chỉ cập nhật ô trạng thái bên cạnh.
  // Mapping theo file HMI bạn gửi:
  // K01 t8/t10, K02 t13/t15, K03 t16/t17, K04 t18/t19, K05 t24/t25
  // K06 t9/t11, K07 t12/t14, K08 t23/t22, K09 t21/t20, K10 t31/t30
  static const char* ovLabel[RELAY_COUNT]  = {"t8","t13","t16","t18","t24","t9","t12","t23","t21","t31"};
  static const char* ovStatus[RELAY_COUNT] = {"t10","t15","t17","t19","t25","t11","t14","t22","t20","t30"};

  // pageMANU: nút b0..b9
  static const char* mb[RELAY_COUNT] = {"b0","b1","b2","b3","b4","b5","b6","b7","b8","b9"};

  char kLabel[6];
  snprintf(kLabel, sizeof(kLabel), "K%02d", idx + 1);
  const char* stateText = cfg.relayState[idx] ? "ON" : "OFF";

  if (hmiCurrentPage == 1) {
    hmiSetTextC(ovLabel[idx], kLabel);
    hmiSetTextC(ovStatus[idx], stateText);
  } else if (hmiCurrentPage == 2) {
    char btnText[18];
    snprintf(btnText, sizeof(btnText), "K%02d %s", idx + 1, stateText);
    hmiSetTextC(mb[idx], btnText);
    hmiSetBco(mb[idx], cfg.relayState[idx] ? 2016 : 48631);
  }
}

void hmiUpdateRelays() {
  for (int i = 0; i < RELAY_COUNT; i++) hmiUpdateRelayOne(i);
}

void hmiUpdateInfoPage() {
  if (hmiCurrentPage != 0) return;

  // pageINFO object name theo file HMI:
  // t_thietbi, t_mathietbi, t_matkhau, t_sokenh, t_ver, t_dated
  hmiSetTextC("t_thietbi", HMI_DEVICE_NAME);     // Tên thiết bị
  hmiSetTextC("t_mathietbi", CONTROL_ID);        // Mã thiết bị / ID tủ
  hmiSetTextC("t_matkhau", HMI_DEVICE_PASS);     // Mật khẩu thiết bị
  hmiSetTextC("t_sokenh", "10");                // Số kênh tải
  hmiSetTextC("t_ver", HMI_DISPLAY_VER);         // Phiên bản
  hmiSetTextC("t_dated", HMI_DISPLAY_DATE);      // Ngày cập nhật

  // Giữ thêm vài tên object cũ để không lỗi nếu HMI còn object cũ.
  hmiSetTextC("t_sodieuk", CONTROL_ID);
  hmiSetTextC("t_kenhtai", "10");
  hmiSetTextC("t_date", HMI_DISPLAY_DATE);
  hmiSetTextC("t_donvi", "Fuvitech");
  hmiSetTextC("t6", "Fuvitech");
}

void hmiUpdateSensorValues(int idx) {
  if (hmiCurrentPage != 1) return;
  if (idx < 0 || idx >= MAX_DEVICES || !sensorList[idx].valid) return;
  SensorData& sd = sensorList[idx];
  hmiSetTextF("t0", sd.temperature, 1);
  hmiSetTextF("t1", sd.humidity, 1);
  hmiSetTextF("t2", sd.co2, 0);
  hmiSetTextF("t3", sd.light, 0);
  hmiSetTextF("t4", sd.voltage, 1);
  hmiSetTextF("t5", sd.current, 2);
  hmiSetTextF("t6", sd.power, 1);
  hmiSetTextF("t7", sd.frequency, 0);
}

void hmiUpdateFirstSensor() {
  if (hmiCurrentPage != 1) return;
  for (int i = 0; i < deviceCount; i++) {
    if (sensorList[i].valid) { hmiUpdateSensorValues(i); return; }
  }
  // Chưa có dữ liệu cảm biến thì vẫn ghi dấu -- để biết STM đã cập nhật được HMI.
  hmiSetTextC("t0", "--");
  hmiSetTextC("t1", "--");
  hmiSetTextC("t2", "--");
  hmiSetTextC("t3", "--");
  hmiSetTextC("t4", "--");
  hmiSetTextC("t5", "--");
  hmiSetTextC("t6", "--");
  hmiSetTextC("t7", "--");
}

void hmiUpdateAll() {
  hmiUpdateInfoPage();
  hmiUpdateFirstSensor();
  hmiUpdateRelays();
}

void hmiRequestRefresh(uint8_t page) {
  hmiCurrentPage = page;
  hmiUpdateAll();
  // Nextion đôi khi gửi PG ngay lúc vừa chuyển trang, nên STM lặp lại cập nhật
  // vài lần ngắn để tránh mất lệnh khi component chưa kịp sẵn sàng.
  hmiRefreshMs = millis();
  hmiRefreshCount = 5;
}

void hmiRefreshTask() {
  if (hmiRefreshCount == 0) return;
  if (millis() - hmiRefreshMs < 200) return;
  hmiRefreshMs = millis();
  hmiRefreshCount--;
  hmiUpdateAll();
}

void hmiSetModeDirect(uint8_t newMode) {
  uint8_t oldMode = cfg.mode;
  cfg.mode = newMode;
  if (oldMode != newMode) {
    saveConfigToFlash(true);
    if (ENABLE_CONFIG_SYNC_TO_ESP) sendConfigStateToESP("SET_MODE", -1, false);
  }
  sendStatusToESP(false);
  sendStatusToHMI();
}

void hmiToggleRelay(int relay) {
  int idx = relayIndexFromNumber(relay);
  if (idx < 0) return;
  cfg.mode = MODE_MANUAL;
  markRelayRunInfo(idx, "MANUAL");
  bool nextState = !cfg.relayState[idx];
  setRelay(idx, nextState, "HMI", true, false, false);
  sendRelayStateToESP(idx, "HMI", false);
  sendStatusToESP(false);
  sendStatusToHMI();
}

void handleHmiAsciiCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  cmd.toUpperCase();

  if (emergencyActive) {
    bool readOnly = cmd.startsWith("PG:") ||
                    cmd.startsWith("PAGE:") ||
                    cmd.startsWith("<REQ|") ||
                    cmd == "REQ:STATUS" ||
                    cmd == "STATUS" ||
                    cmd == "REQ:INFO";
    if (!readOnly) {
      DEBUG_SERIAL.println(F("[EMERGENCY] HMI command blocked"));
      sendStatusToHMI();
      return;
    }
  }

  // Hỗ trợ cả lệnh mới và lệnh cũ từ file HMI:
  // PG:OV / PG:MANUAL / PG:TIMER / PG:SENSOR
  // <REQ|HOME> / <REQ|MANU> / <REQ|TIMER> / <REQ|SENS> / <REQ|INF>
  if (cmd == "PG:INFO" || cmd == "PAGE:INFO" || cmd == "<REQ|INF>" || cmd == "<REQ|INFO>") {
    hmiRequestRefresh(0);
    return;
  }
  if (cmd == "<REQ|HOME>") { cmd = "PG:OV"; }
  if (cmd == "<REQ|MANU>" || cmd == "<REQ|MANUAL>") { cmd = "PG:MANUAL"; }
  if (cmd == "<REQ|SENS>" || cmd == "<REQ|SENSOR>") { cmd = "PG:SENSOR"; }
  if (cmd == "<REQ|TIME>" || cmd == "<REQ|TIMER>") { cmd = "PG:TIMER"; }

  if (cmd == "PG:OV"     || cmd == "PAGE:OV")     { hmiRequestRefresh(1); return; }
  if (cmd == "PG:MANUAL" || cmd == "PAGE:MANUAL") { hmiRequestRefresh(2); return; }
  if (cmd == "PG:SENSOR")                          { hmiRequestRefresh(3); return; }
  if (cmd == "PG:TIMER")                           { hmiRequestRefresh(4); return; }
  if (cmd == "REQ:STATUS" || cmd == "STATUS" || cmd == "REQ:INFO") { sendStatusToHMI(); return; }
  if (cmd == "MODE:MANUAL" || cmd == "MANUAL")     { hmiSetModeDirect(MODE_MANUAL); return; }
  if (cmd == "MODE:TIMER"  || cmd == "TIMER")      { hmiSetModeDirect(MODE_TIMER);  return; }
  if (cmd == "MODE:SENSOR" || cmd == "SENSOR")     { hmiSetModeDirect(MODE_SENSOR); return; }

  if (cmd.startsWith("B") && cmd.length() <= 3) {
    int b = cmd.substring(1).toInt();
    if (b >= 0 && b < RELAY_COUNT) { hmiCurrentPage = 2; hmiToggleRelay(b + 1); }
    return;
  }
  if ((cmd.startsWith("K") || cmd.startsWith("R")) && cmd.indexOf(':') < 0) {
    int relay = cmd.substring(1).toInt();
    if (relay >= 1 && relay <= RELAY_COUNT) { hmiCurrentPage = 2; hmiToggleRelay(relay); }
    return;
  }
}

// ======================================================
// ACK
// ======================================================
void sendAckToESP(const String& ackFor, const char* status, const char* message, const char* errorCode = "") {
  JsonDocument doc;
  doc["type"]       = "ACK";
  doc["ack_for"]    = ackFor;
  doc["stm_id"]     = STM_ID;
  doc["gateway_id"] = GW_ID;
  doc["control_id"] = CONTROL_ID;
  doc["device_id"]  = CONTROL_ID;
  doc["status"]     = status;
  doc["message"]    = message;
  if (strlen(errorCode) > 0) doc["error_code"] = errorCode;
  doc["cfgVersion"]    = cfg.cfgVersion;
  doc["timestamp_ms"]  = millis();
  sendJsonToESP(doc);
}

void sendAckToHMI(const String& ackFor, const char* status, const char* message, const char* errorCode = "") {
  (void)ackFor;
  (void)status;
  (void)message;
  (void)errorCode;
  // Nextion đang dùng lệnh trực tiếp .txt/.bco, không gửi JSON dài để tiết kiệm Flash/RAM.
  hmiUpdateAll();
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
      DEBUG_SERIAL.print(F("[PENDING ADD] "));
      DEBUG_SERIAL.println(msgId);
      return true;
    }
  }
  DEBUG_SERIAL.println(F("[PENDING FULL]"));
  return false;
}

bool removePendingAck(const String& msgId) {
  for (int i = 0; i < MAX_PENDING_ACK; i++) {
    if (pendingAcks[i].used && pendingAcks[i].msgId == msgId) {
      pendingAcks[i].used = false;
      DEBUG_SERIAL.print(F("[PENDING REMOVE] "));
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
      pendingAcks[i].used = false;
      hmiUpdateAll();
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
  // Chỉ cập nhật trực tiếp nút/text trên Nextion, không tạo JSON.
  hmiUpdateRelayOne(idx);
}

void sendRelayStateToESP(int idx, const char* source, bool ackReq) {
  if (!ENABLE_RELAY_STATE_SYNC_TO_ESP) return;
  JsonDocument doc;
  String msgId = makeMsgId("STM_RELAY");
  doc["msg_id"]         = msgId;
  doc["type"]           = "RELAY_STATE";
  doc["ack_req"]        = ackReq;
  doc["stm_id"]         = STM_ID;
  doc["gateway_id"]     = GW_ID;
  doc["control_id"]     = CONTROL_ID;
  doc["device_id"]      = CONTROL_ID;
  doc["fw"]             = FW_VERSION;
  doc["mode"]           = modeToString(cfg.mode);
  doc["relay"]          = idx + 1;
  doc["state"]          = cfg.relayState[idx];
  doc["source"]         = relayRun[idx].source[0] ? relayRun[idx].source : source;
  doc["cmd_source"]     = source;
  doc["schedule_index"] = relayRun[idx].scheduleIndex;
  if (strlen(relayRun[idx].idDevice) > 0) doc["id_device"] = relayRun[idx].idDevice;
  doc["field"]          = fieldToString(relayRun[idx].field);
  doc["value"]          = relayRun[idx].value;
  doc["onValue"]        = relayRun[idx].onValue;
  doc["offValue"]       = relayRun[idx].offValue;
  doc["changed_ms"]     = relayRun[idx].changedMs;
  doc["cfgVersion"]     = cfg.cfgVersion;
  doc["timestamp_ms"]   = millis();
  if (ackReq) addPendingAck(msgId);
  sendJsonToESP(doc);
}

void setRelay(int idx, bool on, const char* source, bool syncServer, bool serverAckReq, bool saveFlashNow) {
  if (idx < 0 || idx >= RELAY_COUNT) return;
  if (emergencyActive && on) {
    DEBUG_SERIAL.println(F("[EMERGENCY] Relay ON blocked"));
    return;
  }
  bool old = cfg.relayState[idx] ? true : false;
  relayWriteRaw(idx, on);
  DEBUG_SERIAL.print(F("[RELAY] R"));
  DEBUG_SERIAL.print(idx + 1);
  DEBUG_SERIAL.print(F(" old="));
  DEBUG_SERIAL.print(old ? 1 : 0);
  DEBUG_SERIAL.print(F(" new="));
  DEBUG_SERIAL.print(on ? 1 : 0);
  DEBUG_SERIAL.print(F(" pin="));
  DEBUG_SERIAL.print(relayPins[idx]);
  DEBUG_SERIAL.print(F(" gpioLevel="));
  DEBUG_SERIAL.println(digitalRead(relayPins[idx]) ? 1 : 0);
  if (old != on && saveFlashNow) saveConfigToFlash(true);
  bool notifyHMI = (old != on) || (strcmp(source, "HMI") == 0) || (strcmp(source, "TEST") == 0);
  if (notifyHMI) sendRelayStateToHMI(idx);
  if (old != on && syncServer && ENABLE_RELAY_STATE_SYNC_TO_ESP) {
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
  if (idx < 0) { DEBUG_SERIAL.println(F("[TEST_RELAY] relay invalid")); return; }
  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println(F("========== RELAY PHYSICAL TEST =========="));
  DEBUG_SERIAL.print(F("Relay: ")); DEBUG_SERIAL.println(relayNumber);
  DEBUG_SERIAL.print(F("Pin: "));   DEBUG_SERIAL.println(relayPins[idx]);
  for (int i = 0; i < 5; i++) {
    relayWriteRaw(idx, true);  sendRelayStateToHMI(idx); delay(400);
    relayWriteRaw(idx, false); sendRelayStateToHMI(idx); delay(400);
  }
  DEBUG_SERIAL.println(F("========== RELAY TEST DONE =========="));
}

// ======================================================
// DEVICE / SENSOR
// ======================================================
int findSensorIndex(const char* id) {
  if (!id) return -1;
  for (uint8_t i = 0; i < deviceCount; i++) {
    if (strcmp(sensorList[i].id, id) == 0) return i;
  }
  return -1;
}

int ensureDeviceSlot(const char* id, const char* name = nullptr) {
  if (!id || strlen(id) == 0) return -1;
  int idx = findSensorIndex(id);
  if (idx >= 0) return idx;
  if (deviceCount >= MAX_DEVICES) return -1;
  idx = deviceCount;
  const char* displayName = (name && strlen(name) > 0) ? name : id;
  safeCopy(deviceList[idx].id,   sizeof(deviceList[idx].id),   id);
  safeCopy(deviceList[idx].name, sizeof(deviceList[idx].name), displayName);
  memset(&sensorList[idx], 0, sizeof(sensorList[idx]));
  safeCopy(sensorList[idx].id, sizeof(sensorList[idx].id), id);
  sensorList[idx].valid = false;
  sensorList[idx].lastMs = 0;
  deviceCount++;
  DEBUG_SERIAL.print(F("[DEVICE AUTO ADD] "));
  DEBUG_SERIAL.println(id);
  return idx;
}

void sendDeviceListToHMI() {
  // Bản HMI tối giản chưa hiển thị danh sách thiết bị dạng động.
  // Web UI/ESP vẫn quản lý danh sách thiết bị.
}

float getSensorValue(const SensorData& s, uint8_t f) {
  switch (f) {
    case FIELD_TEMP:      return s.temperature;
    case FIELD_HUMI:      return s.humidity;
    case FIELD_CO2:       return s.co2;
    case FIELD_LIGHT:     return s.light;
    case FIELD_VOLTAGE:   return s.voltage;
    case FIELD_CURRENT:   return s.current;
    case FIELD_FREQUENCY: return s.frequency;
    case FIELD_POWER:     return s.power;
    default: return 0;
  }
}

// ======================================================
// RUNTIME STATE DETAIL
// ======================================================
void appendRelayRuntimeMeta(JsonDocument& doc) {
  JsonArray active = doc["active_relays"].to<JsonArray>();
  for (int i = 0; i < RELAY_COUNT; i++) {
    if (!cfg.relayState[i]) continue;
    JsonObject a = active.add<JsonObject>();
    a["relay"]          = i + 1;
    a["state"]          = 1;
    a["source"]         = relayRun[i].source[0] ? relayRun[i].source : "SYSTEM";
    a["schedule_index"] = relayRun[i].scheduleIndex;
    if (strlen(relayRun[i].idDevice) > 0) a["id_device"] = relayRun[i].idDevice;
    if (strcmp(relayRun[i].source, "SENSOR") == 0) {
      a["field"] = fieldToString(relayRun[i].field);
      a["value"] = relayRun[i].value;
    }
  }
}

// ======================================================
// FULL STATE
// ======================================================
void buildFullState(JsonDocument& doc, const String& msgId, bool ackReq, bool includeDetails) {
  doc["msg_id"]       = msgId;
  doc["type"]         = "FULL_STATE";
  doc["ack_req"]      = ackReq;
  doc["stm_id"]       = STM_ID;
  doc["gateway_id"]   = GW_ID;
  doc["control_id"]   = CONTROL_ID;
  doc["device_id"]    = CONTROL_ID;
  doc["mode"]         = modeToString(cfg.mode);
  doc["cfgVersion"]   = cfg.cfgVersion;
  doc["time_valid"]   = timeValid;
  doc["minute_of_day"]= minuteOfDayNow();
  doc["device_count"] = deviceCount;
  doc["uptime_ms"]    = millis();

  JsonArray relays = doc["relays"].to<JsonArray>();
  for (int i = 0; i < RELAY_COUNT; i++) relays.add(cfg.relayState[i]);
  appendRelayRuntimeMeta(doc);

  if (includeDetails) {
    JsonArray timers = doc["timer_rules"].to<JsonArray>();
    for (int r = 0; r < RELAY_COUNT; r++) {
      JsonObject relayObj = timers.add<JsonObject>();
      relayObj["relay"] = r + 1;
      JsonArray schedules = relayObj["schedules"].to<JsonArray>();
      for (int s = 0; s < MAX_SCHEDULES; s++) {
        char onBuf[8], offBuf[8];
        formatHHMM(cfg.timerRules[r][s].onMin,  onBuf,  sizeof(onBuf));
        formatHHMM(cfg.timerRules[r][s].offMin, offBuf, sizeof(offBuf));
        JsonObject item = schedules.add<JsonObject>();
        item["index"]  = s + 1;
        item["enable"] = cfg.timerRules[r][s].enable;
        item["on"]     = onBuf;
        item["off"]    = offBuf;
      }
    }
    JsonArray rules = doc["sensor_rules"].to<JsonArray>();
    for (int r = 0; r < RELAY_COUNT; r++) {
      JsonObject item = rules.add<JsonObject>();
      item["relay"]     = r + 1;
      item["enable"]    = cfg.sensorRules[r].enable;
      item["id_device"] = cfg.sensorRules[r].idDevice;
      item["field"]     = fieldToString(cfg.sensorRules[r].field);
      item["logic"]     = logicToString(cfg.sensorRules[r].logic);
      item["onValue"]   = cfg.sensorRules[r].onValue;
      item["offValue"]  = cfg.sensorRules[r].offValue;
    }
    JsonArray devices = doc["devices"].to<JsonArray>();
    for (int i = 0; i < deviceCount; i++) {
      JsonObject d = devices.add<JsonObject>();
      d["id"]   = deviceList[i].id;
      d["name"] = deviceList[i].name;
    }
  }

  doc["fan"]        = fanRunning ? 1 : 0;
  doc["fanMode"]    = cfg.fanMode;
  doc["cabinetTemp"]= cabinetTemp;
  doc["fanOnTemp"]  = cfg.fanOnTemp;
  doc["fanOffTemp"] = cfg.fanOffTemp;
  doc["emergency"]  = emergencyActive ? 1 : 0;
}

void sendFullStateToESP(bool ackReq) {
  JsonDocument doc;
  String msgId = makeMsgId("STM_FULL");
  buildFullState(doc, msgId, ackReq, false);
  if (ackReq) addPendingAck(msgId);
  sendJsonToESP(doc);
}

void sendFullStateToHMI() {
  hmiUpdateAll();
}

// ======================================================
// SYNC REQUEST / RESPONSE
// ======================================================
void sendConfigSyncRequestToESP() {
  JsonDocument doc;
  String msgId = makeMsgId("STM_SYNCREQ");
  doc["msg_id"]          = msgId;
  doc["type"]            = "CONFIG_SYNC_REQUEST";
  doc["cmd"]             = "GET_CONFIG";
  doc["ack_req"]         = true;
  doc["stm_id"]          = STM_ID;
  doc["gateway_id"]      = GW_ID;
  doc["control_id"]      = CONTROL_ID;
  doc["device_id"]       = CONTROL_ID;
  doc["localCfgVersion"] = cfg.cfgVersion;
  doc["mode"]            = modeToString(cfg.mode);
  doc["timestamp_ms"]    = millis();
  JsonArray need = doc["need"].to<JsonArray>();
  need.add("mode");
  need.add("timer_rules");
  need.add("sensor_rules");
  need.add("device_list");
  need.add("fan_config");
  addPendingAck(msgId);
  sendJsonToESP(doc);
}

void applySyncResponse(JsonDocument& doc) {
  if (emergencyActive) {
    sendAckToESP(doc["msg_id"] | "", "ERR", "EMERGENCY_ACTIVE_SYSTEM_LOCKED", "EMERGENCY_LOCKED");
    sendStatusToESP(false);
    return;
  }
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
  bool forceSync = doc["force"].as<bool>() || doc["force_apply"].as<bool>();
  if (serverVersion == cfg.cfgVersion && !forceSync) {
    sendAckToESP(doc["msg_id"] | "", "OK", "CONFIG_ALREADY_LATEST");
    sendFullStateToESP(true);
    return;
  }
  const char* mode = doc["mode"] | nullptr;
  if (mode) cfg.mode = parseMode(mode);

  if (doc["timer_rules"].is<JsonArray>()) {
    JsonArray timers = doc["timer_rules"].as<JsonArray>();
    for (JsonObject relayObj : timers) {
      int r = relayIndexFromNumber(relayObj["relay"] | 0);
      if (r < 0 || !relayObj["schedules"].is<JsonArray>()) continue;
      for (JsonObject item : relayObj["schedules"].as<JsonArray>()) {
        int s = (int)(item["index"] | 0) - 1;
        if (s < 0 || s >= MAX_SCHEDULES) continue;
        cfg.timerRules[r][s].enable = (int)(item["enable"] | 0) == 1 ? 1 : 0;
        cfg.timerRules[r][s].onMin  = parseHHMM(item["on"]  | "00:00");
        cfg.timerRules[r][s].offMin = parseHHMM(item["off"] | "00:00");
      }
    }
  }
  if (doc["sensor_rules"].is<JsonArray>()) {
    for (JsonObject item : doc["sensor_rules"].as<JsonArray>()) {
      int r = relayIndexFromNumber(item["relay"] | 0);
      if (r < 0) continue;
      cfg.sensorRules[r].enable = (int)(item["enable"] | 0) == 1 ? 1 : 0;
      safeCopy(cfg.sensorRules[r].idDevice, sizeof(cfg.sensorRules[r].idDevice), item["id_device"] | "");
      cfg.sensorRules[r].field    = parseField(item["field"]  | "temperature");
      cfg.sensorRules[r].logic    = parseLogic(item["logic"]  | "ABOVE");
      cfg.sensorRules[r].onValue  = item["onValue"]  | item["onAbove"]  | item["onBelow"]  | 0.0;
      cfg.sensorRules[r].offValue = item["offValue"] | item["offBelow"] | item["offAbove"] | 0.0;
    }
  }
  if (doc["fanMode"].is<uint8_t>()) cfg.fanMode   = doc["fanMode"]   | FAN_AUTO;
  if (doc["fanOnTemp"].is<float>()) cfg.fanOnTemp  = doc["fanOnTemp"] | 35.0f;
  if (doc["fanOffTemp"].is<float>())cfg.fanOffTemp = doc["fanOffTemp"]| 30.0f;

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
  JsonDocument doc;
  String msgId = makeMsgId("STM_STATUS");
  doc["msg_id"]        = msgId;
  doc["type"]          = "STATUS";
  doc["ack_req"]       = ackReq;
  doc["stm_id"]        = STM_ID;
  doc["gateway_id"]    = GW_ID;
  doc["control_id"]    = CONTROL_ID;
  doc["device_id"]     = CONTROL_ID;
  doc["mode"]          = modeToString(cfg.mode);
  doc["cfgVersion"]    = cfg.cfgVersion;
  doc["device_count"]  = deviceCount;
  doc["time_valid"]    = timeValid;
  doc["minute_of_day"] = minuteOfDayNow();
  doc["uptime_ms"]     = millis();
  JsonArray relays = doc["relays"].to<JsonArray>();
  for (int i = 0; i < RELAY_COUNT; i++) relays.add(cfg.relayState[i]);
  appendRelayRuntimeMeta(doc);
  doc["fan"]        = fanRunning ? 1 : 0;
  doc["fanMode"]    = cfg.fanMode;
  doc["cabinetTemp"]= cabinetTemp;
  doc["fanOnTemp"]  = cfg.fanOnTemp;
  doc["fanOffTemp"] = cfg.fanOffTemp;
  doc["emergency"]  = emergencyActive ? 1 : 0;
  if (ackReq) addPendingAck(msgId);
  sendJsonToESP(doc);
}

void sendStatusToHMI() {
  hmiUpdateAll();
}

// ======================================================
// DEVICE LIST / SENSOR DATA
// ======================================================
float readFloatAlias(JsonDocument& doc, const char* k1, const char* k2, const char* k3, const char* k4, float cur) {
  const char* keys[4] = {k1, k2, k3, k4};
  for (uint8_t i = 0; i < 4; i++) {
    if (!keys[i] || !strlen(keys[i])) continue;
    JsonVariant v = doc[keys[i]];
    if (!v.isNull()) return v.as<float>();
  }
  return cur;
}

const char* readStringAlias(JsonDocument& doc, const char* k1, const char* k2, const char* k3, const char* k4, const char* def = "") {
  const char* keys[4] = {k1, k2, k3, k4};
  for (uint8_t i = 0; i < 4; i++) {
    if (!keys[i] || !strlen(keys[i])) continue;
    JsonVariant v = doc[keys[i]];
    if (!v.isNull()) return v.as<const char*>();
  }
  return def;
}

void handleDeviceList(JsonDocument& doc) {
  String msgId = doc["msg_id"] | "";
  if (!doc["devices"].is<JsonArray>()) {
    sendAckToESP(msgId, "ERR", "devices array missing", "DEVICE_LIST_INVALID");
    return;
  }
  deviceCount = 0;
  for (JsonObject d : doc["devices"].as<JsonArray>()) {
    if (deviceCount >= MAX_DEVICES) break;
    const char* id   = d["id"] | d["id_device"] | "";
    const char* name = d["name"] | id;
    if (!strlen(id)) continue;
    safeCopy(deviceList[deviceCount].id,   sizeof(deviceList[deviceCount].id),   id);
    safeCopy(deviceList[deviceCount].name, sizeof(deviceList[deviceCount].name), name);
    safeCopy(sensorList[deviceCount].id,   sizeof(sensorList[deviceCount].id),   id);
    sensorList[deviceCount] = {};
    safeCopy(sensorList[deviceCount].id,   sizeof(sensorList[deviceCount].id),   id);
    deviceCount++;
  }
  sendAckToESP(msgId, "OK", "DEVICE_LIST_SAVED");
  sendDeviceListToHMI();
  sendFullStateToESP(true);
}

void handleSensorData(JsonDocument& doc) {
  const char* id = readStringAlias(doc, "id_device", "device_id", "id", "did", "");
  if (!strlen(id)) return;
  int idx = ensureDeviceSlot(id);
  if (idx < 0) return;
  sendDeviceListToHMI();
  sensorList[idx].temperature = readFloatAlias(doc, "temperature", "temp", "t", "T", sensorList[idx].temperature);
  sensorList[idx].humidity    = readFloatAlias(doc, "humidity",    "humi", "h", "H", sensorList[idx].humidity);
  sensorList[idx].co2         = readFloatAlias(doc, "co2",         "CO2",  "c", "C", sensorList[idx].co2);
  sensorList[idx].light       = readFloatAlias(doc, "light",       "lux",  "l", "L", sensorList[idx].light);
  sensorList[idx].voltage     = readFloatAlias(doc, "voltage",     "volt", "v", "V", sensorList[idx].voltage);
  sensorList[idx].current     = readFloatAlias(doc, "current",     "amp",  "a", "A", sensorList[idx].current);
  sensorList[idx].frequency   = readFloatAlias(doc, "frequency",   "freq", "f", "F", sensorList[idx].frequency);
  sensorList[idx].power       = readFloatAlias(doc, "power",       "watt", "p", "P", sensorList[idx].power);
  sensorList[idx].lastMs = millis();
  sensorList[idx].valid  = true;
  hmiUpdateSensorValues(idx);
  // HMI Nextion đã được cập nhật trực tiếp bằng hmiUpdateSensorValues(idx);
}

// ======================================================
// TIMER TASK
// ======================================================
bool scheduleActive(uint16_t nowMin, uint16_t onMin, uint16_t offMin) {
  if (onMin == offMin) return false;
  if (onMin < offMin) return nowMin >= onMin && nowMin < offMin;
  return nowMin >= onMin || nowMin < offMin;
}

void timerTask() {
  if (emergencyActive) return;
  if (cfg.mode != MODE_TIMER) return;
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck < 1000) return;
  lastCheck = millis();
  uint16_t nowMin = minuteOfDayNow();
  for (int r = 0; r < RELAY_COUNT; r++) {
    bool shouldOn = false;
    uint8_t activeSchedule = 0;
    for (int s = 0; s < MAX_SCHEDULES; s++) {
      if (!cfg.timerRules[r][s].enable) continue;
      if (scheduleActive(nowMin, cfg.timerRules[r][s].onMin, cfg.timerRules[r][s].offMin)) {
        shouldOn = true; activeSchedule = s + 1; break;
      }
    }
    markRelayRunInfo(r, "TIMER", activeSchedule);
    bool oldOn = cfg.relayState[r] ? true : false;
    if (oldOn != shouldOn) setRelay(r, shouldOn, "TIMER", true, false, false);
  }
}

// ======================================================
// SENSOR TASK
// ======================================================
void evaluateOneSensorRule(int r) {
  if (r < 0 || r >= RELAY_COUNT) return;
  if (!cfg.sensorRules[r].enable) return;
  int sidx = findSensorIndex(cfg.sensorRules[r].idDevice);
  if (sidx < 0 || !sensorList[sidx].valid) return;
  if (millis() - sensorList[sidx].lastMs > SENSOR_TIMEOUT_MS) return;
  float value = getSensorValue(sensorList[sidx], cfg.sensorRules[r].field);
  if (cfg.sensorRules[r].logic == LOGIC_ABOVE) {
    if (value >= cfg.sensorRules[r].onValue) {
      markRelayRunInfo(r, "SENSOR", 0, cfg.sensorRules[r].idDevice, cfg.sensorRules[r].field, value, cfg.sensorRules[r].onValue, cfg.sensorRules[r].offValue);
      setRelay(r, true, "SENSOR", true, false, false);
    } else if (value <= cfg.sensorRules[r].offValue) {
      markRelayRunInfo(r, "SENSOR", 0, cfg.sensorRules[r].idDevice, cfg.sensorRules[r].field, value, cfg.sensorRules[r].onValue, cfg.sensorRules[r].offValue);
      setRelay(r, false, "SENSOR", true, false, false);
    }
  } else {
    if (value <= cfg.sensorRules[r].onValue) {
      markRelayRunInfo(r, "SENSOR", 0, cfg.sensorRules[r].idDevice, cfg.sensorRules[r].field, value, cfg.sensorRules[r].onValue, cfg.sensorRules[r].offValue);
      setRelay(r, true, "SENSOR", true, false, false);
    } else if (value >= cfg.sensorRules[r].offValue) {
      markRelayRunInfo(r, "SENSOR", 0, cfg.sensorRules[r].idDevice, cfg.sensorRules[r].field, value, cfg.sensorRules[r].onValue, cfg.sensorRules[r].offValue);
      setRelay(r, false, "SENSOR", true, false, false);
    }
  }
}

void sensorTask() {
  if (emergencyActive) return;
  if (cfg.mode != MODE_SENSOR) return;
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck < 1000) return;
  lastCheck = millis();
  for (int r = 0; r < RELAY_COUNT; r++) evaluateOneSensorRule(r);
}

// ======================================================
// LM35 & FAN
// ======================================================
float readLM35() {
  int raw = analogRead(LM35_PIN);
  return raw * 3.3f / 40.95f;
}

void applyFan(bool on) {
  if (on == fanRunning) return;
  fanRunning = on;
  bool level = FAN_ACTIVE_HIGH ? on : !on;
  digitalWrite(FAN_PIN, level ? HIGH : LOW);
  DEBUG_SERIAL.print(F("[FAN] "));
  DEBUG_SERIAL.println(on ? F("ON") : F("OFF"));
}

void applyBuzzer(bool on) {
  if (on == buzzerRunning) return;
  buzzerRunning = on;
  bool level = BUZZER_ACTIVE_HIGH ? on : !on;
  digitalWrite(BUZZER_PIN, level ? HIGH : LOW);
  DEBUG_SERIAL.print(F("[BUZZER] "));
  DEBUG_SERIAL.println(on ? F("ON") : F("OFF"));
}

void evaluateFan() {
  if (cfg.fanMode == FAN_OFF) { applyFan(false); return; }
  if (cfg.fanMode == FAN_ON)  { applyFan(true);  return; }
  if (!fanRunning && cabinetTemp >= cfg.fanOnTemp)  applyFan(true);
  else if (fanRunning && cabinetTemp <= cfg.fanOffTemp) applyFan(false);
}

void sendFanStateToHMI() {
  // Nếu cần hiển thị quạt/nhiệt tủ trên HMI, cập nhật bằng lệnh .txt trực tiếp tại đây.
}

void sendFanStateToESP() {
  JsonDocument doc;
  String msgId = makeMsgId("STM_FAN");
  doc["msg_id"]       = msgId;
  doc["type"]         = "FAN_STATE";
  doc["stm_id"]       = STM_ID;
  doc["gateway_id"]   = GW_ID;
  doc["control_id"]   = CONTROL_ID;
  doc["device_id"]    = CONTROL_ID;
  doc["fan"]          = fanRunning ? 1 : 0;
  doc["fanMode"]      = cfg.fanMode;
  doc["cabinetTemp"]  = cabinetTemp;
  doc["fanOnTemp"]    = cfg.fanOnTemp;
  doc["fanOffTemp"]   = cfg.fanOffTemp;
  doc["cfgVersion"]   = cfg.cfgVersion;
  doc["timestamp_ms"] = millis();
  sendJsonToESP(doc);
}


bool isEmergencyReadOnlyCmd(const char* cmd) {
  if (!cmd || !strlen(cmd)) return false;
  return strcmp(cmd, "GET_STATUS") == 0 ||
         strcmp(cmd, "GET_FULL_STATE") == 0 ||
         strcmp(cmd, "GET_FAN") == 0;
}

void rejectEmergencyCommand(const char* source, const String& msgId) {
  ackBySource(source, msgId, "ERR", "EMERGENCY_ACTIVE_SYSTEM_LOCKED", "EMERGENCY_LOCKED");
  sendStatusToHMI();
  sendStatusToESP(false);
}

void readEmergency() {
  static uint32_t lastEmergCheck = 0;
  static bool lastEmerg = false;
  if (millis() - lastEmergCheck < EMERGENCY_CHECK_MS) return;
  lastEmergCheck = millis();

  // Đảo trạng thái nút khẩn cấp:
  // Bình thường = LOW, nhấn khẩn cấp = HIGH.
  bool cur = (digitalRead(EMERGENCY_PIN) == EMERGENCY_ACTIVE);

  if (cur != lastEmerg) {
    lastEmerg = cur;
    emergencyActive = cur;

    if (cur) {
      DEBUG_SERIAL.println(F("[EMERGENCY] ACTIVATED - SYSTEM LOCKED"));
      for (int i = 0; i < RELAY_COUNT; i++) {
        markRelayRunInfo(i, "EMERGENCY");
        relayWriteRaw(i, false);
      }
      applyFan(true);
      applyBuzzer(true);
    } else {
      DEBUG_SERIAL.println(F("[EMERGENCY] CLEARED - RELAYS KEEP OFF"));
      applyBuzzer(false);

      // An toàn: không tự khôi phục relay sau khi nhả khẩn cấp.
      // Người vận hành phải bật lại bằng tay hoặc chọn lại chế độ chạy.
      for (int i = 0; i < RELAY_COUNT; i++) {
        markRelayRunInfo(i, "SYSTEM");
        relayWriteRaw(i, false);
      }
      evaluateFan();
    }

    sendFanStateToHMI();
    sendFanStateToESP();
    sendStatusToHMI();
    sendStatusToESP(false);
    sendFullStateToESP(true);
  }
}

void lm35FanTask() {
  static uint32_t lastRead = 0;
  static uint32_t lastSend = 0;
  readEmergency();
  if (emergencyActive) {
    if (millis() - lastSend >= 3000) { lastSend = millis(); sendFanStateToHMI(); }
    return;
  }
  if (millis() - lastRead >= LM35_READ_INTERVAL_MS) {
    lastRead = millis();
    cabinetTemp = readLM35();
    DEBUG_SERIAL.print(F("[LM35] Temp="));
    DEBUG_SERIAL.print(cabinetTemp);
    DEBUG_SERIAL.println(F(" C"));
  }
  evaluateFan();
  if (millis() - lastSend >= 3000) { lastSend = millis(); sendFanStateToHMI(); }
}

// ======================================================
// CONFIG STATE
// ======================================================
void sendConfigStateToESP(const char* cmd, int relayIdx, bool ackReq, int scheduleIdx) {
  JsonDocument doc;
  String msgId = makeMsgId("STM_CFG");
  doc["msg_id"]       = msgId;
  doc["type"]         = "CONFIG_STATE";
  doc["cmd"]          = cmd;
  doc["ack_req"]      = ackReq;
  doc["stm_id"]       = STM_ID;
  doc["gateway_id"]   = GW_ID;
  doc["control_id"]   = CONTROL_ID;
  doc["device_id"]    = CONTROL_ID;
  doc["mode"]         = modeToString(cfg.mode);
  doc["cfgVersion"]   = cfg.cfgVersion;
  doc["timestamp_ms"] = millis();
  if (relayIdx >= 0) doc["relay"] = relayIdx + 1;

  if (strcmp(cmd, "SET_TIMER") == 0 && relayIdx >= 0) {
    if (scheduleIdx >= 0 && scheduleIdx < MAX_SCHEDULES) {
      char onBuf[8], offBuf[8];
      formatHHMM(cfg.timerRules[relayIdx][scheduleIdx].onMin,  onBuf,  sizeof(onBuf));
      formatHHMM(cfg.timerRules[relayIdx][scheduleIdx].offMin, offBuf, sizeof(offBuf));
      doc["index"]  = scheduleIdx + 1;
      doc["enable"] = cfg.timerRules[relayIdx][scheduleIdx].enable;
      doc["on"]     = onBuf;
      doc["off"]    = offBuf;
    } else {
      uint8_t enabledCount = 0;
      for (int i = 0; i < MAX_SCHEDULES; i++) if (cfg.timerRules[relayIdx][i].enable) enabledCount++;
      doc["schedule_count"]         = MAX_SCHEDULES;
      doc["enabled_schedule_count"] = enabledCount;
    }
  }
  if (strcmp(cmd, "SET_SENSOR_RULE") == 0 && relayIdx >= 0) {
    SensorRule& r = cfg.sensorRules[relayIdx];
    doc["enable"]    = r.enable;
    doc["id_device"] = r.idDevice;
    doc["field"]     = fieldToString(r.field);
    doc["logic"]     = logicToString(r.logic);
    doc["onValue"]   = r.onValue;
    doc["offValue"]  = r.offValue;
  }
  if (ackReq) addPendingAck(msgId);
  sendJsonToESP(doc);
}

// ======================================================
// COMMAND HANDLERS
// ======================================================
void ackBySource(const char* source, const String& msgId, const char* status, const char* message, const char* errorCode) {
  if (strcmp(source, "ESP") == 0) sendAckToESP(msgId, status, message, errorCode);
  else                             sendAckToHMI(msgId, status, message, errorCode);
}

void handleSetMode(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";
  String modeString = "";
  if (!doc["mode"].isNull()) modeString = doc["mode"].as<String>();
  if (!modeString.length() && !doc["data"].isNull() && !doc["data"]["mode"].isNull()) modeString = doc["data"]["mode"].as<String>();
  if (!modeString.length() && !doc["value"].isNull()) modeString = doc["value"].as<String>();
  modeString.trim(); modeString.toUpperCase();

  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println(F("[CMD] SET_MODE"));
  DEBUG_SERIAL.print(F("msgId=")); DEBUG_SERIAL.println(msgId);
  DEBUG_SERIAL.print(F("mode="));  DEBUG_SERIAL.println(modeString.length() ? modeString : "NULL");

  uint8_t newMode;
  if      (modeString == "MANUAL") newMode = MODE_MANUAL;
  else if (modeString == "TIMER")  newMode = MODE_TIMER;
  else if (modeString == "SENSOR") newMode = MODE_SENSOR;
  else { ackBySource(source, msgId, "ERR", "mode missing or invalid", "MODE_MISSING"); return; }

  uint8_t oldMode = cfg.mode;
  cfg.mode = newMode;
  ackBySource(source, msgId, "OK", "MODE_SET");
  sendStatusToHMI();
  if (oldMode != newMode) saveConfigToFlash(true);
  if (ENABLE_CONFIG_SYNC_TO_ESP) {
    sendConfigStateToESP("SET_MODE", -1, strcmp(source, "HMI") == 0);
    sendFullStateToESP(true);
  }
}

void handleSetRelay(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";
  if (strcmp(source, "ESP") == 0 && ALLOW_ESP_RELAY_CONTROL == 0) {
    ackBySource(source, msgId, "ERR", "ESP relay control disabled", "ESP_RELAY_DISABLED");
    return;
  }
  JsonVariant data = doc["data"];
  int relay = data["relay"] | doc["relay"] | 0;
  int state = -1;
  if (data["state"].is<int>())      state = data["state"].as<int>();
  else if (doc["state"].is<int>())  state = doc["state"].as<int>();
  const char* modeStr = data["mode"] | doc["mode"] | nullptr;
  if (modeStr) cfg.mode = parseMode(modeStr);
  int idx = relayIndexFromNumber(relay);
  if (idx < 0) { ackBySource(source, msgId, "ERR", "relay invalid", "RELAY_INVALID"); return; }
  if (state != 0 && state != 1) { ackBySource(source, msgId, "ERR", "state invalid", "STATE_INVALID"); return; }
  cfg.mode = MODE_MANUAL;
  markRelayRunInfo(idx, "MANUAL");
  setRelay(idx, state == 1, source, true, false, false);
  ackBySource(source, msgId, "OK", state == 1 ? "RELAY_ON" : "RELAY_OFF");
  if (strcmp(source, "ESP") == 0) {
    sendRelayStateToESP(idx, source, false);
    sendStatusToESP(false);
  }
  sendStatusToHMI();
}

void handleTestRelay(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";
  JsonVariant data = doc["data"];
  int relay = data["relay"] | doc["relay"] | 0;
  int idx = relayIndexFromNumber(relay);
  if (idx < 0) { ackBySource(source, msgId, "ERR", "relay invalid", "RELAY_INVALID"); return; }
  ackBySource(source, msgId, "OK", "RELAY_TEST_START");
  relayPhysicalTest((uint8_t)relay);
  ackBySource(source, msgId, "OK", "RELAY_TEST_DONE");
}

void handleSetTimer(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";
  JsonVariant data = doc["data"];
  int relay = 0, index = 1, enable = 0;
  const char* onStr = "00:00";
  const char* offStr = "00:00";
  bool hasScheduleArray = false;
  bool changedSchedules[MAX_SCHEDULES] = { false };

  if (!data.isNull() && data["schedules"].is<JsonArray>()) {
    hasScheduleArray = true; relay = data["relay"] | doc["relay"] | 0;
  } else if (doc["schedules"].is<JsonArray>()) {
    hasScheduleArray = true; relay = data["relay"] | doc["relay"] | 0;
  } else {
    relay  = data["relay"]  | doc["relay"]  | 0;
    index  = data["index"]  | doc["index"]  | 1;
    enable = data["enable"] | doc["enable"] | 0;
    onStr  = data["on"]     | doc["on"]     | "00:00";
    offStr = data["off"]    | doc["off"]    | "00:00";
  }

  int idx = relayIndexFromNumber(relay);
  if (idx < 0) { ackBySource(source, msgId, "ERR", "relay invalid", "RELAY_INVALID"); return; }

  if (hasScheduleArray) {
    JsonArray arr = (!data.isNull() && data["schedules"].is<JsonArray>())
                    ? data["schedules"].as<JsonArray>()
                    : doc["schedules"].as<JsonArray>();
    for (JsonObject s : arr) {
      int si = ((int)(s["index"] | 0)) - 1;
      if (si < 0 || si >= MAX_SCHEDULES) continue;
      cfg.timerRules[idx][si].enable = (int)(s["enable"] | 0) == 1 ? 1 : 0;
      cfg.timerRules[idx][si].onMin  = parseHHMM(s["on"]  | "00:00");
      cfg.timerRules[idx][si].offMin = parseHHMM(s["off"] | "00:00");
      changedSchedules[si] = true;
    }
  } else {
    int si = index - 1;
    if (si < 0 || si >= MAX_SCHEDULES) {
      ackBySource(source, msgId, "ERR", "schedule index invalid", "SCHEDULE_INDEX_INVALID");
      return;
    }
    cfg.timerRules[idx][si].enable = enable == 1 ? 1 : 0;
    cfg.timerRules[idx][si].onMin  = parseHHMM(onStr);
    cfg.timerRules[idx][si].offMin = parseHHMM(offStr);
    changedSchedules[si] = true;
  }

  cfg.mode = MODE_TIMER;
  ackBySource(source, msgId, "OK", "TIMER_SAVED");
  sendStatusToHMI();
  saveConfigToFlash(true);
  if (ENABLE_CONFIG_SYNC_TO_ESP) {
    bool ackOnce = strcmp(source, "HMI") == 0;
    bool sentAny = false;
    for (int si = 0; si < MAX_SCHEDULES; si++) {
      if (!changedSchedules[si]) continue;
      sendConfigStateToESP("SET_TIMER", idx, ackOnce && !sentAny, si);
      sentAny = true;
    }
    if (!sentAny) sendConfigStateToESP("SET_TIMER", idx, ackOnce, -1);
  }
}

void handleSetSensorRule(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";
  JsonVariant data = doc["data"];
  int relay = data["relay"] | doc["relay"] | 0;
  int idx = relayIndexFromNumber(relay);
  if (idx < 0) { ackBySource(source, msgId, "ERR", "relay invalid", "RELAY_INVALID"); return; }
  const char* idDevice = data["id_device"] | doc["id_device"] | "";
  const char* field    = data["field"]     | doc["field"]     | "temperature";
  const char* logic    = data["logic"]     | doc["logic"]     | "ABOVE";
  if (!strlen(idDevice)) { ackBySource(source, msgId, "ERR", "device missing", "DEVICE_MISSING"); return; }
  if (ensureDeviceSlot(idDevice) < 0) { ackBySource(source, msgId, "ERR", "device list full", "DEVICE_FULL"); return; }
  SensorRule& r = cfg.sensorRules[idx];
  if      (data["enable"].is<int>()) r.enable = data["enable"].as<int>() ? 1 : 0;
  else if (doc["enable"].is<int>())  r.enable = doc["enable"].as<int>()  ? 1 : 0;
  else r.enable = 1;
  safeCopy(r.idDevice, sizeof(r.idDevice), idDevice);
  r.field = parseField(field);
  r.logic = parseLogic(logic);
  if (r.logic == LOGIC_ABOVE) {
    r.onValue  = data["onAbove"]  | doc["onAbove"]  | data["onValue"]  | doc["onValue"]  | 0.0;
    r.offValue = data["offBelow"] | doc["offBelow"] | data["offValue"] | doc["offValue"] | 0.0;
  } else {
    r.onValue  = data["onBelow"]  | doc["onBelow"]  | data["onValue"]  | doc["onValue"]  | 0.0;
    r.offValue = data["offAbove"] | doc["offAbove"] | data["offValue"] | doc["offValue"] | 0.0;
  }
  cfg.mode = MODE_SENSOR;
  ackBySource(source, msgId, "OK", "SENSOR_RULE_SAVED");
  sendStatusToHMI();
  evaluateOneSensorRule(idx);
  saveConfigToFlash(true);
  if (ENABLE_CONFIG_SYNC_TO_ESP) sendConfigStateToESP("SET_SENSOR_RULE", idx, strcmp(source, "HMI") == 0);
}

void handleSetTime(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";
  JsonVariant data = doc["data"];
  if (!data.isNull() && data["epoch"].is<uint32_t>()) {
    setClockFromEpoch(data["epoch"] | 0);
  } else if (doc["epoch"].is<uint32_t>()) {
    setClockFromEpoch(doc["epoch"] | 0);
  } else {
    int hh = -1, mm = -1;
    if      (data["hour"].is<int>())   hh = data["hour"].as<int>();
    else if (doc["hour"].is<int>())    hh = doc["hour"].as<int>();
    if      (data["minute"].is<int>()) mm = data["minute"].as<int>();
    else if (doc["minute"].is<int>())  mm = doc["minute"].as<int>();
    if (hh < 0 || mm < 0) { ackBySource(source, msgId, "ERR", "time missing", "TIME_MISSING"); return; }
    setClockFromHM(hh, mm);
  }
  ackBySource(source, msgId, "OK", "TIME_SET");
  {
    JsonDocument boot;
    boot["type"]         = "BOOT";
    boot["fw"]           = FW_VERSION;
    boot["timer_format"] = "COMPACT_OR_SCHEDULES";
    boot["hmi_baud"]     = HMI_BAUD;
    boot["timestamp_ms"] = millis();
    sendJsonToHMI(boot);
  }
  sendStatusToHMI();
  sendStatusToESP(false);
}

void handleSetFan(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";
  JsonVariant data = doc["data"];
  int fm = -1, fanCmd = -1;
  if      (data["fanMode"].is<int>()) fm     = data["fanMode"].as<int>();
  else if (doc["fanMode"].is<int>())  fm     = doc["fanMode"].as<int>();
  if      (data["fan"].is<int>())     fanCmd = data["fan"].as<int>();
  else if (doc["fan"].is<int>())      fanCmd = doc["fan"].as<int>();
  if      (fanCmd == 0)                           cfg.fanMode = FAN_OFF;
  else if (fanCmd == 1)                           cfg.fanMode = FAN_ON;
  else if (fm >= FAN_OFF && fm <= FAN_ON)         cfg.fanMode = fm;
  else { ackBySource(source, msgId, "ERR", "fanMode or fan missing", "FAN_MODE_MISSING"); return; }
  ackBySource(source, msgId, "OK", "FAN_SET");
  if (cfg.fanMode == FAN_OFF) applyFan(false);
  if (cfg.fanMode == FAN_ON)  applyFan(true);
  sendFanStateToHMI();
  saveConfigToFlash(true);
  if (ENABLE_CONFIG_SYNC_TO_ESP) { sendFanStateToESP(); sendStatusToESP(strcmp(source, "HMI") == 0); }
}

void handleSetFanThreshold(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";
  JsonVariant data = doc["data"];
  float onTemp = -999.0f, offTemp = -999.0f;
  if      (data["fanOnTemp"].is<float>()) onTemp  = data["fanOnTemp"].as<float>();
  else if (doc["fanOnTemp"].is<float>())  onTemp  = doc["fanOnTemp"].as<float>();
  if      (data["fanOffTemp"].is<float>())offTemp = data["fanOffTemp"].as<float>();
  else if (doc["fanOffTemp"].is<float>()) offTemp = doc["fanOffTemp"].as<float>();
  if (onTemp < -100.0f || offTemp < -100.0f) { ackBySource(source, msgId, "ERR", "threshold missing", "THRESHOLD_MISSING"); return; }
  if (onTemp <= offTemp) { ackBySource(source, msgId, "ERR", "onTemp must be > offTemp", "THRESHOLD_INVALID"); return; }
  cfg.fanOnTemp = onTemp; cfg.fanOffTemp = offTemp;
  ackBySource(source, msgId, "OK", "FAN_THRESHOLD_SAVED");
  sendFanStateToHMI();
  saveConfigToFlash(true);
  if (ENABLE_CONFIG_SYNC_TO_ESP) { sendFanStateToESP(); sendStatusToESP(strcmp(source, "HMI") == 0); }
}

void handleSaveFlash(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";
  ackBySource(source, msgId, "OK", "FLASH_SAVED");
  saveConfigToFlash(false);
  if (ENABLE_CONFIG_SYNC_TO_ESP) sendFullStateToESP(true);
}

void handleGetFullState(JsonDocument& doc, const char* source) {
  String msgId = doc["msg_id"] | "";
  ackBySource(source, msgId, "OK", "FULL_STATE_SENT");
  if (strcmp(source, "ESP") == 0) sendFullStateToESP(true);
  sendFullStateToHMI();
}

void handleCommand(JsonDocument& doc, const char* source) {
  const char* cmd = doc["cmd"] | "";
  if (!strlen(cmd)) { ackBySource(source, doc["msg_id"] | "", "ERR", "cmd missing", "CMD_MISSING"); return; }

  if (emergencyActive && !isEmergencyReadOnlyCmd(cmd)) {
    rejectEmergencyCommand(source, doc["msg_id"] | "");
    return;
  }

  if      (strcmp(cmd, "SET_MODE")        == 0) handleSetMode(doc, source);
  else if (strcmp(cmd, "SET_RELAY")       == 0) handleSetRelay(doc, source);
  else if (strcmp(cmd, "TEST_RELAY")      == 0) handleTestRelay(doc, source);
  else if (strcmp(cmd, "SET_TIMER")       == 0) handleSetTimer(doc, source);
  else if (strcmp(cmd, "SET_SENSOR_RULE") == 0) handleSetSensorRule(doc, source);
  else if (strcmp(cmd, "SET_TIME")        == 0) handleSetTime(doc, source);
  else if (strcmp(cmd, "SAVE_FLASH")      == 0) handleSaveFlash(doc, source);
  else if (strcmp(cmd, "GET_FULL_STATE")  == 0) handleGetFullState(doc, source);
  else if (strcmp(cmd, "GET_STATUS")      == 0) {
    ackBySource(source, doc["msg_id"] | "", "OK", "STATUS_SENT");
    sendStatusToHMI();
    if (strcmp(source, "ESP") == 0) sendStatusToESP(false);
  }
  else if (strcmp(cmd, "GET_DEVICE_LIST") == 0) {
    ackBySource(source, doc["msg_id"] | "", "OK", "DEVICE_LIST_SENT");
    sendDeviceListToHMI();
  }
  else if (strcmp(cmd, "SET_FAN")           == 0) handleSetFan(doc, source);
  else if (strcmp(cmd, "SET_FAN_THRESHOLD") == 0) handleSetFanThreshold(doc, source);
  else if (strcmp(cmd, "GET_FAN")           == 0) {
    ackBySource(source, doc["msg_id"] | "", "OK", "FAN_STATE_SENT");
    sendFanStateToHMI();
    sendFanStateToESP();
  }
  else if (strcmp(cmd, "CONFIG_SYNC_REQUEST") == 0 || strcmp(cmd, "GET_CONFIG") == 0) {
    sendConfigSyncRequestToESP();
    ackBySource(source, doc["msg_id"] | "", "OK", "SYNC_REQUEST_SENT");
  }
  else { ackBySource(source, doc["msg_id"] | "", "ERR", "unknown cmd", "CMD_UNKNOWN"); }
}

// ======================================================
// ACK FROM SERVER
// ======================================================
void handleAckFromESP(JsonDocument& doc) {
  String ackFor = doc["ack_for"] | "";
  if (ackFor.length()) removePendingAck(ackFor);
  hmiUpdateAll();
}

// ======================================================
// PROCESS ESP / HMI LINE
// ======================================================
void processESPLine(const String& line) {
  if (!line.length()) return;
  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println(F("[ESP32 -> STM32]"));
  DEBUG_SERIAL.println(line);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    DEBUG_SERIAL.print(F("[ESP JSON ERR] "));
    DEBUG_SERIAL.println(err.c_str());
    return;
  }
  const char* type = doc["type"] | "";
  if      (strcmp(type, "DEVICE_LIST") == 0)                                           handleDeviceList(doc);
  else if (strcmp(type, "SENSOR") == 0)                                                handleSensorData(doc);
  else if (strcmp(type, "CMD") == 0 || strcmp(type, "CONFIG") == 0 || doc["cmd"].is<const char*>()) handleCommand(doc, "ESP");
  else if (strcmp(type, "ACK") == 0)                                                   handleAckFromESP(doc);
  else if (strcmp(type, "CONFIG_SYNC_RESPONSE") == 0 || strcmp(type, "SYNC_RESPONSE") == 0) applySyncResponse(doc);
  else sendAckToESP(doc["msg_id"] | "", "ERR", "unknown type", "TYPE_UNKNOWN");
}

void processHMILine(const String& line) {
  if (!line.length()) return;
  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println(F("[HMI -> STM32]"));
  DEBUG_SERIAL.println(line);

  String t = line; t.trim();
  if (t.length() > 0 && t[0] != '{' && t[0] != '[') { handleHmiAsciiCommand(t); return; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    DEBUG_SERIAL.print(F("[HMI JSON ERR] "));
    DEBUG_SERIAL.println(err.c_str());
    // Bỏ qua JSON lỗi từ HMI/Nextion để tránh treo do byte nhiễu.
    return;
  }
  const char* type = doc["type"] | "";
  if (strcmp(type, "CMD") == 0 || strcmp(type, "CONFIG") == 0 || doc["cmd"].is<const char*>())
    handleCommand(doc, "HMI");
  else
    sendAckToHMI(doc["msg_id"] | "", "ERR", "unknown hmi type", "TYPE_UNKNOWN");
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
      if (espLine.length()) processESPLine(espLine);
      espLine = "";
    } else {
      espLine += c;
      if (espLine.length() > 8192) espLine = "";
    }
  }
}

void readHMISerial() {
  while (HMI_SERIAL.available()) {
    uint8_t b = (uint8_t)HMI_SERIAL.read();

    // Nextion thường trả về 0x04 FF FF FF khi thực thi lệnh.
    // Lọc bỏ để STM chỉ nhận lệnh ASCII như B0, PG:OV, MODE:MANUAL...
    if (b == 0xFF || b == 0x04 || b == 0x00) continue;

    char c = (char)b;
    if (c == '\r') continue;
    if (c == '\n') {
      hmiLine.trim();
      if (hmiLine.length()) processHMILine(hmiLine);
      hmiLine = "";
    } else {
      // Chỉ nhận ký tự ASCII in được để tránh rác UART.
      if (c >= 32 && c <= 126) hmiLine += c;
      if (hmiLine.length() > 80) hmiLine = "";
    }
  }
}

// ======================================================
// INIT
// ======================================================
void initBuzzer() {
  pinMode(BUZZER_PIN, OUTPUT);
  applyBuzzer(false);
}

void initFan() {
  pinMode(FAN_PIN, OUTPUT);
  pinMode(EMERGENCY_PIN, INPUT_PULLDOWN);
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
  if (millis() - last >= 10000) { last = millis(); sendStatusToESP(false); }
}

void bootSyncTask() {
  static bool done = false;
  static uint32_t bootMs = 0;
  if (emergencyActive) return;
  if (done) return;
  if (bootMs == 0) bootMs = millis();
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
  DEBUG_SERIAL.println(F("====================================="));
  DEBUG_SERIAL.println(F("STM32 F103C8 FUVIAIR RELAY V10 START"));
  DEBUG_SERIAL.println(F("====================================="));

  bool ok = loadConfigFromFlash();
  if (!ok) saveConfigToFlash(false);

  ESP_SERIAL.begin(ESP_BAUD);
  HMI_SERIAL.begin(HMI_BAUD);

  clearPendingAcks();
  initRelayRunInfo();
  initRelays();
  initBuzzer();
  initFan();

  cabinetTemp = readLM35();
  DEBUG_SERIAL.print(F("[LM35] Initial temp="));
  DEBUG_SERIAL.println(cabinetTemp);

  sendStatusToHMI();
  sendStatusToESP(false);
}

void loop() {
  // Đọc khẩn cấp trước mọi chức năng để khóa hệ thống nhanh nhất.
  lm35FanTask();

  readESPSerial();
  readHMISerial();
  hmiRefreshTask();

  if (!emergencyActive) {
    timerTask();
    sensorTask();
    checkPendingAckTimeouts();
    bootSyncTask();
  }

  periodicStatusTask();
}
