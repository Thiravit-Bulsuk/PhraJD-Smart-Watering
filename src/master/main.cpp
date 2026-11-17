// บอร์ด 1: บอร์ดหลัก ควบคุมลำดับวาล์ว/ปั๊ม, UDP ผ่านเราท์เตอร์ไปยังบอร์ด 2-4, MQTT ไปยังเว็บแอป
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <time.h>

#include "BoardConfig.h"
#include "Protocol.h"
#include "RootCA.h"
#include "secrets.h"

// Logs are uploaded to the web app (kept 3 days in Firestore); leave LOG_TOKEN empty to disable.
#ifndef LOG_URL
#define LOG_URL ""
#endif
#ifndef LOG_TOKEN
#define LOG_TOKEN ""
#endif

#define TOPIC_MASTER "phrajd/watering/master"
#define TOPIC_STATE "phrajd/watering/state"
#define TOPIC_CONFIG "phrajd/watering/config"
#define TOPIC_LOG "phrajd/watering/log"
#define TOPIC_CMD "phrajd/watering/cmd"
// Retained JD tank level (0-100 %) published by the Wat Doi monitoring system every few minutes.
#define TOPIC_TANK "JD"

struct ValveRef {
  uint8_t node;
  uint8_t idx;
};
// Watering order: board 2 valves 1-4, board 3 valve 1, board 4 valves 1-2.
constexpr ValveRef VALVES[] = {{2, 0}, {2, 1}, {2, 2}, {2, 3}, {3, 0}, {4, 0}, {4, 1}};
constexpr int TOTAL_VALVES = sizeof(VALVES) / sizeof(VALVES[0]);

constexpr uint32_t OPEN_CONFIRM_TIMEOUT_MS = 60000;
// Motorized valves take about 5 s to travel fully open after the node reports its relay on.
constexpr uint32_t VALVE_TRAVEL_MS = 6000;
constexpr uint32_t PUMP_STOP_BEFORE_CLOSE_MS = 5000;
constexpr uint32_t PUMP_RESTART_DELAY_MS = 15000;
constexpr uint32_t MANUAL_TIMEOUT_MS = 30UL * 60 * 1000;
constexpr uint16_t MAX_DURATION_MIN = 240;
constexpr uint32_t STATE_HEARTBEAT_MS = 10000;
constexpr int ROUND_COUNT = 4;
constexpr uint8_t DEFAULT_START_HOURS[ROUND_COUNT] = {2, 6, 12, 17};
constexpr float TANK_MIN_PERCENT = 40;
constexpr uint32_t TANK_STALE_MS = 15UL * 60 * 1000;
constexpr uint32_t LOG_UPLOAD_INTERVAL_MS = 60000;
constexpr int LOG_BUFFER_SIZE = 40;

// Every valve in a round is open for the same number of minutes.
struct Round {
  bool enabled;
  uint8_t startHour;
  uint8_t startMinute;
  uint16_t durationMin;
};

struct Config {
  bool autoEnabled;
  Round rounds[ROUND_COUNT];
};

// Layout saved by the previous firmware (2 rounds, per-valve minutes), used only for migration.
struct ConfigV2 {
  bool autoEnabled;
  struct {
    bool enabled;
    uint8_t startHour;
    uint8_t startMinute;
    uint16_t durationMin[TOTAL_VALVES];
  } rounds[2];
};

struct NodeState {
  bool seen;
  bool online;
  uint32_t lastSeenMs;
  uint8_t reportedMask;
  uint8_t desiredMask;
  uint32_t openedAtMs[8];
  IPAddress ip;
  int8_t rssi;
  uint32_t uptimeSec;
  uint32_t lastSentMs;
  bool sendNow;
};

enum Phase : uint8_t { PHASE_IDLE, PHASE_WAIT_OPEN, PHASE_RUNNING, PHASE_FINISHING, PHASE_MANUAL };
const char* const PHASE_NAMES[] = {"idle", "opening", "running", "finishing", "manual"};

enum CmdType : uint8_t { CMD_START, CMD_STOP, CMD_CONFIG, CMD_VALVE, CMD_PUMP, CMD_AUTO };
struct Command {
  CmdType type;
  int8_t valve;
  int8_t round;
  bool on;
  Config cfg;
};

struct LogMsg {
  char text[160];
};

struct PendingLog {
  uint32_t time;
  uint32_t seq;
  char text[160];
};

Preferences prefs;
Config cfg;
NodeState nodes[LAST_NODE_ID + 1] = {};
Phase phase = PHASE_IDLE;
int curStep = -1;
int prevStep = -1;
int runRound = 0;
uint32_t phaseStartMs = 0;
uint32_t stepStartMs = 0;
bool pumpOn = false;
uint32_t pumpOffAtMs = 0;
bool manualPump = false;
uint32_t manualLastCmdMs = 0;
int pendingCloseValve = -1;
uint32_t pendingCloseAtMs = 0;
uint32_t lastRunDate[ROUND_COUNT] = {};
float tankLevel = -1;
uint32_t tankUpdatedMs = 0;

volatile uint32_t stateVersion = 0;
volatile uint32_t configVersion = 0;

WiFiUDP udp;
bool udpStarted = false;
QueueHandle_t cmdQueue;
QueueHandle_t logQueue;
SemaphoreHandle_t stateMutex;

WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);

void logf(const char* fmt, ...) {
  LogMsg m;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(m.text, sizeof(m.text), fmt, ap);
  va_end(ap);
  Serial.println(m.text);
  xQueueSend(logQueue, &m, 0);
}

// ---------- Config ----------

void lastRunKey(int r, char* key) { snprintf(key, 8, "last%d", r); }

void loadConfig() {
  if (prefs.getBytesLength("cfg3") == sizeof(cfg)) {
    prefs.getBytes("cfg3", &cfg, sizeof(cfg));
  } else {
    cfg.autoEnabled = true;
    for (int r = 0; r < ROUND_COUNT; r++) cfg.rounds[r] = {r == 0, DEFAULT_START_HOURS[r], 0, 30};
    ConfigV2 old;
    if (prefs.getBytesLength("cfg2") == sizeof(old)) {
      prefs.getBytes("cfg2", &old, sizeof(old));
      cfg.autoEnabled = old.autoEnabled;
      for (int r = 0; r < 2; r++) {
        uint16_t longest = 0;
        for (int i = 0; i < TOTAL_VALVES; i++) longest = max(longest, old.rounds[r].durationMin[i]);
        cfg.rounds[r] = {old.rounds[r].enabled, old.rounds[r].startHour, old.rounds[r].startMinute,
                         longest ? longest : (uint16_t)30};
      }
    }
  }
  for (int r = 0; r < ROUND_COUNT; r++) {
    char key[8];
    lastRunKey(r, key);
    lastRunDate[r] = prefs.isKey(key) ? prefs.getUInt(key, 0) : 0;
  }
}

void saveConfig() { prefs.putBytes("cfg3", &cfg, sizeof(cfg)); }

// ---------- Nodes / valves ----------

bool nodeOnline(uint8_t id) { return nodes[id].seen && millis() - nodes[id].lastSeenMs < NODE_OFFLINE_MS; }

bool valveConfirmedOpen(int v) {
  const ValveRef& r = VALVES[v];
  return nodeOnline(r.node) && (nodes[r.node].reportedMask & (1 << r.idx));
}

bool anyValveConfirmedOpen() {
  for (uint8_t id = FIRST_NODE_ID; id <= LAST_NODE_ID; id++) {
    if (nodeOnline(id) && nodes[id].reportedMask) return true;
  }
  return false;
}

bool valveFullyOpen(int v) {
  return valveConfirmedOpen(v) && millis() - nodes[VALVES[v].node].openedAtMs[VALVES[v].idx] >= VALVE_TRAVEL_MS;
}

bool anyValveFullyOpen() {
  for (int v = 0; v < TOTAL_VALVES; v++) {
    if (valveFullyOpen(v)) return true;
  }
  return false;
}

int desiredOpenCount() {
  int n = 0;
  for (int v = 0; v < TOTAL_VALVES; v++) {
    if (nodes[VALVES[v].node].desiredMask & (1 << VALVES[v].idx)) n++;
  }
  return n;
}

void setDesired(int v, bool open) {
  NodeState& n = nodes[VALVES[v].node];
  uint8_t before = n.desiredMask;
  if (open) n.desiredMask |= (1 << VALVES[v].idx);
  else n.desiredMask &= ~(1 << VALVES[v].idx);
  if (n.desiredMask != before) {
    n.sendNow = true;
    stateVersion++;
  }
}

void closeAllValves() {
  for (uint8_t id = FIRST_NODE_ID; id <= LAST_NODE_ID; id++) {
    if (nodes[id].desiredMask) stateVersion++;
    nodes[id].desiredMask = 0;
    nodes[id].sendNow = true;
  }
  pendingCloseValve = -1;
}

void sendSetValves(uint8_t id) {
  NetPacket p = {};
  p.magic = NET_MAGIC;
  p.type = MSG_SET_VALVES;
  p.src = MASTER_ID;
  p.dst = id;
  p.valveMask = nodes[id].desiredMask;
  p.leaseSec = NODE_LEASE_SEC;
  p.uptimeSec = millis() / 1000;
  p.rssi = WiFi.RSSI();
  // Unicast (Wi-Fi retries + ACK) once the node's IP is known, broadcast until then.
  IPAddress dest = nodeOnline(id) ? nodes[id].ip : WiFi.broadcastIP();
  udp.beginPacket(dest, UDP_PORT);
  udp.write((const uint8_t*)&p, sizeof(p));
  udp.endPacket();
  nodes[id].lastSentMs = millis();
  nodes[id].sendNow = false;
}

void sendToNodes() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!udpStarted) udpStarted = udp.begin(UDP_PORT);
  if (!udpStarted) return;
  for (uint8_t id = FIRST_NODE_ID; id <= LAST_NODE_ID; id++) {
    NodeState& n = nodes[id];
    bool mismatch = !nodeOnline(id) || n.reportedMask != n.desiredMask;
    uint32_t interval = mismatch ? 2000 : 5000;
    if (n.sendNow || millis() - n.lastSentMs >= interval) sendSetValves(id);
  }
}

void processRx() {
  if (!udpStarted) return;
  int size;
  while ((size = udp.parsePacket()) > 0) {
    NetPacket p;
    if (size != sizeof(p) || udp.read((uint8_t*)&p, sizeof(p)) != sizeof(p)) {
      udp.flush();
      continue;
    }
    if (p.magic != NET_MAGIC || p.type != MSG_STATUS || p.dst != MASTER_ID) continue;
    if (p.src < FIRST_NODE_ID || p.src > LAST_NODE_ID) continue;
    NodeState& n = nodes[p.src];
    if (n.reportedMask != p.valveMask) stateVersion++;
    uint8_t newlyOpen = p.valveMask & ~(nodeOnline(p.src) ? n.reportedMask : 0);
    for (int i = 0; i < 8; i++) {
      if (newlyOpen & (1 << i)) n.openedAtMs[i] = millis();
    }
    n.seen = true;
    n.lastSeenMs = millis();
    n.reportedMask = p.valveMask;
    n.ip = udp.remoteIP();
    n.rssi = p.rssi;
    n.uptimeSec = p.uptimeSec;
  }
}

void checkNodesOnline() {
  for (uint8_t id = FIRST_NODE_ID; id <= LAST_NODE_ID; id++) {
    bool online = nodeOnline(id);
    if (online != nodes[id].online) {
      nodes[id].online = online;
      stateVersion++;
      logf(online ? "บอร์ด %d ออนไลน์" : "บอร์ด %d ขาดการติดต่อ", id);
    }
  }
}

// ---------- Pump (interlock: never run without a confirmed open valve and enough water in JD tank) ----------

bool tankFresh() { return tankLevel >= 0 && millis() - tankUpdatedMs < TANK_STALE_MS; }

bool tankOk() { return tankFresh() && tankLevel >= TANK_MIN_PERCENT; }

void stopAll(const char* reason);

void logTankBlocked(const char* what) {
  if (tankFresh()) logf("%s: ระดับน้ำแท็งก์เจดีย์ %.0f%% ต่ำกว่า %.0f%%", what, tankLevel, TANK_MIN_PERCENT);
  else logf("%s: ไม่ทราบระดับน้ำแท็งก์เจดีย์ (ไม่ได้รับข้อมูลเกิน 15 นาที)", what);
}

void writePump(bool on) {
  digitalWrite(PUMP_RELAY_PIN, on ? RELAY_ON : RELAY_OFF);
  if (pumpOn != on) {
    pumpOn = on;
    if (!on) pumpOffAtMs = millis();
    stateVersion++;
  }
}

void updatePump() {
  bool autoRun = phase == PHASE_WAIT_OPEN || phase == PHASE_RUNNING;
  if (!tankOk()) {
    if (pumpOn) {
      writePump(false);
      logTankBlocked("ตัดปั๊มทันที");
    }
    manualPump = false;
    if (autoRun) stopAll("ระดับน้ำแท็งก์เจดีย์ไม่พอ");
    return;
  }

  bool allowed = anyValveConfirmedOpen();
  bool want = autoRun || (phase == PHASE_MANUAL && manualPump);

  if (pumpOn && !allowed) {
    writePump(false);
    manualPump = false;
    logf("ตัดปั๊มอัตโนมัติ: ไม่พบวาล์วที่เปิดอยู่ (ป้องกันปั๊มเสียหาย)");
  } else if (pumpOn && !want) {
    writePump(false);
    logf("ปิดปั๊ม");
  } else if (!pumpOn && want && anyValveFullyOpen()) {
    bool restartOk = pumpOffAtMs == 0 || millis() - pumpOffAtMs >= PUMP_RESTART_DELAY_MS;
    if (phase == PHASE_MANUAL || restartOk) {
      writePump(true);
      logf("เปิดปั๊ม");
    }
  }
}

// ---------- Automatic sequence ----------

uint16_t stepMinutes(int) { return cfg.rounds[runRound].durationMin; }

int nextEnabledStep(int from) {
  for (int i = from; i < TOTAL_VALVES; i++) {
    if (stepMinutes(i) > 0) return i;
  }
  return -1;
}

void beginStep(int s) {
  curStep = s;
  setDesired(s, true);
  phase = PHASE_WAIT_OPEN;
  phaseStartMs = millis();
  stateVersion++;
  logf("กำลังเปิดวาล์ว %d (บอร์ด %d)", s + 1, VALVES[s].node);
}

void beginFinish() {
  phase = PHASE_FINISHING;
  phaseStartMs = millis();
  manualPump = false;
  stateVersion++;
}

void advance(int from) {
  int n = nextEnabledStep(from);
  if (n >= 0) beginStep(n);
  else beginFinish();
}

void startRun(int round, const char* reason) {
  if (phase != PHASE_IDLE) {
    logf("ไม่สามารถเริ่มรอบ %d ได้: ระบบกำลังทำงานอยู่ (%s)", round + 1, PHASE_NAMES[phase]);
    return;
  }
  if (!tankOk()) {
    char what[48];
    snprintf(what, sizeof(what), "ไม่เริ่มรดน้ำรอบ %d", round + 1);
    logTankBlocked(what);
    return;
  }
  runRound = round;
  int s = nextEnabledStep(0);
  if (s < 0) {
    logf("รอบ %d ไม่มีวาล์วที่ตั้งเวลาไว้ ไม่เริ่มรดน้ำ", round + 1);
    return;
  }
  logf("เริ่มรดน้ำรอบ %d (%s)", round + 1, reason);
  closeAllValves();
  prevStep = -1;
  beginStep(s);
}

void stopAll(const char* reason) {
  if (phase == PHASE_IDLE) {
    closeAllValves();
    return;
  }
  logf("หยุดการทำงาน (%s)", reason);
  beginFinish();
}

void tickRun() {
  uint32_t now = millis();
  switch (phase) {
    case PHASE_WAIT_OPEN:
      if (valveFullyOpen(curStep)) {
        // Previous valve closes only after the next one is fully open, so the pump always has a path.
        if (prevStep >= 0) setDesired(prevStep, false);
        prevStep = -1;
        stepStartMs = now;
        phase = PHASE_RUNNING;
        stateVersion++;
        logf("วาล์ว %d เปิดแล้ว รดน้ำ %u นาที", curStep + 1, stepMinutes(curStep));
      } else if (now - phaseStartMs >= OPEN_CONFIRM_TIMEOUT_MS) {
        logf("ผิดพลาด: วาล์ว %d (บอร์ด %d) ไม่ตอบสนอง ข้ามไปวาล์วถัดไป", curStep + 1, VALVES[curStep].node);
        setDesired(curStep, false);
        advance(curStep + 1);
      }
      break;
    case PHASE_RUNNING:
      if (now - stepStartMs >= stepMinutes(curStep) * 60000UL) {
        prevStep = curStep;
        advance(curStep + 1);
      }
      break;
    case PHASE_FINISHING:
      // Valves close only once the pump has been off long enough for pipe pressure to drop.
      if (!pumpOn && now - phaseStartMs >= PUMP_STOP_BEFORE_CLOSE_MS && now - pumpOffAtMs >= PUMP_STOP_BEFORE_CLOSE_MS) {
        closeAllValves();
        phase = PHASE_IDLE;
        curStep = prevStep = -1;
        stateVersion++;
        logf("ปิดวาล์วทั้งหมดแล้ว ระบบพร้อม");
      }
      break;
    case PHASE_MANUAL:
      if (pendingCloseValve >= 0 && !pumpOn && now - pendingCloseAtMs >= PUMP_STOP_BEFORE_CLOSE_MS &&
          now - pumpOffAtMs >= PUMP_STOP_BEFORE_CLOSE_MS) {
        setDesired(pendingCloseValve, false);
        pendingCloseValve = -1;
      }
      if (now - manualLastCmdMs >= MANUAL_TIMEOUT_MS) {
        logf("โหมดทดสอบไม่มีการใช้งานเกิน 30 นาที ปิดทั้งหมดอัตโนมัติ");
        beginFinish();
      } else if (!pumpOn && desiredOpenCount() == 0 && pendingCloseValve < 0) {
        phase = PHASE_IDLE;
        stateVersion++;
      }
      break;
    default:
      break;
  }
}

void tickSchedule() {
  static uint32_t lastCheckMs = 0;
  if (millis() - lastCheckMs < 1000) return;
  lastCheckMs = millis();
  struct tm t;
  if (!getLocalTime(&t, 0) || t.tm_year < 120) return;
  uint32_t today = (t.tm_year + 1900) * 10000UL + (t.tm_mon + 1) * 100UL + t.tm_mday;
  for (int r = 0; r < ROUND_COUNT; r++) {
    const Round& rd = cfg.rounds[r];
    if (!cfg.autoEnabled || !rd.enabled) continue;
    if (t.tm_hour != rd.startHour || t.tm_min != rd.startMinute || lastRunDate[r] == today) continue;
    lastRunDate[r] = today;
    char key[8];
    lastRunKey(r, key);
    prefs.putUInt(key, today);
    if (phase == PHASE_MANUAL) stopAll("ถึงเวลารดน้ำตามตาราง");
    if (phase == PHASE_IDLE) startRun(r, "ตามเวลาที่ตั้งไว้");
    else logf("ข้ามรอบ %d วันนี้: ระบบไม่ว่าง", r + 1);
  }
}

// ---------- Manual test mode ----------

void manualValve(int v, bool open) {
  if (phase != PHASE_IDLE && phase != PHASE_MANUAL) {
    logf("ทดสอบไม่ได้: ระบบกำลังรดน้ำอัตโนมัติ กรุณากดหยุดก่อน");
    return;
  }
  phase = PHASE_MANUAL;
  manualLastCmdMs = millis();
  stateVersion++;
  if (open) {
    if (pendingCloseValve == v) pendingCloseValve = -1;
    setDesired(v, true);
    logf("ทดสอบ: เปิดวาล์ว %d", v + 1);
    return;
  }
  bool isOpen = nodes[VALVES[v].node].desiredMask & (1 << VALVES[v].idx);
  bool pumpRecentlyOn = pumpOn || (pumpOffAtMs && millis() - pumpOffAtMs < PUMP_STOP_BEFORE_CLOSE_MS);
  if (isOpen && desiredOpenCount() == 1 && pumpRecentlyOn) {
    // Closing the last open valve: stop the pump first, close the valve a few seconds later.
    manualPump = false;
    pendingCloseValve = v;
    pendingCloseAtMs = millis();
    logf("ทดสอบ: ปิดปั๊มก่อน แล้วจึงปิดวาล์ว %d", v + 1);
    return;
  }
  setDesired(v, false);
  logf("ทดสอบ: ปิดวาล์ว %d", v + 1);
}

void manualPumpCmd(bool on) {
  if (phase != PHASE_IDLE && phase != PHASE_MANUAL) {
    logf("ทดสอบไม่ได้: ระบบกำลังรดน้ำอัตโนมัติ กรุณากดหยุดก่อน");
    return;
  }
  if (on && !tankOk()) {
    logTankBlocked("ไม่อนุญาตให้เปิดปั๊ม");
    return;
  }
  if (on && !anyValveConfirmedOpen()) {
    logf("ไม่อนุญาตให้เปิดปั๊ม: ต้องเปิดวาล์วอย่างน้อย 1 ตัวก่อน");
    return;
  }
  phase = PHASE_MANUAL;
  manualLastCmdMs = millis();
  manualPump = on;
  stateVersion++;
  if (on && !anyValveFullyOpen()) logf("ทดสอบ: สั่งเปิดปั๊ม (รอวาล์วเปิดเต็มที่ก่อน)");
  else logf(on ? "ทดสอบ: สั่งเปิดปั๊ม" : "ทดสอบ: สั่งปิดปั๊ม");
}

void processCommands() {
  Command c;
  while (xQueueReceive(cmdQueue, &c, 0) == pdTRUE) {
    switch (c.type) {
      case CMD_START: startRun(c.round, "สั่งจากเว็บแอป"); break;
      case CMD_STOP: stopAll("สั่งจากเว็บแอป"); break;
      case CMD_VALVE: manualValve(c.valve, c.on); break;
      case CMD_PUMP: manualPumpCmd(c.on); break;
      case CMD_AUTO:
        cfg.autoEnabled = c.on;
        saveConfig();
        configVersion++;
        logf(c.on ? "เปิดใช้งานการตั้งเวลาอัตโนมัติ" : "ปิดใช้งานการตั้งเวลาอัตโนมัติ");
        break;
      case CMD_CONFIG: {
        memcpy(cfg.rounds, c.cfg.rounds, sizeof(cfg.rounds));
        saveConfig();
        configVersion++;
        char summary[96] = "";
        for (int r = 0; r < ROUND_COUNT; r++) {
          const Round& rd = cfg.rounds[r];
          if (!rd.enabled) continue;
          size_t len = strlen(summary);
          snprintf(summary + len, sizeof(summary) - len, " #%d %02u:%02u/%uน.", r + 1, rd.startHour, rd.startMinute,
                   rd.durationMin);
        }
        logf("บันทึกการตั้งเวลาใหม่:%s", summary[0] ? summary : " ไม่มีรอบที่เปิดใช้");
        break;
      }
    }
  }
}

void controlTask(void*) {
  for (;;) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    processRx();
    processCommands();
    checkNodesOnline();
    tickSchedule();
    tickRun();
    updatePump();
    sendToNodes();
    xSemaphoreGive(stateMutex);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ---------- MQTT ----------

void onTankMessage(const byte* payload, unsigned int len) {
  char buf[12];
  if (len == 0 || len >= sizeof(buf)) return;
  memcpy(buf, payload, len);
  buf[len] = '\0';
  char* end;
  float v = strtof(buf, &end);
  if (end == buf || *end != '\0' || v < 0 || v > 100) return;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (v != tankLevel) stateVersion++;
  tankLevel = v;
  tankUpdatedMs = millis();
  xSemaphoreGive(stateMutex);
}

void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  if (!strcmp(topic, TOPIC_TANK)) {
    onTankMessage(payload, len);
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;
  const char* action = doc["action"] | "";
  Command c = {};

  if (!strcmp(action, "start")) {
    int r = doc["round"] | 0;
    if (r < 0 || r >= ROUND_COUNT) return;
    c.type = CMD_START;
    c.round = r;
  } else if (!strcmp(action, "stop")) {
    c.type = CMD_STOP;
  } else if (!strcmp(action, "auto")) {
    if (!doc["enabled"].is<bool>()) return;
    c.type = CMD_AUTO;
    c.on = doc["enabled"];
  } else if (!strcmp(action, "valve")) {
    int v = doc["valve"] | -1;
    if (v < 0 || v >= TOTAL_VALVES || !doc["open"].is<bool>()) return;
    c.type = CMD_VALVE;
    c.valve = v;
    c.on = doc["open"];
  } else if (!strcmp(action, "pump")) {
    if (!doc["on"].is<bool>()) return;
    c.type = CMD_PUMP;
    c.on = doc["on"];
  } else if (!strcmp(action, "config")) {
    JsonArray rounds = doc["rounds"];
    if (rounds.size() != ROUND_COUNT) return;
    c.type = CMD_CONFIG;
    for (int r = 0; r < ROUND_COUNT; r++) {
      JsonObject o = rounds[r];
      int h = o["startHour"] | -1;
      int m = o["startMinute"] | -1;
      int dur = o["duration"] | -1;
      if (!o["enabled"].is<bool>() || h < 0 || h > 23 || m < 0 || m > 59) return;
      if (dur < 1 || dur > MAX_DURATION_MIN) return;
      Round& rd = c.cfg.rounds[r];
      rd.enabled = o["enabled"];
      rd.startHour = h;
      rd.startMinute = m;
      rd.durationMin = dur;
    }
  } else {
    return;
  }
  xQueueSend(cmdQueue, &c, 0);
}

void publishConfig() {
  JsonDocument d;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  d["autoEnabled"] = cfg.autoEnabled;
  JsonArray rounds = d["rounds"].to<JsonArray>();
  for (int r = 0; r < ROUND_COUNT; r++) {
    const Round& rd = cfg.rounds[r];
    JsonObject o = rounds.add<JsonObject>();
    o["enabled"] = rd.enabled;
    o["startHour"] = rd.startHour;
    o["startMinute"] = rd.startMinute;
    o["duration"] = rd.durationMin;
  }
  xSemaphoreGive(stateMutex);
  char buf[512];
  size_t n = serializeJson(d, buf, sizeof(buf));
  mqtt.publish(TOPIC_CONFIG, (const uint8_t*)buf, n, true);
}

void publishState() {
  JsonDocument d;
  struct tm t;
  bool synced = getLocalTime(&t, 0) && t.tm_year >= 120;
  char timeBuf[24] = "";
  if (synced) strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &t);

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  d["time"] = timeBuf;
  d["timeSynced"] = synced;
  d["uptime"] = millis() / 1000;
  d["rssi"] = WiFi.RSSI();
  d["pump"] = pumpOn;
  d["phase"] = PHASE_NAMES[phase];
  d["step"] = curStep;
  d["stepElapsedSec"] = phase == PHASE_RUNNING ? (millis() - stepStartMs) / 1000 : 0;
  d["stepDurationSec"] = curStep >= 0 ? stepMinutes(curStep) * 60 : 0;
  d["round"] = runRound;
  if (tankFresh()) d["tank"] = tankLevel;
  else d["tank"] = nullptr;
  d["tankMin"] = TANK_MIN_PERCENT;
  d["tankOk"] = tankOk();
  JsonArray arr = d["nodes"].to<JsonArray>();
  for (uint8_t id = FIRST_NODE_ID; id <= LAST_NODE_ID; id++) {
    const NodeState& n = nodes[id];
    JsonObject o = arr.add<JsonObject>();
    o["id"] = id;
    o["online"] = nodeOnline(id);
    o["lastSeenSec"] = n.seen ? (long)((millis() - n.lastSeenMs) / 1000) : -1;
    o["valves"] = n.reportedMask;
    o["desired"] = n.desiredMask;
    o["valveCount"] = NODE_VALVE_COUNT[id];
    o["ip"] = n.seen ? n.ip.toString() : "";
    o["rssi"] = n.rssi;
    o["uptime"] = n.uptimeSec;
  }
  xSemaphoreGive(stateMutex);

  char buf[900];
  size_t len = serializeJson(d, buf, sizeof(buf));
  mqtt.publish(TOPIC_STATE, (const uint8_t*)buf, len, true);
}

// ---------- Log upload (loop task only) ----------

PendingLog pendingLogs[LOG_BUFFER_SIZE];
int pendingLogCount = 0;
uint32_t logSeq = 0;

void queueLogUpload(const LogMsg& m) {
  if (!LOG_URL[0] || !LOG_TOKEN[0]) return;
  if (pendingLogCount == LOG_BUFFER_SIZE) {
    memmove(pendingLogs, pendingLogs + 1, sizeof(PendingLog) * (LOG_BUFFER_SIZE - 1));
    pendingLogCount--;
  }
  PendingLog& p = pendingLogs[pendingLogCount++];
  time_t now = time(nullptr);
  // 0 = clock not synced yet, the server uses its receive time instead.
  p.time = now > 1600000000 ? (uint32_t)now : 0;
  p.seq = ++logSeq;
  strlcpy(p.text, m.text, sizeof(p.text));
}

void uploadLogs() {
  static uint32_t lastAttemptMs = 0;
  if (!pendingLogCount || WiFi.status() != WL_CONNECTED) return;
  // TLS certificate checks fail until NTP has set the clock.
  if (time(nullptr) < 1600000000) return;
  if (lastAttemptMs && millis() - lastAttemptMs < LOG_UPLOAD_INTERVAL_MS) return;
  lastAttemptMs = millis();

  int count = pendingLogCount;
  JsonDocument d;
  JsonArray arr = d["logs"].to<JsonArray>();
  for (int i = 0; i < count; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["t"] = pendingLogs[i].time;
    o["s"] = pendingLogs[i].seq;
    o["m"] = pendingLogs[i].text;
  }
  String body;
  serializeJson(d, body);

  WiFiClientSecure client;
  client.setCACert(GTS_ROOT_R1);
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, LOG_URL)) return;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-log-token", LOG_TOKEN);
  int code = http.POST(body);
  http.end();
  if (code < 200 || code >= 300) {
    Serial.printf("Log upload failed: %d\n", code);
    return;
  }
  pendingLogCount -= count;
  memmove(pendingLogs, pendingLogs + count, sizeof(PendingLog) * pendingLogCount);
}

void connectMqtt() {
  String clientId = "phrajd-master-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.println("Connecting MQTT...");
  if (mqtt.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD, TOPIC_MASTER, 1, true, "offline")) {
    Serial.println("MQTT connected");
    mqtt.publish(TOPIC_MASTER, "online", true);
    mqtt.subscribe(TOPIC_CMD, 1);
    mqtt.subscribe(TOPIC_TANK, 1);
    publishConfig();
    publishState();
  } else {
    Serial.printf("MQTT failed, rc=%d\n", mqtt.state());
  }
}

// ---------- Setup / loop ----------

void setup() {
  digitalWrite(PUMP_RELAY_PIN, RELAY_OFF);
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== PhraJD Watering - Master (board 1) ===");

  prefs.begin("watering", false);
  loadConfig();

  cmdQueue = xQueueCreate(8, sizeof(Command));
  logQueue = xQueueCreate(16, sizeof(LogMsg));
  stateMutex = xSemaphoreCreateMutex();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(250);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected, IP %s, channel %d\n", WiFi.localIP().toString().c_str(), WiFi.channel());
  } else {
    Serial.println("WiFi not connected yet, will keep retrying");
  }
  configTzTime("ICT-7", "pool.ntp.org", "time.google.com");

  tlsClient.setCACert(ROOT_CA);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(1024);

  // Control runs in its own task so slow MQTT/TLS reconnects never delay the pump interlock.
  xTaskCreatePinnedToCore(controlTask, "control", 8192, nullptr, 2, nullptr, 1);
}

void loop() {
  static uint32_t lastMqttAttemptMs = 0;
  static uint32_t lastStatePubMs = 0;
  static uint32_t publishedStateVersion = 0;
  static uint32_t publishedConfigVersion = 0;

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) {
      if (lastMqttAttemptMs == 0 || millis() - lastMqttAttemptMs >= 5000) {
        lastMqttAttemptMs = millis();
        connectMqtt();
      }
    } else {
      mqtt.loop();
    }
  }

  if (mqtt.connected()) {
    if (configVersion != publishedConfigVersion) {
      publishedConfigVersion = configVersion;
      publishConfig();
    }
    if (stateVersion != publishedStateVersion || millis() - lastStatePubMs >= STATE_HEARTBEAT_MS) {
      publishedStateVersion = stateVersion;
      lastStatePubMs = millis();
      publishState();
    }
  }

  LogMsg m;
  while (xQueueReceive(logQueue, &m, 0) == pdTRUE) {
    if (mqtt.connected()) mqtt.publish(TOPIC_LOG, m.text);
    queueLogUpload(m);
  }
  uploadLogs();
  delay(10);
}
