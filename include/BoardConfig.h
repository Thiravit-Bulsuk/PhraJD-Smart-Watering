#pragma once
#include <Arduino.h>
#include "Protocol.h"

#ifndef RELAY_ACTIVE_LOW
#define RELAY_ACTIVE_LOW 1
#endif

constexpr uint8_t RELAY_ON = RELAY_ACTIVE_LOW ? LOW : HIGH;
constexpr uint8_t RELAY_OFF = RELAY_ACTIVE_LOW ? HIGH : LOW;

#if NODE_ID == 1
constexpr uint8_t PUMP_RELAY_PIN = 26;
#elif NODE_ID == 2
constexpr uint8_t VALVE_PINS[] = {26, 27, 25, 33};
#elif NODE_ID == 3
constexpr uint8_t VALVE_PINS[] = {26};
#elif NODE_ID == 4
constexpr uint8_t VALVE_PINS[] = {26, 27};
#else
#error "NODE_ID must be 1-4"
#endif

#if NODE_ID != 1
constexpr uint8_t VALVE_COUNT = sizeof(VALVE_PINS);
static_assert(VALVE_COUNT == NODE_VALVE_COUNT[NODE_ID], "VALVE_PINS does not match NODE_VALVE_COUNT");
#endif
