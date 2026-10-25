// Copy this file to vera_config.h and fill in your network + broker details.
// vera_config.h is git-ignored so credentials never leave your machine.
#pragma once

// ---------- network ----------
#define WIFI_SSID        "your-ssid"
#define WIFI_PASSWORD    "your-password"
#define NODE_HOSTNAME    "vera-node"          // reachable as vera-node.local
#define NODE_ID          "vera-node-01"
#define NODE_BED         0                     // which bed this node serves (0 = DWC-01)

// ---------- dashboard links (either or both) ----------
// LAN: the dashboard connects straight to ws://vera-node.local:81
#define ENABLE_WEBSOCKET 1
#define WS_PORT          81

// Cloud: MQTT broker that the dashboard reaches over secure WebSockets.
// Works from the GitHub Pages build (HTTPS) because the browser side is wss.
#define ENABLE_MQTT      1
#define MQTT_HOST        "broker.hivemq.com"
#define MQTT_PORT        8883                  // 8883 = TLS, 1883 = plain
#define MQTT_TLS         1
#define MQTT_USER        ""
#define MQTT_PASSWORD    ""
#define MQTT_PREFIX      "vera/bay07"          // topics: <prefix>/telemetry, /cmd, /status, /ack

// ---------- time (photoperiod runs locally if the link drops) ----------
#define NTP_SERVER       "pool.ntp.org"
#define TZ_INFO          "IST-5:30"            // POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"
#define LIGHTS_ON_HOUR   5
#define LIGHTS_OFF_HOUR  23

// ---------- targets for the local controller ----------
#define PH_TARGET        5.80f
#define PH_DEADBAND      0.15f
#define EC_TARGET        1.85f                 // mS/cm
#define EC_DEADBAND      0.10f
#define WATER_TEMP_SET   20.0f                 // °C, chiller hysteresis ±0.5

// ---------- dosing pumps (calibrate: run 60 s into a cylinder, mL/60 = rate) ----------
#define PUMP_ML_PER_SEC  1.10f
#define DOSE_PH_ML       5.0f
#define DOSE_EC_ML       20.0f                 // per part (A and B are dosed equally)
#define DOSE_MIN_GAP_S   900                   // let the reservoir mix before re-dosing
#define DOSE_MAX_ML_HOUR 60.0f                 // hard safety cap per pump
#define LEVEL_MIN_DOSING 20.0f                 // % — never dose into a near-empty tank

// ---------- comms-loss failsafe ----------
#define LINK_TIMEOUT_S   120                   // manual overrides expire if the dashboard vanishes
