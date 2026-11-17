// บอร์ด 2-4: รับคำสั่งเปิด/ปิดวาล์วจากบอร์ดหลักผ่านเราท์เตอร์ Wi-Fi (UDP)
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include "BoardConfig.h"
#include "Protocol.h"
#include "secrets.h"

constexpr uint32_t MASTER_LOST_MS = 30000;
constexpr uint32_t WIFI_RECONNECT_MS = 30000;
constexpr uint8_t VALID_MASK = (1 << VALVE_COUNT) - 1;

WiFiUDP udp;
bool udpStarted = false;
IPAddress masterIp;
uint8_t valveMask = 0;
uint32_t leaseStartMs = 0;
uint32_t leaseMs = 0;
uint32_t lastMasterMs = 0;
uint32_t lastStatusMs = 0;

void applyValves(uint8_t mask) {
  mask &= VALID_MASK;
  for (uint8_t i = 0; i < VALVE_COUNT; i++) {
    digitalWrite(VALVE_PINS[i], (mask >> i) & 1 ? RELAY_ON : RELAY_OFF);
  }
  if (mask != valveMask) Serial.printf("Valves: 0x%02X -> 0x%02X\n", valveMask, mask);
  valveMask = mask;
}

void sendStatus() {
  lastStatusMs = millis();
  if (!udpStarted || WiFi.status() != WL_CONNECTED) return;
  NetPacket p = {};
  p.magic = NET_MAGIC;
  p.type = MSG_STATUS;
  p.src = NODE_ID;
  p.dst = MASTER_ID;
  p.valveMask = valveMask;
  p.uptimeSec = millis() / 1000;
  p.rssi = WiFi.RSSI();
  // Broadcast until the master's IP is learned from its commands.
  bool masterKnown = masterIp != IPAddress() && millis() - lastMasterMs < MASTER_LOST_MS;
  udp.beginPacket(masterKnown ? masterIp : WiFi.broadcastIP(), UDP_PORT);
  udp.write((const uint8_t*)&p, sizeof(p));
  udp.endPacket();
}

void processRx() {
  int size;
  while ((size = udp.parsePacket()) > 0) {
    NetPacket p;
    if (size != sizeof(p) || udp.read((uint8_t*)&p, sizeof(p)) != sizeof(p)) {
      udp.flush();
      continue;
    }
    if (p.magic != NET_MAGIC || p.type != MSG_SET_VALVES || p.src != MASTER_ID || p.dst != NODE_ID) continue;
    masterIp = udp.remoteIP();
    lastMasterMs = millis();
    applyValves(p.valveMask);
    leaseStartMs = millis();
    leaseMs = p.leaseSec * 1000UL;
    sendStatus();
  }
}

void maintainWiFi() {
  static uint32_t disconnectedSinceMs = 0;
  if (WiFi.status() == WL_CONNECTED) {
    disconnectedSinceMs = 0;
    if (!udpStarted) {
      udpStarted = udp.begin(UDP_PORT);
      Serial.printf("WiFi connected, IP %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    return;
  }
  if (disconnectedSinceMs == 0) disconnectedSinceMs = millis();
  if (millis() - disconnectedSinceMs >= WIFI_RECONNECT_MS) {
    Serial.println("WiFi lost, reconnecting");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    disconnectedSinceMs = millis();
  }
}

void setup() {
  for (uint8_t i = 0; i < VALVE_COUNT; i++) {
    digitalWrite(VALVE_PINS[i], RELAY_OFF);
    pinMode(VALVE_PINS[i], OUTPUT);
  }
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n=== PhraJD Watering - Valve node %d (%d valves) ===\n", NODE_ID, VALVE_COUNT);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void loop() {
  maintainWiFi();
  if (udpStarted) processRx();

  if (valveMask && millis() - leaseStartMs >= leaseMs) {
    Serial.println("No command from master, closing all valves (lease expired)");
    applyValves(0);
    sendStatus();
  }

  if (millis() - lastStatusMs >= NODE_STATUS_INTERVAL_MS + NODE_ID * 137) sendStatus();
  delay(5);
}
