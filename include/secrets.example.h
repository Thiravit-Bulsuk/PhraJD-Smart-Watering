#pragma once
// คัดลอกไฟล์นี้เป็น include/secrets.h แล้วใส่ค่าจริง (secrets.h ไม่ถูก commit ขึ้น git)

#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"

#define MQTT_HOST "your-deployment.ala.us-east-1.emqxsl.com"
#define MQTT_PORT 8883
#define MQTT_USERNAME "your-mqtt-user"
#define MQTT_PASSWORD "your-mqtt-password"

// ส่ง log ไปเก็บที่เว็บแอป (ต้องตรงกับ LOG_TOKEN ใน dashboard/.env) เว้นว่างไว้ถ้าไม่ใช้
#define LOG_URL "https://your-service.a.run.app/api/logs"
#define LOG_TOKEN ""
