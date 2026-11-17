#pragma once
#include <stdint.h>

// All boards join the same Wi-Fi router and talk over UDP on the local network (no internet needed).
constexpr uint16_t UDP_PORT = 4210;
constexpr uint16_t NET_MAGIC = 0xA760;
constexpr uint8_t MASTER_ID = 1;
constexpr uint8_t FIRST_NODE_ID = 2;
constexpr uint8_t LAST_NODE_ID = 4;

// Valve count per node id (index 0 and 1 unused).
constexpr uint8_t NODE_VALVE_COUNT[LAST_NODE_ID + 1] = {0, 0, 4, 1, 2};

// Node closes all valves if the master stops refreshing the command for this long.
constexpr uint16_t NODE_LEASE_SEC = 120;
constexpr uint32_t NODE_STATUS_INTERVAL_MS = 5000;
// Master treats a node as offline (and its valves as NOT open) after this long without status.
constexpr uint32_t NODE_OFFLINE_MS = 20000;

enum MsgType : uint8_t {
  MSG_SET_VALVES = 1,  // master -> node: valveMask = valves that must be open
  MSG_STATUS = 2,      // node -> master: valveMask = valves actually open
};

struct __attribute__((packed)) NetPacket {
  uint16_t magic;
  uint8_t type;
  uint8_t src;
  uint8_t dst;
  uint8_t valveMask;
  uint16_t leaseSec;
  uint32_t uptimeSec;
  int8_t rssi;
};
