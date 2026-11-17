// ทดสอบสัญญาณ Wi-Fi ณ จุดติดตั้ง (เช่น บอร์ด 4) ดูผลทาง Serial Monitor 115200
#include <Arduino.h>
#include <WiFi.h>

#include "BoardConfig.h"
#include "secrets.h"

constexpr uint32_t REPORT_INTERVAL_MS = 1000;
constexpr uint32_t SUMMARY_INTERVAL_MS = 30000;
constexpr uint32_t SCAN_INTERVAL_MS = 60000;

int32_t rssiMin = 0, rssiMax = -127;
int64_t rssiSum = 0;
uint32_t rssiCount = 0;
uint32_t disconnectCount = 0;
uint32_t connectedSinceMs = 0;
bool wasConnected = false;

const char* quality(int32_t rssi) {
  if (rssi >= -60) return "ดีมาก";
  if (rssi >= -67) return "ดี";
  if (rssi >= -75) return "พอใช้ (เริ่มไม่เสถียร)";
  if (rssi >= -82) return "อ่อน (หลุดบ่อย)";
  return "แย่มาก (ใช้งานไม่ได้)";
}

void scanRouter() {
  Serial.printf("\n--- สแกนหา \"%s\" ---\n", WIFI_SSID);
  int n = WiFi.scanNetworks(false, true);
  int found = 0;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) != WIFI_SSID) continue;
    found++;
    Serial.printf("  AP %s  ช่อง %2d  RSSI %4d dBm  %s\n", WiFi.BSSIDstr(i).c_str(), WiFi.channel(i), WiFi.RSSI(i),
                  quality(WiFi.RSSI(i)));
  }
  if (!found) Serial.println("  ไม่พบเราท์เตอร์");
  WiFi.scanDelete();
  Serial.println();
}

void printSummary() {
  if (!rssiCount) return;
  Serial.printf("\n=== สรุป %lu ตัวอย่าง: เฉลี่ย %ld dBm (%s)  ต่ำสุด %ld  สูงสุด %ld  หลุด %lu ครั้ง ===\n\n",
                (unsigned long)rssiCount, (long)(rssiSum / rssiCount), quality(rssiSum / rssiCount), (long)rssiMin,
                (long)rssiMax, (unsigned long)disconnectCount);
}

void setup() {
#if NODE_ID != 1
  for (uint8_t i = 0; i < VALVE_COUNT; i++) {
    digitalWrite(VALVE_PINS[i], RELAY_OFF);
    pinMode(VALVE_PINS[i], OUTPUT);
  }
#endif
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== PhraJD Wi-Fi signal test ===");
  Serial.println("เกณฑ์: >= -67 dBm ดี, -68..-75 พอใช้, < -75 ควรเพิ่ม repeater หรือเปลี่ยนเสาอากาศ");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  scanRouter();
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void loop() {
  static uint32_t lastReportMs = 0, lastSummaryMs = 0, lastScanMs = millis();
  uint32_t now = millis();
  bool connected = WiFi.status() == WL_CONNECTED;

  if (connected && !wasConnected) {
    connectedSinceMs = now;
    Serial.printf("เชื่อมต่อแล้ว IP %s  Gateway %s  AP %s  ช่อง %d\n", WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(), WiFi.BSSIDstr().c_str(), WiFi.channel());
  } else if (!connected && wasConnected) {
    disconnectCount++;
    Serial.printf("!!! Wi-Fi หลุด (ครั้งที่ %lu)\n", (unsigned long)disconnectCount);
  }
  wasConnected = connected;

  if (now - lastReportMs >= REPORT_INTERVAL_MS) {
    lastReportMs = now;
    if (connected) {
      int32_t rssi = WiFi.RSSI();
      rssiMin = min(rssiMin, rssi);
      rssiMax = max(rssiMax, rssi);
      rssiSum += rssi;
      rssiCount++;
      Serial.printf("RSSI %4ld dBm  %-22s  ต่อเนื่อง %lus\n", (long)rssi, quality(rssi),
                    (unsigned long)((now - connectedSinceMs) / 1000));
    } else {
      Serial.println("กำลังเชื่อมต่อ...");
    }
  }

  if (now - lastSummaryMs >= SUMMARY_INTERVAL_MS) {
    lastSummaryMs = now;
    printSummary();
  }

  // Scanning briefly interrupts the connection, so only do it occasionally.
  if (now - lastScanMs >= SCAN_INTERVAL_MS) {
    lastScanMs = now;
    scanRouter();
  }
  delay(20);
}
