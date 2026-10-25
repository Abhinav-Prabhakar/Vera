# Vera node — ESP8266 firmware

One node runs one deep-water-culture bed. It reads every sensor the dashboard shows, drives the bed's equipment through a relay board, and keeps a **complete local controller** so the bed stays safe when the network or the dashboard goes away.

```
firmware/vera-node/
├── platformio.ini            PlatformIO project (NodeMCU v2 / ESP-12E)
├── vera-node.ino             Arduino IDE entry point
└── src/
    ├── main.cpp              the firmware
    └── vera_config.example.h copy to vera_config.h and fill in
```

## Bill of materials

| Part | Role | Bus / pin |
|---|---|---|
| NodeMCU v2 (ESP-12E, ESP8266) | controller | — |
| ADS1115 16-bit ADC | analog probes (the ESP8266 has only one ADC) | I²C `0x48` |
| DFRobot Gravity pH v2 (SEN0161-V2) | solution pH | ADS1115 A0 |
| DFRobot Gravity TDS/EC (SEN0244) | EC, temperature-compensated | ADS1115 A1 |
| DFRobot Gravity analog DO (SEN0237) | dissolved oxygen | ADS1115 A2 |
| 0.5–4.5 V submersible pressure transducer | reservoir level | ADS1115 A3 |
| SHT31 | air temperature + RH (→ VPD) | I²C `0x44` |
| BH1750 | light (→ PAR estimate) | I²C `0x23` |
| DS18B20 (waterproof) + 4.7 kΩ pull-up | solution temperature | GPIO13 · D7 |
| PCF8574 + 8-channel relay board (active-low) | equipment | I²C `0x20` |
| 0–10 V PWM converter | dimmable LED drivers | GPIO14 · D5 |
| 3 × 12 V peristaltic pumps | nutrient A, nutrient B, pH-down | relays 5–7 |

**I²C:** SDA = GPIO4 (D2), SCL = GPIO5 (D1).

## Relay map

| Channel | Equipment | Local automation |
|---|---|---|
| 0 | Grow lights (+ PWM dimming) | photoperiod 05:00–23:00 with 30 min ramps, via NTP |
| 1 | Recirculation pump | always on |
| 2 | Air pump (aeration) | always on |
| 3 | Solution chiller | hysteresis ±0.5 °C around the setpoint |
| 4 | Feed valve | open |
| 5 | Nutrient A pump | EC below target − 0.10 → dose A and B equally |
| 6 | Nutrient B pump | 〃 |
| 7 | pH-down pump | pH above target + 0.15 → dose acid |

### Dosing safety interlocks
- 15 min mixing gap between doses, 60 mL per pump per hour hard cap
- never doses into a reservoir below 20 %, or while the recirculation pump is off
- pumps run on non-blocking timers, so the controller keeps sensing while dosing

### Comms-loss failsafe
Manual overrides sent from the dashboard expire 120 s after the last message from any dashboard; the node then returns every control to automation and reports a `failsafe` event.

## Build and flash

```bash
cd firmware/vera-node
cp src/vera_config.example.h src/vera_config.h   # add Wi-Fi + broker
pio run -t upload && pio device monitor
```

Verified build (PlatformIO, espressif8266 4.2.1, NodeMCU v2): **RAM 41.6 %** · **flash 45.3 %**.

Arduino IDE also works: open `vera-node.ino` and install the libraries listed in `platformio.ini`.

## Calibration (serial console, 115200 baud)

| Command | Do this first |
|---|---|
| `cal ph 7` / `cal ph 4` | probe rinsed and settled in the pH 7.00, then 4.00, buffer |
| `cal ec 1.413` | probe in a 1413 µS/cm standard |
| `cal do` | probe in air-saturated water |
| `cal level empty` / `cal level full` | reservoir at empty, then full |
| `status` · `dose ph 5` · `dose ec 10` · `reset cal` | — |

Calibration is stored in emulated EEPROM with a CRC, so a corrupt write falls back to defaults instead of producing garbage readings.

## Protocol

The same JSON travels over the LAN WebSocket (`ws://vera-node.local:81`) and over MQTT (`<prefix>/telemetry`, `/event`, `/ack` out; `<prefix>/cmd` in; retained `<prefix>/status` = `online`/`offline` as a last-will).

**Telemetry**, every 2 s:
```json
{ "type": "telemetry", "node": "vera-node-01", "bed": 0, "fw": "1.0.0", "uptime": 86012, "rssi": -61,
  "airTemp": 22.41, "humidity": 68.2, "vpd": 0.862, "waterTemp": 20.38, "ph": 5.874, "ec": 1.861,
  "do2": 8.42, "level": 81.5, "par": 412,
  "controls": { "lights": { "mode": "auto", "on": true, "level": 100 }, "phc": { "mode": "auto", "on": true, "level": 100, "target": 5.8 } } }
```

**Commands** from the dashboard:
```json
{ "type": "set",  "scope": "b0", "control": "lights", "mode": "manual", "level": 60, "on": true, "ref": "ui-12" }
{ "type": "set",  "scope": "b0", "control": "chiller", "set": 18.5 }
{ "type": "dose", "scope": "b0", "control": "phc", "ml": 5 }
{ "type": "auto_all" }
```
Every command is answered with `{ "type": "ack", "ref": "ui-12", "ok": true }`, or `ok: false` plus a `reason` (for example a dosing lockout).

## Try it without hardware

```bash
node tools/mock-node.mjs          # ws://localhost:8181, zero dependencies
npx serve .                       # open Vera over http, link icon → LAN → ws://localhost:8181
```

> GitHub Pages is served over HTTPS, and browsers block `ws://` from HTTPS pages. Use the MQTT link (`wss://`) from the hosted site, or open Vera over `http://` on your LAN for the direct WebSocket link.
