/*
 * ========================================================================
 *  TensioVigil-IoT  |  Real-Time Cardiovascular Vital-Sign Monitoring &
 *                      Clinical Decision Support for Hypertensive Patients
 * ------------------------------------------------------------------------
 *  Target board   : ESP32 DevKit-v1 (Wokwi)
 *  Author         : [REDACTED]
 *  Wireless link  : Wi-Fi 802.11 b/g/n  (Wokwi-GUEST)
 *  App-layer link : MQTT v3.1.1  (broker.hivemq.com:1883)
 *  Sensor bus     : I2C   (LCD), 1-Wire (DHT22), ADC (potentiometers)
 *  Cloud topics   : tensiovigil/<patient_id>/vitals    (publish, QoS 1)
 *                   tensiovigil/<patient_id>/alert     (publish, QoS 1)
 *                   tensiovigil/<patient_id>/command   (subscribe, QoS 1)
 *
 *  Clinical decision rules implemented from the
 *  2023 ESH Guidelines for the Management of Arterial Hypertension
 *  (Mancia et al., 2023) and the 2024 ESC Guidelines for the management
 *  of elevated blood pressure and hypertension (McEvoy et al., 2024).
 * ========================================================================
 */

#include <WiFi.h>              // Wi-Fi driver for ESP32
#include <PubSubClient.h>      // Eclipse Paho-style MQTT client
#include <Wire.h>              // I2C master driver
#include <LiquidCrystal_I2C.h> // 16x2 I2C LCD driver
#include <DHT.h>               // DHT22 temperature / humidity driver
#include <ArduinoJson.h>       // JSON encoder/decoder for MQTT payloads

/* ----------------------------- PIN MAP -------------------------------- */
#define PIN_POT_SBP   34   // ADC1_CH6 – simulates Systolic BP cuff signal
#define PIN_POT_DBP   35   // ADC1_CH7 – simulates Diastolic BP cuff signal
#define PIN_POT_HR    32   // ADC1_CH4 – simulates PPG-derived heart rate
#define PIN_DHT       4    // 1-Wire   – DHT22 body-temperature probe
#define PIN_LED_GRN   25   // Status LED – Normal
#define PIN_LED_YEL   26   // Status LED – Elevated / Grade-1
#define PIN_LED_RED   27   // Status LED – Grade-2 / Crisis
#define PIN_BUZZER    13   // Active buzzer – Hypertensive crisis alarm
#define I2C_SDA       21   // I2C data line
#define I2C_SCL       22   // I2C clock line

/* --------------------- NETWORK / MQTT CONFIGURATION ------------------- */
const char* WIFI_SSID   = "Wokwi-GUEST";          // Wokwi-provided AP
const char* WIFI_PASS   = "";                     // Open network in sim
const char* MQTT_HOST   = "broker.hivemq.com";    // Public test broker
const uint16_t MQTT_PORT = 1883;                  // Unencrypted MQTT
const char* PATIENT_ID  = "HTN-1010";             // Pseudonymous patient ID
const char* CLIENT_ID   = "TensioVigil-HTN1010";  // Unique MQTT client ID

/* --------------------------- TOPIC STRINGS ---------------------------- */
char TOPIC_VITALS[64];
char TOPIC_ALERT [64];
char TOPIC_CMD   [64];

/* ------------------------- TIMING CONSTANTS --------------------------- */
const uint32_t SAMPLE_PERIOD_MS    = 2000;   // 0.5 Hz sampling
const uint32_t PUBLISH_PERIOD_MS   = 5000;   // 0.2 Hz cloud publishing
const uint32_t MQTT_RECONNECT_MS   = 5000;   // back-off when broker drops

/* ------------------------ CLINICAL THRESHOLDS ------------------------- */
// Heart-rate cut-offs (resting adult)
const int HR_BRADY_BPM = 60;     // < 60 bpm => bradycardia
const int HR_TACHY_BPM = 100;    // > 100 bpm => tachycardia
// Body-temperature cut-offs (°C)
const float TEMP_HYPO  = 35.0f;  // < 35.0 => hypothermia
const float TEMP_FEVER = 38.0f;  // > 38.0 => febrile

/* ------------------------- GLOBAL OBJECTS ----------------------------- */
WiFiClient            netClient;
PubSubClient          mqtt(netClient);
LiquidCrystal_I2C     lcd(0x27, 16, 2);
DHT                   dht(PIN_DHT, DHT22);

/* ----------------------- RUNTIME STATE -------------------------------- */
uint32_t lastSampleMs  = 0;
uint32_t lastPublishMs = 0;
uint32_t lastMqttTryMs = 0;

struct Vitals {
  int   sbp;            // Systolic BP  (mmHg)
  int   dbp;            // Diastolic BP (mmHg)
  int   hr;             // Heart rate   (bpm)
  float tempC;          // Body temp    (°C)
  uint8_t category;     // 0 Optimal | 1 Normal | 2 High-normal
                        // 3 Grade-1 | 4 Grade-2 | 5 Grade-3/Crisis
  char  label[20];      // Human-readable BP class
  bool  crisis;         // true => hypertensive emergency
};
Vitals current{};

/* ===================== UTILITY  FUNCTIONS ============================ */

// Linear map for float values (Arduino map() is integer-only)
float mapFloat(float x, float inMin, float inMax, float outMin, float outMax) {
  return (x - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

// Translate raw ADC count to clinically realistic vital-sign range.
// Real cuff sensors would feed the same datatype, so the rest of the
// pipeline is unchanged when the prototype is moved to actual hardware.
void readSensors(Vitals& v) {
  int rawSbp = analogRead(PIN_POT_SBP);   // 0..4095 (12-bit)
  int rawDbp = analogRead(PIN_POT_DBP);
  int rawHr  = analogRead(PIN_POT_HR);

  v.sbp   = (int) mapFloat(rawSbp, 0, 4095,  80, 200);   // 80-200 mmHg
  v.dbp   = (int) mapFloat(rawDbp, 0, 4095,  40, 130);   // 40-130 mmHg
  v.hr    = (int) mapFloat(rawHr,  0, 4095,  30, 180);   //  30-180 bpm
  v.tempC = dht.readTemperature();                        // °C
  if (isnan(v.tempC)) v.tempC = 36.5f;                    // sensor-failure default
}

/* --------------------- CLINICAL DECISION ENGINE ----------------------- */
// Implements the 2023 ESH office-BP classification (Mancia et al., 2023).
// The category is defined by the HIGHEST of SBP or DBP, per guideline.
void classify(Vitals& v) {
  uint8_t catS = 0, catD = 0;

  if      (v.sbp >= 180) catS = 5;
  else if (v.sbp >= 160) catS = 4;
  else if (v.sbp >= 140) catS = 3;
  else if (v.sbp >= 130) catS = 2;
  else if (v.sbp >= 120) catS = 1;
  else                   catS = 0;

  if      (v.dbp >= 110) catD = 5;
  else if (v.dbp >= 100) catD = 4;
  else if (v.dbp >=  90) catD = 3;
  else if (v.dbp >=  85) catD = 2;
  else if (v.dbp >=  80) catD = 1;
  else                   catD = 0;

  v.category = (catS > catD) ? catS : catD;

  switch (v.category) {
    case 0: strcpy(v.label, "Optimal");      break;
    case 1: strcpy(v.label, "Normal");       break;
    case 2: strcpy(v.label, "High-normal");  break;
    case 3: strcpy(v.label, "Grade-1 HTN");  break;
    case 4: strcpy(v.label, "Grade-2 HTN");  break;
    case 5: strcpy(v.label, "Grade-3 CRIS"); break;
  }

  // Hypertensive emergency: SBP >= 180 OR DBP >= 120 (Mancia et al., 2023)
  v.crisis = (v.sbp >= 180 || v.dbp >= 120);
}

/* ------------------- HARDWARE FEEDBACK (LCD / LED / BUZZER) ----------- */
void drive_indicators(const Vitals& v) {
  // Drive 3 status LEDs in a one-hot fashion
  digitalWrite(PIN_LED_GRN, v.category <= 1);
  digitalWrite(PIN_LED_YEL, v.category == 2 || v.category == 3);
  digitalWrite(PIN_LED_RED, v.category >= 4);

  // Audible alert only for hypertensive emergency (avoids alarm fatigue)
  digitalWrite(PIN_BUZZER, v.crisis ? HIGH : LOW);
}

void updateLcd(const Vitals& v) {
  // Line 1 : BP and HR    – the two numbers a clinician scans first
  // Line 2 : Category label
  char l1[17], l2[17];
  snprintf(l1, sizeof(l1), "%3d/%-3d  HR%3d", v.sbp, v.dbp, v.hr);
  snprintf(l2, sizeof(l2), "%-16s",          v.label);
  lcd.setCursor(0, 0); lcd.print(l1);
  lcd.setCursor(0, 1); lcd.print(l2);
}

/* ============================ NETWORKING ============================ */
void wifi_connect() {
  Serial.print("[WiFi] Connecting to "); Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print('.');
  }
  Serial.printf("\n[WiFi] OK  IP=%s  RSSI=%d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

// MQTT subscribe callback – allows a clinician dashboard to push a
// command back to the device (e.g. trigger a manual reading, mute buzzer)
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, payload, len)) return;            // malformed
  const char* cmd = doc["cmd"] | "";
  Serial.printf("[MQTT] cmd=%s\n", cmd);

  if (!strcmp(cmd, "mute")) {
    digitalWrite(PIN_BUZZER, LOW);                            // silence
  } else if (!strcmp(cmd, "ping")) {
    mqtt.publish(TOPIC_VITALS, "{\"pong\":true}");            // health-chk
  }
}

void mqtt_connect() {
  if (mqtt.connected()) return;
  if (millis() - lastMqttTryMs < MQTT_RECONNECT_MS) return;
  lastMqttTryMs = millis();

  Serial.print("[MQTT] Connecting...");
  if (mqtt.connect(CLIENT_ID)) {
    Serial.println(" OK");
    mqtt.subscribe(TOPIC_CMD, 1);                             // QoS 1
  } else {
    Serial.printf(" FAIL rc=%d\n", mqtt.state());
  }
}

/* -------------------- CLOUD PUBLISHING (JSON / MQTT) ----------------- */
void publishVitals(const Vitals& v) {
  StaticJsonDocument<256> doc;
  doc["pid"]   = PATIENT_ID;
  doc["ts"]    = millis();
  doc["sbp"]   = v.sbp;
  doc["dbp"]   = v.dbp;
  doc["hr"]    = v.hr;
  doc["tempC"] = v.tempC;
  doc["cat"]   = v.category;
  doc["label"] = v.label;

  char buf[256];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(TOPIC_VITALS, (uint8_t*)buf, n, false);        // retain=false
  Serial.printf("[PUB] %s -> %s\n", TOPIC_VITALS, buf);

  // High-priority topic for crisis events: triggers cloud-side SMS/e-mail
  if (v.crisis || v.hr < HR_BRADY_BPM || v.hr > HR_TACHY_BPM) {
    StaticJsonDocument<256> a;
    a["pid"]   = PATIENT_ID;
    a["ts"]    = millis();
    a["sbp"]   = v.sbp;
    a["dbp"]   = v.dbp;
    a["hr"]    = v.hr;
    a["tempC"] = v.tempC;
    a["reason"] = v.crisis ? "HYPERTENSIVE_CRISIS"
                : (v.hr < HR_BRADY_BPM ? "BRADYCARDIA" : "TACHYCARDIA");
    char ab[256];
    size_t na = serializeJson(a, ab, sizeof(ab));
    mqtt.publish(TOPIC_ALERT, (uint8_t*)ab, na, true);        // retain=true
    Serial.printf("[ALERT] %s\n", ab);
  }
}

/* =============================== SETUP =============================== */
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== TensioVigil-IoT booting ===");

  // GPIO direction
  pinMode(PIN_LED_GRN, OUTPUT);
  pinMode(PIN_LED_YEL, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_BUZZER , OUTPUT);

  // I2C + LCD
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0,0); lcd.print("TensioVigil-IoT ");
  lcd.setCursor(0,1); lcd.print("Booting...      ");

  // 1-Wire sensor
  dht.begin();

  // Compose topic strings once
  snprintf(TOPIC_VITALS, sizeof(TOPIC_VITALS),
           "tensiovigil/%s/vitals", PATIENT_ID);
  snprintf(TOPIC_ALERT,  sizeof(TOPIC_ALERT),
           "tensiovigil/%s/alert",  PATIENT_ID);
  snprintf(TOPIC_CMD,    sizeof(TOPIC_CMD),
           "tensiovigil/%s/command",PATIENT_ID);

  // Network
  wifi_connect();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(30);
  mqtt_connect();
}

/* =============================== LOOP ================================ */
void loop() {
  // (1) Keep network up
  if (WiFi.status() != WL_CONNECTED) wifi_connect();
  if (!mqtt.connected())             mqtt_connect();
  mqtt.loop();                                                // service IN

  uint32_t now = millis();

  // (2) Periodic sensor sampling and on-device decision
  if (now - lastSampleMs >= SAMPLE_PERIOD_MS) {
    lastSampleMs = now;
    readSensors(current);
    classify   (current);
    drive_indicators(current);
    updateLcd  (current);
    Serial.printf("[SENS] %d/%d HR=%d T=%.1fC -> %s%s\n",
                  current.sbp, current.dbp, current.hr,
                  current.tempC, current.label,
                  current.crisis ? " *CRISIS*" : "");
  }

  // (3) Periodic cloud publish
  if (now - lastPublishMs >= PUBLISH_PERIOD_MS) {
    lastPublishMs = now;
    if (mqtt.connected()) publishVitals(current);
  }
}
