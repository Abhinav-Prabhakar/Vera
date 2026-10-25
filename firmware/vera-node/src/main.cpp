/*
 * Vera node — ESP8266 controller for one deep-water-culture bed.
 *
 *  Sensors  : SHT31 (air °C / RH) · DS18B20 (solution °C) · BH1750 (light → PAR)
 *             ADS1115 → pH probe · EC/TDS probe · dissolved-O₂ probe · level transducer
 *  Actuators: PCF8574 8-relay board (lights, pump, air, chiller, valve, 3 dosing pumps)
 *             + PWM → 0-10 V module for dimmable LED drivers
 *  Links    : WebSocket server on :81 (LAN) and/or MQTT over TLS (cloud).
 *             Both carry the same JSON protocol the Vera dashboard speaks.
 *
 *  The node is the source of truth for safety: it keeps a full local controller
 *  (photoperiod, aeration, chiller, pH/EC dosing with lockouts) and only takes
 *  manual overrides from the dashboard — which expire if the dashboard vanishes.
 */
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <WiFiClientSecure.h>
#include <EEPROM.h>
#include <Wire.h>
#include <time.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WebSocketsServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_SHT31.h>
#include <BH1750.h>
#include <PCF8574.h>

#if __has_include("vera_config.h")
  #include "vera_config.h"
#else
  #warning "vera_config.h not found — using vera_config.example.h (copy it and add your Wi-Fi details)"
  #include "vera_config.example.h"
#endif

#ifndef VERA_FW_VERSION
  #define VERA_FW_VERSION "1.0.0"
#endif

// ---------------------------------------------------------------- pins
// NodeMCU v2: D1 = GPIO5 (SCL), D2 = GPIO4 (SDA), D5 = GPIO14, D7 = GPIO13
static const uint8_t PIN_SDA      = 4;
static const uint8_t PIN_SCL      = 5;
static const uint8_t PIN_ONEWIRE  = 13;
static const uint8_t PIN_LED_PWM  = 14;     // → 0-10 V dimming module
static const uint8_t PIN_STATUS   = LED_BUILTIN;

// relay channels on the PCF8574 (active-low boards)
enum Relay : uint8_t { R_LIGHTS = 0, R_PUMP, R_AIR, R_CHILLER, R_VALVE, R_DOSE_A, R_DOSE_B, R_DOSE_PH, R_COUNT };
// ADS1115 channels
enum Adc : uint8_t { A_PH = 0, A_EC = 1, A_DO = 2, A_LEVEL = 3 };

// ---------------------------------------------------------------- devices
Adafruit_ADS1115 ads;
Adafruit_SHT31   sht;
BH1750           lux;
PCF8574          relays(0x20);
OneWire          oneWire(PIN_ONEWIRE);
DallasTemperature waterProbe(&oneWire);

#if ENABLE_WEBSOCKET
WebSocketsServer ws(WS_PORT);
#endif
#if ENABLE_MQTT
  #if MQTT_TLS
WiFiClientSecure netClient;
  #else
WiFiClient       netClient;
  #endif
PubSubClient     mqtt(netClient);
#endif

// ---------------------------------------------------------------- calibration (EEPROM)
struct Calibration {
  uint32_t magic;
  float phV7, phV4;          // probe voltage in pH 7.00 and pH 4.00 buffers
  float ecK;                 // EC cell constant multiplier
  float doVsat; float doTcal;// DO probe voltage in air-saturated water at doTcal °C
  float levelV0, levelV100;  // transducer voltage at empty / full
  float parPerLux;           // µmol·m⁻²·s⁻¹ per lux for this light mix
  uint32_t crc;
};
static Calibration cal;
static const uint32_t CAL_MAGIC = 0x56455241; // "VERA"

static uint32_t crc32(const uint8_t *d, size_t n){
  uint32_t c = 0xFFFFFFFF;
  for (size_t i = 0; i < n; i++){ c ^= d[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & -(int32_t)(c & 1)); }
  return ~c;
}
static void calDefaults(){
  cal = { CAL_MAGIC, 1.500f, 2.032f, 1.0f, 1.600f, 25.0f, 0.50f, 4.20f, 0.0185f, 0 };
}
static void calSave(){
  cal.crc = crc32((const uint8_t *)&cal, offsetof(Calibration, crc));
  EEPROM.put(0, cal); EEPROM.commit();
}
static void calLoad(){
  EEPROM.get(0, cal);
  if (cal.magic != CAL_MAGIC || cal.crc != crc32((const uint8_t *)&cal, offsetof(Calibration, crc))){ calDefaults(); calSave(); }
}

// ---------------------------------------------------------------- state
struct Reading { float airTemp = NAN, humidity = NAN, waterTemp = NAN, ph = NAN, ec = NAN, do2 = NAN, level = NAN, par = NAN, vpd = NAN; };
static Reading now;

enum Mode : uint8_t { AUTO = 0, MANUAL = 1 };
struct Control { const char *id; Mode mode; float level; bool on; float target; };
// order matters: index used for lookups below
static Control ctl[] = {
  { "lights",    AUTO, 100, true, 0 },
  { "pump",      AUTO, 100, true, 0 },
  { "air",       AUTO, 100, true, 0 },
  { "chiller",   AUTO, 100, true, WATER_TEMP_SET },
  { "valve",     AUTO, 100, true, 0 },
  { "nutrients", AUTO, 100, true, EC_TARGET },
  { "phc",       AUTO, 100, true, PH_TARGET },
};
static const size_t CTL_N = sizeof(ctl) / sizeof(ctl[0]);
static Control *find(const char *id){ for (auto &c : ctl) if (!strcmp(c.id, id)) return &c; return nullptr; }

struct Doser { Relay relay; uint32_t offAt = 0; uint32_t lastDose = 0; float mlWindow = 0; uint32_t windowStart = 0; };
static Doser doseA{ R_DOSE_A }, doseB{ R_DOSE_B }, dosePH{ R_DOSE_PH };

static uint32_t lastLinkMs = 0;     // last message from any dashboard
static bool chillerRunning = false;
static uint8_t relayState = 0xFF;   // cached (active-low) to avoid I2C chatter

// ---------------------------------------------------------------- helpers
static void relay(Relay r, bool on){
  const uint8_t bit = 1 << r;
  const bool cur = !(relayState & bit);
  if (cur == on) return;
  if (on) relayState &= ~bit; else relayState |= bit;
  relays.digitalWrite(r, on ? LOW : HIGH);
}
static float hourNow(){
  time_t t = time(nullptr);
  if (t < 1700000000) return -1;                         // clock not set yet
  struct tm lt; localtime_r(&t, &lt);
  return lt.tm_hour + lt.tm_min / 60.0f;
}
static float saturationDO(float t){ return 14.62f - 0.3898f * t + 0.006969f * t * t - 0.00005897f * t * t * t; }
static float svp(float t){ return 0.6108f * expf(17.27f * t / (t + 237.3f)); }

// ---------------------------------------------------------------- sensing
static float readVolts(Adc ch, adsGain_t gain){
  ads.setGain(gain);
  float acc = 0;
  for (int i = 0; i < 8; i++) acc += ads.computeVolts(ads.readADC_SingleEnded(ch));
  return acc / 8;
}
static void sense(){
  float t = sht.readTemperature(), h = sht.readHumidity();
  if (!isnan(t) && t > -20 && t < 70) now.airTemp = t;
  if (!isnan(h) && h >= 0 && h <= 100) now.humidity = h;

  float wt = waterProbe.getTempCByIndex(0);
  if (wt != DEVICE_DISCONNECTED_C && wt > -5 && wt < 50) now.waterTemp = wt;
  waterProbe.requestTemperatures();                      // async; read next cycle
  const float T = isnan(now.waterTemp) ? 25.0f : now.waterTemp;

  // pH — two-point line through the 7.00 and 4.00 buffer voltages
  const float vPH = readVolts(A_PH, GAIN_ONE);
  const float slope = 3.0f / (cal.phV7 - cal.phV4);
  now.ph = constrain(7.0f + (cal.phV7 - vPH) * slope, 0.0f, 14.0f);

  // EC — DFRobot TDS polynomial with 2 %/°C temperature compensation
  const float vEC = readVolts(A_EC, GAIN_ONE) / (1.0f + 0.02f * (T - 25.0f));
  const float tds = (133.42f * vEC * vEC * vEC - 255.86f * vEC * vEC + 857.39f * vEC) * 0.5f * cal.ecK;
  now.ec = max(0.0f, tds * 2.0f / 1000.0f);              // ppm(500) → mS/cm

  // dissolved O₂ — single-point saturation calibration, scaled by temperature
  const float vDO = readVolts(A_DO, GAIN_ONE);
  const float vSatT = cal.doVsat + 0.035f * (T - cal.doTcal);      // saturation voltage drifts ~35 mV/°C
  now.do2 = max(0.0f, vDO / max(0.1f, vSatT) * saturationDO(T));

  // level — 0.5–4.5 V pressure transducer, mapped between calibrated empty/full
  const float vL = readVolts(A_LEVEL, GAIN_TWOTHIRDS);
  now.level = constrain((vL - cal.levelV0) / (cal.levelV100 - cal.levelV0) * 100.0f, 0.0f, 100.0f);

  float lx = lux.readLightLevel();
  if (lx >= 0) now.par = lx * cal.parPerLux;

  if (!isnan(now.airTemp) && !isnan(now.humidity)) now.vpd = svp(now.airTemp) * (1.0f - now.humidity / 100.0f);
}

// ---------------------------------------------------------------- local controller
static float autoLights(){
  const float h = hourNow();
  if (h < 0) return 100;                                 // no clock: fail bright, never dark
  auto ramp = [](float x, float a, float b){ return constrain((x - a) / (b - a), 0.0f, 1.0f); };
  return 100.0f * ramp(h, LIGHTS_ON_HOUR, LIGHTS_ON_HOUR + 0.5f) * (1.0f - ramp(h, LIGHTS_OFF_HOUR - 0.5f, LIGHTS_OFF_HOUR));
}
static float effective(Control &c, float autoValue){
  if (!c.on) return 0;
  return c.mode == MANUAL ? c.level : autoValue;
}
static bool canDose(Doser &d, float ml){
  const uint32_t ms = millis();
  if (ms - d.windowStart > 3600000UL){ d.windowStart = ms; d.mlWindow = 0; }
  if (d.offAt) return false;                                                   // already running
  if (d.lastDose && ms - d.lastDose < DOSE_MIN_GAP_S * 1000UL) return false;   // let it mix
  if (d.mlWindow + ml > DOSE_MAX_ML_HOUR) return false;                        // hourly cap
  if (!isnan(now.level) && now.level < LEVEL_MIN_DOSING) return false;         // near-empty tank
  if (effective(*find("pump"), 100) <= 0) return false;                        // nothing would mix it
  return true;
}
static void startDose(Doser &d, float ml){
  relay(d.relay, true);
  d.offAt = millis() + (uint32_t)(ml / PUMP_ML_PER_SEC * 1000.0f);
  d.lastDose = millis();
  d.mlWindow += ml;
}
static void serviceDosers(){
  for (Doser *d : { &doseA, &doseB, &dosePH }){
    if (d->offAt && (int32_t)(millis() - d->offAt) >= 0){ relay(d->relay, false); d->offAt = 0; }
  }
}
static void publishEvent(const char *kind, const char *control, float ml, bool automatic);

static void control(){
  Control &L = *find("lights"), &P = *find("pump"), &A = *find("air"), &C = *find("chiller"),
          &V = *find("valve"), &N = *find("nutrients"), &H = *find("phc");

  const float lights = effective(L, autoLights());
  relay(R_LIGHTS, lights > 0.5f);
  analogWrite(PIN_LED_PWM, (int)(lights / 100.0f * 1023));

  relay(R_PUMP, effective(P, 100) > 0.5f);
  relay(R_AIR,  effective(A, 100) > 0.5f);
  relay(R_VALVE, effective(V, 100) > 0.5f);

  // chiller: hysteresis around the (possibly overridden) setpoint
  if (C.on && !isnan(now.waterTemp)){
    if (now.waterTemp > C.target + 0.5f) chillerRunning = true;
    if (now.waterTemp < C.target - 0.5f) chillerRunning = false;
  } else chillerRunning = false;
  relay(R_CHILLER, chillerRunning);

  // pH down: acid when above target + deadband
  if (H.on && H.mode == AUTO && !isnan(now.ph) && now.ph > H.target + PH_DEADBAND && canDose(dosePH, DOSE_PH_ML)){
    startDose(dosePH, DOSE_PH_ML);
    publishEvent("dose", "phc", DOSE_PH_ML, true);
  }
  // nutrients: equal parts A and B when EC falls below target − deadband
  if (N.on && N.mode == AUTO && !isnan(now.ec) && now.ec < N.target - EC_DEADBAND && canDose(doseA, DOSE_EC_ML) && canDose(doseB, DOSE_EC_ML)){
    startDose(doseA, DOSE_EC_ML);
    startDose(doseB, DOSE_EC_ML);
    publishEvent("dose", "nutrients", DOSE_EC_ML * 2, true);
  }

  // failsafe: if every dashboard is gone, overrides lapse back to automation
  if (lastLinkMs && millis() - lastLinkMs > LINK_TIMEOUT_S * 1000UL){
    for (auto &c : ctl){ c.mode = AUTO; c.on = true; }
    lastLinkMs = 0;
    publishEvent("failsafe", "all", 0, true);
  }
}

// ---------------------------------------------------------------- protocol
static void send(const String &json, const char *topicSuffix = "telemetry"){
#if ENABLE_WEBSOCKET
  String copy = json; ws.broadcastTXT(copy);
#endif
#if ENABLE_MQTT
  if (mqtt.connected()) mqtt.publish((String(MQTT_PREFIX) + "/" + topicSuffix).c_str(), json.c_str());
#endif
}
static void addReading(JsonDocument &doc){
  auto put = [&](const char *k, float v, int dec){ if (!isnan(v)) doc[k] = serialized(String(v, dec)); };
  put("airTemp", now.airTemp, 2); put("humidity", now.humidity, 1); put("vpd", now.vpd, 3);
  put("waterTemp", now.waterTemp, 2); put("ph", now.ph, 3); put("ec", now.ec, 3);
  put("do2", now.do2, 2); put("level", now.level, 1); put("par", now.par, 0);
}
static void sendTelemetry(){
  JsonDocument doc;
  doc["type"] = "telemetry"; doc["node"] = NODE_ID; doc["bed"] = NODE_BED; doc["fw"] = VERA_FW_VERSION;
  doc["uptime"] = millis() / 1000; doc["rssi"] = WiFi.RSSI(); doc["heap"] = ESP.getFreeHeap();
  addReading(doc);
  JsonObject c = doc["controls"].to<JsonObject>();
  const float autos[] = { autoLights(), 100, 100, 100, 100, 100, 100 };
  for (size_t i = 0; i < CTL_N; i++){
    JsonObject o = c[ctl[i].id].to<JsonObject>();
    o["mode"] = ctl[i].mode == MANUAL ? "manual" : "auto";
    o["on"] = ctl[i].on;
    o["level"] = effective(ctl[i], autos[i]);
    if (ctl[i].target) o["target"] = ctl[i].target;
  }
  doc["chiller"] = chillerRunning;
  String out; serializeJson(doc, out);
  send(out);
}
static void publishEvent(const char *kind, const char *controlId, float ml, bool automatic){
  JsonDocument doc;
  doc["type"] = "event"; doc["node"] = NODE_ID; doc["bed"] = NODE_BED;
  doc["kind"] = kind; doc["control"] = controlId; doc["auto"] = automatic;
  if (ml > 0) doc["ml"] = ml;
  String out; serializeJson(doc, out);
  send(out, "event");
}
static void ack(const char *ref, bool ok, const char *reason = nullptr){
  JsonDocument doc;
  doc["type"] = "ack"; doc["node"] = NODE_ID; doc["ref"] = ref; doc["ok"] = ok;
  if (reason) doc["reason"] = reason;
  String out; serializeJson(doc, out);
  send(out, "ack");
}
// scopes the dashboard may address this node with: "gh", "b<bed>", "b<bed>r<row>"
static bool forMe(const char *scope){
  if (!scope || !strcmp(scope, "gh")) return true;
  return scope[0] == 'b' && scope[1] - '0' == NODE_BED;
}
static void handleCommand(const char *payload, size_t len){
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;
  lastLinkMs = millis();
  const char *type = doc["type"] | "";
  const char *id = doc["control"] | "";
  const char *scope = doc["scope"] | "gh";
  const char *ref = doc["ref"] | "";
  if (!strcmp(type, "ping")){ ack(ref, true); return; }
  if (!strcmp(type, "hello")){ sendTelemetry(); return; }
  if (!forMe(scope)){ return; }
  if (strlen(scope) > 2 && strcmp(id, "valve")){ ack(ref, false, "no per-row actuator on this node"); return; }

  if (!strcmp(type, "auto_all")){ for (auto &c : ctl){ c.mode = AUTO; c.on = true; } ack(ref, true); return; }
  Control *c = find(id);
  if (!c){ ack(ref, false, "unknown control"); return; }
  if (!strcmp(type, "set")){
    if (doc["mode"].is<const char *>()) c->mode = !strcmp(doc["mode"], "manual") ? MANUAL : AUTO;
    if (doc["level"].is<float>()) c->level = constrain(doc["level"].as<float>(), 0.0f, 100.0f);
    if (doc["on"].is<bool>()) c->on = doc["on"];
    if (doc["target"].is<float>()) c->target = doc["target"];
    if (doc["set"].is<float>()) c->target = constrain(doc["set"].as<float>(), 14.0f, 28.0f);
    ack(ref, true);
  } else if (!strcmp(type, "dose")){
    const float ml = constrain(doc["ml"] | 5.0f, 1.0f, 50.0f);
    bool ok = false;
    if (!strcmp(id, "phc") && canDose(dosePH, ml)){ startDose(dosePH, ml); ok = true; }
    if (!strcmp(id, "nutrients") && canDose(doseA, ml) && canDose(doseB, ml)){ startDose(doseA, ml); startDose(doseB, ml); ok = true; }
    ack(ref, ok, ok ? nullptr : "dosing lockout (mixing gap, hourly cap, low level or pump off)");
    if (ok) publishEvent("dose", id, ml, false);
  }
  sendTelemetry();
}

#if ENABLE_WEBSOCKET
static void onWs(uint8_t num, WStype_t type, uint8_t *payload, size_t length){
  if (type == WStype_CONNECTED){ lastLinkMs = millis(); sendTelemetry(); }
  if (type == WStype_TEXT) handleCommand((const char *)payload, length);
}
#endif
#if ENABLE_MQTT
static void onMqtt(char *topic, byte *payload, unsigned int length){ handleCommand((const char *)payload, length); }
static void mqttConnect(){
  static uint32_t lastTry = 0;
  if (mqtt.connected() || millis() - lastTry < 5000) return;
  lastTry = millis();
  const String status = String(MQTT_PREFIX) + "/status";
  if (mqtt.connect(NODE_ID, MQTT_USER[0] ? MQTT_USER : nullptr, MQTT_PASSWORD[0] ? MQTT_PASSWORD : nullptr, status.c_str(), 1, true, "offline")){
    mqtt.publish(status.c_str(), "online", true);
    mqtt.subscribe((String(MQTT_PREFIX) + "/cmd").c_str(), 1);
  }
}
#endif

// ---------------------------------------------------------------- serial console (calibration)
static void console(){
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n'); line.trim();
  auto arg = [&](int i){ int s = 0; for (int k = 0; k < i; k++){ s = line.indexOf(' ', s) + 1; if (!s) return String(); } int e = line.indexOf(' ', s); return line.substring(s, e < 0 ? line.length() : e); };
  if (line == "status"){
    Serial.printf("air %.2f°C %.1f%%  water %.2f°C  pH %.2f  EC %.2f  DO %.1f  level %.0f%%  PAR %.0f\n",
      now.airTemp, now.humidity, now.waterTemp, now.ph, now.ec, now.do2, now.level, now.par);
  } else if (line == "cal ph 7"){ cal.phV7 = readVolts(A_PH, GAIN_ONE); calSave(); Serial.printf("pH7 = %.4f V\n", cal.phV7); }
  else if (line == "cal ph 4"){ cal.phV4 = readVolts(A_PH, GAIN_ONE); calSave(); Serial.printf("pH4 = %.4f V\n", cal.phV4); }
  else if (line.startsWith("cal ec ")){            // e.g. "cal ec 1.413" with a 1413 µS/cm buffer
    const float ref = arg(2).toFloat();
    if (ref > 0 && now.ec > 0){ cal.ecK *= ref / now.ec; calSave(); Serial.printf("EC k = %.4f\n", cal.ecK); }
  } else if (line == "cal do"){ cal.doVsat = readVolts(A_DO, GAIN_ONE); cal.doTcal = isnan(now.waterTemp) ? 25 : now.waterTemp; calSave(); Serial.println("DO saturated point saved"); }
  else if (line == "cal level empty"){ cal.levelV0 = readVolts(A_LEVEL, GAIN_TWOTHIRDS); calSave(); Serial.println("level 0% saved"); }
  else if (line == "cal level full"){ cal.levelV100 = readVolts(A_LEVEL, GAIN_TWOTHIRDS); calSave(); Serial.println("level 100% saved"); }
  else if (line.startsWith("dose ")){ const char *id = arg(1) == "ph" ? "phc" : "nutrients"; String j = String("{\"type\":\"dose\",\"control\":\"") + id + "\",\"ml\":" + arg(2) + "}"; handleCommand(j.c_str(), j.length()); }
  else if (line == "reset cal"){ calDefaults(); calSave(); Serial.println("calibration reset"); }
  else Serial.println("commands: status · cal ph 7|4 · cal ec <mS/cm> · cal do · cal level empty|full · dose ph|ec <ml> · reset cal");
}

// ---------------------------------------------------------------- setup / loop
void setup(){
  Serial.begin(115200);
  pinMode(PIN_STATUS, OUTPUT);
  analogWriteRange(1023); analogWriteFreq(1000);
  EEPROM.begin(sizeof(Calibration) + 16);
  calLoad();

  Wire.begin(PIN_SDA, PIN_SCL);
  for (uint8_t r = 0; r < R_COUNT; r++) relays.pinMode(r, OUTPUT, HIGH);   // all relays off at boot
  relays.begin();
  if (!ads.begin(0x48)) Serial.println("! ADS1115 not found");
  if (!sht.begin(0x44)) Serial.println("! SHT31 not found");
  if (!lux.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) Serial.println("! BH1750 not found");
  waterProbe.begin(); waterProbe.setWaitForConversion(false); waterProbe.requestTemperatures();

  WiFi.mode(WIFI_STA); WiFi.hostname(NODE_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTime(TZ_INFO, NTP_SERVER);
  MDNS.begin(NODE_HOSTNAME);
  ArduinoOTA.setHostname(NODE_HOSTNAME);
  ArduinoOTA.begin();

#if ENABLE_WEBSOCKET
  ws.begin(); ws.onEvent(onWs);
  MDNS.addService("ws", "tcp", WS_PORT);
#endif
#if ENABLE_MQTT
  #if MQTT_TLS
  netClient.setInsecure();               // demo default — pin the broker certificate in production
  netClient.setBufferSizes(1024, 1024);
  #endif
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1024);
  mqtt.setCallback(onMqtt);
#endif
  Serial.printf("\nVera node %s · %s · bed %d\n", VERA_FW_VERSION, NODE_ID, NODE_BED);
}

void loop(){
  static uint32_t tSense = 0, tTele = 0, tBlink = 0;
  ArduinoOTA.handle();
  MDNS.update();
#if ENABLE_WEBSOCKET
  ws.loop();
#endif
#if ENABLE_MQTT
  if (WiFi.status() == WL_CONNECTED){ mqttConnect(); mqtt.loop(); }
#endif
  serviceDosers();
  console();

  const uint32_t ms = millis();
  if (ms - tSense >= 1000){ tSense = ms; sense(); control(); }
  if (ms - tTele >= 2000){ tTele = ms; sendTelemetry(); }
  if (ms - tBlink >= (WiFi.status() == WL_CONNECTED ? 2000UL : 250UL)){ tBlink = ms; digitalWrite(PIN_STATUS, !digitalRead(PIN_STATUS)); }
}
