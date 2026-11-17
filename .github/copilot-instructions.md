# PhraJD Smart Watering — Copilot instructions

ตอบผู้ใช้เป็นภาษาไทย

## Architecture
- `src/master` (ESP32 บอร์ด 1, ปั๊ม) ⇄ UDP port 4210 บน LAN ⇄ `src/node` (บอร์ด 2-4, วาล์ว 1-7)
- บอร์ดหลักเชื่อม EMQX Serverless `a7821161.ala.us-east-1.emqxsl.com` (MQTT/TLS 8883, Root CA DigiCert Global Root G2) topic `phrajd/watering/{master,state,config,log,cmd}` — `master`/`state`/`config` เป็น retained
- `dashboard/` = Express บน Cloud Run (project `watdoi-monitoring`, region `asia-southeast1`, service `phrajd-watering`)

## Cost constraint (สำคัญ)
- Cloud Run ต้องเป็น `--min-instances=0 --cpu-throttling` เพื่อให้อยู่ใน free tier ห้ามให้เซิร์ฟเวอร์ถือการเชื่อมต่อ MQTT ค้างไว้ หรือให้หน้าเว็บ poll เซิร์ฟเวอร์
- เบราว์เซอร์อ่านสถานะจาก EMQX โดยตรงผ่าน WebSocket (`wss://<host>:8084/mqtt`) ด้วยบัญชี **Subscribe only** (`MQTT_VIEW_USERNAME`/`MQTT_VIEW_PASSWORD`, ส่งให้เบราว์เซอร์ผ่าน `GET /api/mqtt`)
- คำสั่งควบคุมส่งผ่าน `POST /api/command` (ตรวจ header `x-control-password`) แล้วเซิร์ฟเวอร์เชื่อม MQTT เฉพาะตอน publish
- บันทึกเหตุการณ์: บอร์ดหลักรวบ log แล้ว `POST /api/logs` (header `x-log-token` = `LOG_TOKEN`) ทุก 1 นาที เก็บใน Firestore `(default)` collection `wateringLogs` วันละ 1 document (ลบเองด้วย TTL `expireAt` หลัง 3 วัน) — `GET /api/logs` อ่าน 4 document และ cache 30 วินาทีเพื่อให้อยู่ในโควตาฟรี
- บอร์ดหลักไม่เปิดปั๊มถ้าระดับน้ำแท็งก์เจดีย์ (topic `JD` จากระบบวัดระดับน้ำ, retained 0-100) ต่ำกว่า 40% หรือไม่ได้ข้อมูลเกิน 15 นาที

## Commands
- เว็บ: `cd dashboard && npm install && npm run dev` (ใช้ `.env`), deploy: `./dashboard/deploy.sh`
- เฟิร์มแวร์: `pio run -e master|node_2|node_3|node_4 -t upload`
