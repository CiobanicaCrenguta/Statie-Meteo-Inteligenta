/*

  Senzori:
  - Misol MS-WH-SP-WS02: anemometru + giruetă
  - DFRobot tipping bucket rain gauge
  - BME280: temperatură, umiditate, presiune
  - ML8511: UV analogic

  Strategie:
  - ESP32 stă activ permanent.
  - Vântul este eșantionat la 5 secunde.
  - BME280 și ML8511 sunt citite la 60 secunde.
  - Datele sunt agregate și trimise prin MQTT la 5 minute.
  - JSON-ul trimis este compatibil cu receptor.py de pe Raspberry Pi.
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <math.h>

// ============================================================
// CONFIG WIFI + MQTT
// ============================================================

const char* WIFI_SSID = "DIGI-V4W3";
const char* WIFI_PASS = "u4uhPje7q7";

// IP-ul Raspberry Pi-ului, cu Mosquitto pe portul 1883.
const char* MQTT_HOST = "meteo.local";
const int   MQTT_PORT = 1883;

const char* MQTT_CLIENT_ID_BASE = "esp32_meteo_obarsia_olt";
const char* MQTT_TOPIC_DATA = "meteo/senzori";
const char* MQTT_TOPIC_STATUS = "meteo/status/esp32";

WiFiClient espClient;
PubSubClient mqtt(espClient);

// ============================================================
// PINI
// ============================================================

const int ANEMOMETER_PIN = 14;
const int VANE_PIN       = 34;  // ADC1, input-only
const int RAIN_PIN       = 27;
const int UV_PIN         = 35;  // ADC1, input-only

const int I2C_SDA = 21;
const int I2C_SCL = 22;

// ============================================================
// TIMING
// ============================================================

const unsigned long WIND_SAMPLE_MS = 5000;       // 5 secunde
const unsigned long ENV_SAMPLE_MS  = 60000;      // 1 minut
const unsigned long PUBLISH_MS     = 300000;     // 5 minute

const unsigned long WIFI_RETRY_MS  = 10000;
const unsigned long MQTT_RETRY_MS  = 10000;

// ============================================================
// DEBOUNCE
// ============================================================

const uint32_t WIND_DEBOUNCE_US = 15000;     // 15 ms
const uint32_t RAIN_DEBOUNCE_US = 100000;    // 100 ms

// ============================================================
// CALIBRARE
// ============================================================

// DFRobot SEN0575: aprox. 0.2794 mm per tip.
const float MM_PER_TIP = 0.2794;

// Misol anemometer: aproximativ 2.4 km/h per Hz.
const float WIND_KMH_PER_HZ = 2.4;

// Corecție software pentru orientarea giruetei.
const float DIRECTION_OFFSET_DEG = 0.0;

// ML8511: conversie aproximativă.
const float UV_VOLTAGE_AT_INDEX_0  = 0.99;
const float UV_VOLTAGE_AT_INDEX_10 = 2.80;

// ============================================================
// TABEL GIROUETĂ
//  4.99kΩ rezistor
// ============================================================

struct VaneEntry {
  int adc;
  float degrees;
  const char* dir;
};

const VaneEntry vaneTable[] = {
  {  312, 112.5, "ESE" },
  {  430,  67.5, "ENE" },
  {  490,  90.0, "E"   },
  {  705, 157.5, "SSE" },
  { 1055, 135.0, "SE"  },
  { 1378, 202.5, "SSV" },
  { 1598, 180.0, "S"   },
  { 2125,  22.5, "NNE" },
  { 2340,  45.0, "NE"  },
  { 2823, 247.5, "VSV" },
  { 2923, 225.0, "SV"  },
  { 3155, 337.5, "NNV" },
  { 3440,   0.0, "N"   },
  { 3600, 292.5, "VNV" },
  { 3835, 315.0, "NV"  },
  { 4075, 270.0, "V"   },
};

const int vaneCount = sizeof(vaneTable) / sizeof(vaneTable[0]);

// ============================================================
// BME280
// ============================================================

Adafruit_BME280 bme;
bool bmeOk = false;

// ============================================================
// ISR STATE
// ============================================================

volatile uint32_t windPulseCount = 0;
volatile uint32_t rainTipCount = 0;
volatile uint32_t totalRainTipCount = 0;

volatile uint32_t lastWindPulseUs = 0;
volatile uint32_t lastRainTipUs = 0;

// ============================================================
// AGREGARE 5 MINUTE
// ============================================================

struct Aggregator {
  double tempSum = 0;
  double humSum = 0;
  double presSum = 0;
  double uvSum = 0;
  int envSamples = 0;

  double windSum = 0;
  int windSamples = 0;
  float gustMax = 0;

  double dirSinSum = 0;
  double dirCosSum = 0;
  double dirWeightSum = 0;

  void reset() {
    tempSum = 0;
    humSum = 0;
    presSum = 0;
    uvSum = 0;
    envSamples = 0;

    windSum = 0;
    windSamples = 0;
    gustMax = 0;

    dirSinSum = 0;
    dirCosSum = 0;
    dirWeightSum = 0;
  }
};

Aggregator agg;

// ============================================================
// TIMERS
// ============================================================

unsigned long lastWindSampleMs = 0;
unsigned long lastEnvSampleMs = 0;
unsigned long lastPublishMs = 0;
unsigned long lastWifiRetryMs = 0;
unsigned long lastMqttRetryMs = 0;

// ============================================================
// ISR
// ============================================================

void IRAM_ATTR anemometerISR() {
  uint32_t now = micros();

  if ((uint32_t)(now - lastWindPulseUs) > WIND_DEBOUNCE_US) {
    windPulseCount++;
    lastWindPulseUs = now;
  }
}

void IRAM_ATTR rainISR() {
  uint32_t now = micros();

  if ((uint32_t)(now - lastRainTipUs) > RAIN_DEBOUNCE_US) {
    rainTipCount++;
    totalRainTipCount++;
    lastRainTipUs = now;
  }
}

// ============================================================
// UTILS
// ============================================================

float normalizeDegrees(float deg) {
  while (deg < 0) deg += 360.0;
  while (deg >= 360.0) deg -= 360.0;
  return deg;
}

float applyDirectionOffset(float deg) {
  return normalizeDegrees(deg + DIRECTION_OFFSET_DEG);
}

int readAdcAverage(int pin, int samples = 32) {
  long sum = 0;

  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delayMicroseconds(500);
  }

  return sum / samples;
}

float readAdcVoltage(int pin, int samples = 32) {
  long mvSum = 0;

  for (int i = 0; i < samples; i++) {
    mvSum += analogReadMilliVolts(pin);
    delayMicroseconds(500);
  }

  return (mvSum / (float)samples) / 1000.0;
}

const VaneEntry& readVaneEntry(int* rawAdcOut = nullptr) {
  int adc = readAdcAverage(VANE_PIN, 32);

  if (rawAdcOut != nullptr) {
    *rawAdcOut = adc;
  }

  int bestIdx = 0;
  int bestDiff = 99999;

  for (int i = 0; i < vaneCount; i++) {
    int diff = abs(adc - vaneTable[i].adc);

    if (diff < bestDiff) {
      bestDiff = diff;
      bestIdx = i;
    }
  }

  return vaneTable[bestIdx];
}

float readUvEstimate() {
  float voltage = readAdcVoltage(UV_PIN, 32);

  // ML8511 / SEN0175: mapare aproximativă tensiune -> intensitate UV.
  // DFRobot foloseste aproximativ 0.99V pentru 0 mW/cm²
  // si ~2.8-2.9V pentru 15 mW/cm².
  float uv_mW_cm2 = (voltage - 0.99) * (15.0 / (2.80 - 0.99));

  if (uv_mW_cm2 < 0) uv_mW_cm2 = 0;
  if (uv_mW_cm2 > 15) uv_mW_cm2 = 15;

  // Conversie: 1 mW/cm² = 10 W/m².
  float uv_W_m2 = uv_mW_cm2 * 10.0;

  return uv_W_m2;
}

float circularDirectionFromSums(double sinSum, double cosSum) {
  if (abs(sinSum) < 0.000001 && abs(cosSum) < 0.000001) {
    int adc = 0;
    const VaneEntry& v = readVaneEntry(&adc);
    return applyDirectionOffset(v.degrees);
  }

  float deg = atan2(sinSum, cosSum) * 180.0 / PI;
  return normalizeDegrees(deg);
}

// ============================================================
// WIFI + MQTT
// ============================================================

void printWiFiStatus(wl_status_t status) {
  Serial.print("[WiFi] Status: ");

  switch (status) {
    case WL_IDLE_STATUS:
      Serial.println("IDLE");
      break;

    case WL_NO_SSID_AVAIL:
      Serial.println("SSID nu a fost găsit");
      break;

    case WL_SCAN_COMPLETED:
      Serial.println("SCAN_COMPLETED");
      break;

    case WL_CONNECTED:
      Serial.println("CONNECTED");
      break;

    case WL_CONNECT_FAILED:
      Serial.println("Conectare eșuată - parolă greșită sau autentificare respinsă");
      break;

    case WL_CONNECTION_LOST:
      Serial.println("Conexiune pierdută");
      break;

    case WL_DISCONNECTED:
      Serial.println("DISCONNECTED");
      break;

    default:
      Serial.println((int)status);
      break;
  }
}

void connectWiFiIfNeeded() {
  static bool started = false;
  static bool printedConnected = false;

  if (WiFi.status() == WL_CONNECTED) {
    if (!printedConnected) {
      Serial.print("[WiFi] Conectat. IP ESP32: ");
      Serial.println(WiFi.localIP());
      printedConnected = true;
    }

    return;
  }

  printedConnected = false;

  unsigned long now = millis();

  if (!started) {
    Serial.println("[WiFi] Pornesc conectarea...");
    Serial.print("[WiFi] SSID folosit: ");
    Serial.println(WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    started = true;
    lastWifiRetryMs = now;
    return;
  }

  if (now - lastWifiRetryMs >= WIFI_RETRY_MS) {
    lastWifiRetryMs = now;

    wl_status_t st = WiFi.status();
    printWiFiStatus(st);

    Serial.println("[WiFi] Reîncerc conectarea...");
    WiFi.disconnect();
    delay(300);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

String buildMqttClientId() {
  String id = MQTT_CLIENT_ID_BASE;
  id += "-";
  id += WiFi.macAddress();
  id.replace(":", "");
  return id;
}

void connectMqttIfNeeded() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (mqtt.connected()) {
    return;
  }

  unsigned long now = millis();

  if (now - lastMqttRetryMs < MQTT_RETRY_MS) {
    return;
  }

  lastMqttRetryMs = now;

  Serial.println("[MQTT] Conectare...");

  String clientId = buildMqttClientId();

  bool ok = mqtt.connect(
    clientId.c_str(),
    MQTT_TOPIC_STATUS,
    1,
    true,
    "offline"
  );

  if (ok) {
    Serial.println("[MQTT] Conectat.");
    mqtt.publish(MQTT_TOPIC_STATUS, "online", true);
  } else {
    Serial.print("[MQTT] Eroare, rc=");
    Serial.println(mqtt.state());
  }
}

// ============================================================
// SENSOR SAMPLING
// ============================================================

void sampleWind(unsigned long elapsedMs) {
  if (elapsedMs == 0) {
    return;
  }

  uint32_t pulses = 0;

  noInterrupts();
  pulses = windPulseCount;
  windPulseCount = 0;
  interrupts();

  float seconds = elapsedMs / 1000.0;
  float hz = pulses / seconds;
  float kmh = hz * WIND_KMH_PER_HZ;

  int adc = 0;
  const VaneEntry& vane = readVaneEntry(&adc);
  float dirDeg = applyDirectionOffset(vane.degrees);

  agg.windSum += kmh;
  agg.windSamples++;

  if (kmh > agg.gustMax) {
    agg.gustMax = kmh;
  }

  // Medie circulară ponderată cu viteza.
  // Dacă vântul este foarte slab, îi dăm greutate mică.
  float weight = max(kmh, 0.1f);
  float rad = dirDeg * PI / 180.0;

  agg.dirSinSum += sin(rad) * weight;
  agg.dirCosSum += cos(rad) * weight;
  agg.dirWeightSum += weight;

  Serial.printf(
    "[WIND] %.2f km/h | gust max %.2f | dir %.1f | adc %d | windSamples %d\n",
    kmh,
    agg.gustMax,
    dirDeg,
    adc,
    agg.windSamples
  );
}

void sampleEnvironment() {
  if (!bmeOk) {
    Serial.println("[BME280] Senzor indisponibil.");
    return;
  }

  bool ok = bme.takeForcedMeasurement();

  if (!ok) {
    Serial.println("[BME280] Citire eșuată.");
    return;
  }

  float temperatura = bme.readTemperature();
  float umiditate = bme.readHumidity();
  float presiune = bme.readPressure() / 100.0F;
  float uv = readUvEstimate();

  if (isnan(temperatura) || isnan(umiditate) || isnan(presiune)) {
    Serial.println("[ENV] Valori NaN, ignor citirea.");
    return;
  }

  agg.tempSum += temperatura;
  agg.humSum += umiditate;
  agg.presSum += presiune;
  agg.uvSum += uv;
  agg.envSamples++;

  Serial.printf(
    "[ENV] T %.2f C | H %.1f %% | P %.1f hPa | UV %.2f | envSamples %d\n",
    temperatura,
    umiditate,
    presiune,
    uv,
    agg.envSamples
  );
}

// ============================================================
// MQTT PUBLISH
// ============================================================

bool publishAggregatedData() {
  if (agg.envSamples == 0 || agg.windSamples == 0) {
    Serial.println("[PUB] Nu sunt suficiente eșantioane pentru publicare.");
    Serial.print("[PUB] envSamples=");
    Serial.print(agg.envSamples);
    Serial.print(" | windSamples=");
    Serial.println(agg.windSamples);
    return false;
  }

  if (WiFi.status() != WL_CONNECTED || !mqtt.connected()) {
    Serial.println("[MQTT] Nu este conectat. Nu public.");
    return false;
  }

  uint32_t rainTipsSnapshot = 0;
  uint32_t rainTotalSnapshot = 0;

  noInterrupts();
  rainTipsSnapshot = rainTipCount;
  rainTotalSnapshot = totalRainTipCount;
  interrupts();

  float temperatura = agg.tempSum / agg.envSamples;
  float umiditate = agg.humSum / agg.envSamples;
  float presiune = agg.presSum / agg.envSamples;
  float uv = agg.uvSum / agg.envSamples;

  float vitezaVant = agg.windSum / agg.windSamples;
  float rafala = agg.gustMax;

  float directieVant = circularDirectionFromSums(
    agg.dirSinSum,
    agg.dirCosSum
  );

  float precipitatii = rainTipsSnapshot * MM_PER_TIP;
  float precipitatiiTotal = rainTotalSnapshot * MM_PER_TIP;

  StaticJsonDocument<512> doc;

  // Cheile acestea sunt compatibile cu receptor.py de pe Raspberry Pi.
  doc["temperatura"] = round(temperatura * 10.0) / 10.0;
  doc["umiditate"] = round(umiditate);
  doc["presiune"] = round(presiune * 10.0) / 10.0;
  doc["viteza_vant"] = round(vitezaVant * 10.0) / 10.0;
  doc["rafala"] = round(rafala * 10.0) / 10.0;
  doc["uv"] = round(uv * 10.0) / 10.0;
  doc["precipitatii"] = round(precipitatii * 100.0) / 100.0;
  doc["directie_vant"] = round(directieVant);

  // Câmpuri extra, utile pentru debug.
  doc["rain_total_mm"] = round(precipitatiiTotal * 100.0) / 100.0;
  doc["env_samples"] = agg.envSamples;
  doc["wind_samples"] = agg.windSamples;
  doc["uptime_s"] = millis() / 1000;

  char payload[512];
  serializeJson(doc, payload);

  Serial.print("[MQTT] Payload: ");
  Serial.println(payload);

  bool sent = mqtt.publish(MQTT_TOPIC_DATA, payload, false);

  if (sent) {
    Serial.println("[MQTT] Publicat cu succes.");

    // Scădem doar tips-urile incluse în payload.
    // Dacă apare ploaie în timpul publicării, nu o pierdem.
    noInterrupts();
    if (rainTipCount >= rainTipsSnapshot) {
      rainTipCount -= rainTipsSnapshot;
    } else {
      rainTipCount = 0;
    }
    interrupts();

    agg.reset();
    return true;
  }

  Serial.println("[MQTT] Publicare eșuată. Păstrez agregarea pentru retry.");
  return false;
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=== ESP32 Weather Node — Obârșia Olt ===");

  // Pini digitali cu interrupt
  pinMode(ANEMOMETER_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ANEMOMETER_PIN), anemometerISR, FALLING);

  pinMode(RAIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(RAIN_PIN), rainISR, FALLING);

  // ADC
  pinMode(VANE_PIN, INPUT);
  pinMode(UV_PIN, INPUT);

  analogReadResolution(12);
  analogSetPinAttenuation(VANE_PIN, ADC_11db);
  analogSetPinAttenuation(UV_PIN, ADC_11db);

  // I2C + BME280
  Wire.begin(I2C_SDA, I2C_SCL);

  if (bme.begin(0x76)) {
    bmeOk = true;
    Serial.println("[BME280] Găsit la 0x76.");
  } else if (bme.begin(0x77)) {
    bmeOk = true;
    Serial.println("[BME280] Găsit la 0x77.");
  } else {
    bmeOk = false;
    Serial.println("[BME280] NU a fost găsit la 0x76 sau 0x77.");
  }

  if (bmeOk) {
    bme.setSampling(
      Adafruit_BME280::MODE_FORCED,
      Adafruit_BME280::SAMPLING_X2,   // temperatura
      Adafruit_BME280::SAMPLING_X16,  // presiune
      Adafruit_BME280::SAMPLING_X1,   // umiditate
      Adafruit_BME280::FILTER_X16,
      Adafruit_BME280::STANDBY_MS_1000
    );
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(60);
  mqtt.setSocketTimeout(5);

  agg.reset();

  unsigned long now = millis();
  lastWindSampleMs = now;
  lastEnvSampleMs = now;
  lastPublishMs = now;

  connectWiFiIfNeeded();

  // Prima citire de mediu imediat după boot, ca să nu așteptăm 60 secunde.
  sampleEnvironment();
}

// ============================================================
// LOOP
// ============================================================

void loop() {
  connectWiFiIfNeeded();
  connectMqttIfNeeded();

  if (mqtt.connected()) {
    mqtt.loop();
  }

  unsigned long now = millis();

  if (now - lastWindSampleMs >= WIND_SAMPLE_MS) {
    unsigned long elapsed = now - lastWindSampleMs;
    sampleWind(elapsed);
    lastWindSampleMs = now;
  }

  if (now - lastEnvSampleMs >= ENV_SAMPLE_MS) {
    sampleEnvironment();
    lastEnvSampleMs = now;
  }

  if (now - lastPublishMs >= PUBLISH_MS) {
    if (agg.envSamples == 0) {
      sampleEnvironment();
    }

    bool ok = publishAggregatedData();

    if (ok) {
      lastPublishMs = now;
    } else {
      // Dacă publicarea eșuează, mai încercăm peste 30 secunde,
      // nu peste alte 5 minute.
      lastPublishMs = now - PUBLISH_MS + 30000;
    }
  }
}