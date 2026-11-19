# ระบบรดน้ำต้นไม้ PhraJD

ระบบรดน้ำอัตโนมัติด้วย ESP32 4 บอร์ด สื่อสารกันผ่านเราท์เตอร์ Wi-Fi (UDP ภายในวง LAN) และควบคุม/ดูสถานะผ่านเว็บแอปบน Google Cloud Run

**เว็บแอป:** https://phrajd-watering-tgtmz75xwa-as.a.run.app

<img src="qrcode-webapp.png" alt="QR code เว็บแอป" width="180">

**ที่เก็บโค้ด (repository):** https://github.com/Thiravit-Bulsuk/PhraJD-Smart-Watering

<img src="docs/repo-qr.png" alt="QR code repository" width="180">

```
เบราว์เซอร์ ⇄ EMQX MQTT ⇄ บอร์ด 1 (บอร์ดหลัก + รีเลย์ปั๊ม)
                                         │ UDP port 4210 ผ่านเราท์เตอร์ "PhraJD"
                     ┌───────────────────┼───────────────────┐
              บอร์ด 2 (วาล์ว 1-4)   บอร์ด 3 (วาล์ว 5)   บอร์ด 4 (วาล์ว 6-7)
```

เบราว์เซอร์อ่านสถานะจาก EMQX โดยตรงผ่าน WebSocket (`wss://...:8084/mqtt`) ด้วยบัญชี MQTT ที่**สมัครรับข้อมูลได้อย่างเดียว** ส่วนคำสั่งควบคุมส่งผ่าน Cloud Run (ตรวจรหัส CONTROL_PASSWORD ก่อน) ซึ่งเชื่อมต่อ MQTT เฉพาะตอนส่งคำสั่ง ทำให้ Cloud Run ปิดตัวเองเมื่อไม่มีคนใช้ (min-instances=0) และอยู่ใน free tier

## การทำงาน

- ตั้งได้วันละ **4 รอบ** แต่ละรอบมีเวลาเริ่มและเวลาเปิด "วาล์วละ X นาที" ค่าเดียวใช้กับทุกวาล์ว (ค่าเริ่มต้น: รอบ 1 เวลา 02:00 เปิดใช้, รอบ 2-4 ปิดอยู่) เปิดวาล์ว 1 → 7 ทีละตัว
- เปิด/ปิดการตั้งเวลาอัตโนมัติทั้งหมดได้จากกล่องแรกในเว็บ
- **ปั๊มเปิดได้เฉพาะเมื่อมีวาล์วที่ยืนยันว่าเปิดแล้วอย่างน้อย 1 ตัว** และถูกตัดทันทีถ้าไม่มีวาล์วเปิด/บอร์ดวาล์วขาดการติดต่อ
- ตอนสลับวาล์ว: เปิดวาล์วถัดไปและรอยืนยันก่อน แล้วจึงปิดวาล์วเดิม (ปั๊มมีทางน้ำออกตลอด)
- จบรอบ: ปิดปั๊มก่อน 5 วินาทีแล้วจึงปิดวาล์ว
- บอร์ดวาล์วจะปิดวาล์วเองถ้าไม่ได้รับคำสั่งจากบอร์ดหลักเกิน 120 วินาที
- วาล์วที่ไม่ตอบสนองภายใน 60 วินาทีจะถูกข้าม
- โหมดทดสอบบนเว็บ: เปิด/ปิดวาล์วและปั๊มได้ทันที (ปิดทั้งหมดเองถ้าไม่มีการกดเกิน 30 นาที)

## การต่อสาย (ESP32 DevKit)

| บอร์ด | env | ขารีเลย์ |
|---|---|---|
| 1 บอร์ดหลัก | `master` | ปั๊ม GPIO26 |
| 2 | `node_2` | วาล์ว 1-4: GPIO26, 27, 25, 33 |
| 3 | `node_3` | วาล์ว 5: GPIO26 |
| 4 | `node_4` | วาล์ว 6: GPIO26, วาล์ว 7: GPIO27 |

แก้ขาได้ที่ [include/BoardConfig.h](include/BoardConfig.h) — ถ้าโมดูลรีเลย์เป็นแบบ Active HIGH ให้ตั้ง `RELAY_ACTIVE_LOW=0` ใน [platformio.ini](platformio.ini)

## อัปโหลดเฟิร์มแวร์

```bash
cp include/secrets.example.h include/secrets.h   # แล้วใส่รหัส Wi-Fi / MQTT
pio run -e master -t upload
pio run -e node_2 -t upload
pio run -e node_3 -t upload
pio run -e node_4 -t upload
pio device monitor
```

ทุกบอร์ดต่อ Wi-Fi `PhraJD` บอร์ด 2-4 ส่งสถานะแบบ broadcast จนกว่าจะรู้ IP ของบอร์ดหลัก จากนั้นสื่อสารแบบ unicast ไม่ต้องตั้ง IP คงที่ และไม่ต้องใช้อินเทอร์เน็ต (ใช้เฉพาะเว็บแอปและเวลา NTP)

ถ้าบอร์ดใดอยู่ไกลจากเราท์เตอร์ ให้เพิ่ม Wi-Fi repeater/mesh ที่ใช้ชื่อ `PhraJD` เดียวกัน (หน้าเว็บแสดงความแรงสัญญาณ Wi-Fi ของแต่ละบอร์ด)

## เว็บแอป

```bash
cd dashboard
cp .env.example .env     # ใส่ MQTT, บัญชี MQTT ดูอย่างเดียว และ CONTROL_PASSWORD (รหัสสำหรับกดควบคุมบนเว็บ)
npm install
npm run dev              # http://localhost:4173
./deploy.sh              # deploy ไป Cloud Run (project watdoi-monitoring, service phrajd-watering)
```

บัญชี `MQTT_VIEW_USERNAME` ต้องสร้างใน EMQX Cloud Console → Access Control → Authentication แล้วตั้ง Authorization ให้ **subscribe ได้อย่างเดียว** (รหัสนี้ถูกส่งไปที่เบราว์เซอร์ ห้ามใช้บัญชีที่ publish ได้) บันทึกเหตุการณ์จะเก็บไว้ในเบราว์เซอร์ (50 รายการล่าสุด) เฉพาะช่วงที่เปิดหน้าเว็บอยู่

## MQTT topics (`phrajd/watering/...`)

| topic | ทิศทาง | ข้อมูล |
|---|---|---|
| `master` | บอร์ด → เว็บ | `online` / `offline` (retained, LWT) |
| `state` | บอร์ด → เว็บ | JSON สถานะปั๊ม วาล์ว บอร์ด (retained) |
| `config` | บอร์ด → เว็บ | `{autoEnabled, rounds: [{enabled, startHour, startMinute, duration} x4]}` (retained) |
| `log` | บอร์ด → เว็บ | ข้อความเหตุการณ์ |
| `cmd` | เว็บ → บอร์ด | `{action: start\|stop\|auto\|config\|valve\|pump, ...}` |
