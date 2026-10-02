/*
  ==================================================================
  SISTEMA DE SEGURANÇA PATRIMONIAL — ESP32 + MQTT + FreeRTOS
  ==================================================================
  Versão v3.3 — agendamento de armamento (janela diária + NTP)
  ------------------------------------------------------------------
  Sensores : PIR (16) + HC-SR04 (TRIG 5 / ECHO 18) + LDR (34)
  Atuadores: Buzzer 1 (19), Buzzer 2 (23), LED (2)
  Display  : OLED SSD1306 I2C (SDA 21 / SCL 22)
  Entrada  : Botão (4, INPUT_PULLUP)
  Rede     : Wokwi-GUEST
  Broker   : broker.hivemq.com (TCP 1883)
  NVS      : estado + agendamento
  NTP      : pool.ntp.org (UTC-3, America/Sao_Paulo sem DST)

  Arquitetura: 5 tasks FreeRTOS em 2 cores
    mqttTask    (core 0) → WiFi + MQTT (publish/subscribe)
    displayTask (core 0) → OLED a cada 250ms
    sensorTask  (core 1) → PIR, HC-SR04, LDR, botão → eventQueue
    alarmTask   (core 1) → máquina de estados + LED + bordas de schedule
    sirenTask   (core 1) → sirene contínua controlada por flag

  Agendamento (v3.3):
    - Tópico .../schedule: painel publica; ESP32 persiste em NVS e re-broadcasta (retained)
    - Janela diária HH:MM→HH:MM (pode cruzar meia-noite); opcional só dias úteis
    - ESP32 com NTP é a autoridade: arma no início e desarma no fim da janela
    - Override: desarmar dentro da janela segura até o fim dela (g_scheduleManualHold)
    - Eventos: schedule_saved / schedule_armed / schedule_disarmed / schedule_skipped
  ==================================================================
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ctype.h>

// ===================== Configurações =====================
constexpr char WIFI_SSID[]     = "Wokwi-GUEST";
constexpr char WIFI_PASSWORD[] = "";
constexpr char MQTT_SERVER[]   = "broker.hivemq.com";
constexpr uint16_t MQTT_PORT   = 1883;

// ⚠️ Fonte única do namespace MQTT — troque "meu-esp32" por algo único.
// Todos os tópicos abaixo são derivados deste prefixo (mesmo contrato do dashboard).
#define SECURITY_TOPIC_PREFIX "seguranca/patrimonio/meu-esp32"

constexpr char TOPIC_CMD[]     = SECURITY_TOPIC_PREFIX "/cmd";
constexpr char TOPIC_STATE[]   = SECURITY_TOPIC_PREFIX "/state";
constexpr char TOPIC_ALARM[]   = SECURITY_TOPIC_PREFIX "/alarm";
constexpr char TOPIC_LOG[]     = SECURITY_TOPIC_PREFIX "/log";
constexpr char TOPIC_SENSORS[] = SECURITY_TOPIC_PREFIX "/sensors";
constexpr char TOPIC_STATUS[]  = SECURITY_TOPIC_PREFIX "/status";
constexpr char TOPIC_SCHEDULE[] = SECURITY_TOPIC_PREFIX "/schedule";

// NTP — America/Sao_Paulo sem DST (UTC-3). Ajuste se o hardware for em outro fuso.
constexpr int32_t NTP_GMT_OFFSET_SEC = -3 * 3600;
constexpr int32_t NTP_DST_OFFSET_SEC = 0;
constexpr char NTP_SERVER_1[] = "pool.ntp.org";
constexpr char NTP_SERVER_2[] = "time.google.com";

// Agendamento de armamento (janela diária; pode cruzar meia-noite).
constexpr uint16_t SCHEDULE_DEFAULT_ARM_MIN    = 18 * 60;  // 18:00
constexpr uint16_t SCHEDULE_DEFAULT_DISARM_MIN = 8 * 60;   // 08:00

// Pinos
constexpr int PIN_PIR      = 16;
constexpr int PIN_TRIG     = 5;
constexpr int PIN_ECHO     = 18;
constexpr int PIN_BUZZER_1 = 19;
constexpr int PIN_BUZZER_2 = 23;
constexpr int PIN_BUTTON   = 4;
constexpr int PIN_LED      = 2;
constexpr int PIN_LDR      = 34;

// OLED
constexpr int OLED_W = 128;
constexpr int OLED_H = 64;
constexpr int OLED_ADDR = 0x3C;

// Parâmetros de detecção
constexpr int      DIST_THRESHOLD_CM   = 40;
constexpr uint32_t DIST_CONFIRM_MS     = 800;
constexpr uint32_t PIR_HOLD_MS         = 3000;
constexpr uint32_t BUTTON_DEBOUNCE_MS  = 300;
constexpr uint32_t ALARM_AUTO_RESET_MS = 60000;

// LDR
constexpr uint8_t  LDR_CAL_SAMPLES = 50;
constexpr uint8_t  LDR_AVG_SAMPLES = 8;
constexpr int      LDR_DELTA_ON    = 300;
constexpr int      LDR_DELTA_OFF   = 200;
constexpr uint32_t LDR_CONFIRM_MS  = 2000;

// HC-SR04
constexpr uint8_t ULTRA_SAMPLES = 5;

// ===================== Estruturas =====================
enum EventType : uint8_t {
  EVT_PIR        = 0,
  EVT_PROXIMITY  = 1,
  EVT_BUTTON     = 2,
  EVT_CMD_ARM    = 3,
  EVT_CMD_DISARM = 4,
  EVT_CMD_RESET  = 5,
  EVT_CMD_TEST   = 6,
  EVT_CMD_RECAL  = 7,
  EVT_LDR        = 8
};

struct Event {
  EventType type;
  uint16_t value;
};

struct TxMessage {
  char topic[80];
  char payload[200];
  bool retain = false;
};

enum SystemState : uint8_t {
  STATE_DISARMED = 0,
  STATE_ARMED    = 1,
  STATE_ALARM    = 2
};

struct SystemStatus {
  SystemState state       = STATE_DISARMED;
  bool        pir         = false;
  uint16_t    distance_cm = 999;
  int         ldr         = 0;
  bool        ldrAnomaly  = false;
  uint32_t    lastPirMs   = 0;
  uint32_t    lastAlarmMs = 0;
};

// Janela diária de armamento: minutos desde 00:00.
struct ScheduleConfig {
  bool     enabled       = false;
  uint16_t armMinutes    = SCHEDULE_DEFAULT_ARM_MIN;
  uint16_t disarmMinutes = SCHEDULE_DEFAULT_DISARM_MIN;
  bool     weekdaysOnly  = true;
};

// ===================== Globais =====================
WiFiClient   espClient;
PubSubClient mqttClient(espClient);
Preferences  prefs;
Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);

QueueHandle_t     eventQueue  = nullptr;
QueueHandle_t     txQueue     = nullptr;
SemaphoreHandle_t statusMutex = nullptr;

SystemStatus g_status;
char g_clientId[40] = {0};

// Sirene: alarmTask escreve, sirenTask lê
volatile bool g_sirenActive = false;

// MQTT conectado: só mqttTask escreve; displayTask lê (PubSubClient não é thread-safe)
volatile bool g_mqttConnected = false;

// Recalibração LDR: alarmTask pede; sensorTask executa (não bloqueia o alarme)
volatile bool g_ldrRecalRequested = false;

// Agendamento: config em RAM (mutex); hold manual e NTP como flags voláteis.
ScheduleConfig g_schedule;
volatile bool  g_scheduleManualHold = false;
volatile bool  g_timeSynced = false;

// LDR
int      g_ldrBase    = 2000;
int      g_ldrOn      = 2300;
int      g_ldrOff     = 2100;
bool     g_ldrAnomaly = false;
uint32_t g_ldrStart   = 0;

// HC-SR04 filtro
float   g_ultraBuffer[ULTRA_SAMPLES] = {400, 400, 400, 400, 400};
uint8_t g_ultraIdx = 0;
float   g_ultraSum = 2000.0f;

// ===================== NVS =====================
void nvsLoad() {
  prefs.begin("security", true);
  g_status.state = (SystemState)prefs.getUChar("state", STATE_DISARMED);
  prefs.end();
}

void nvsSaveState(SystemState s) {
  prefs.begin("security", false);
  prefs.putUChar("state", (uint8_t)s);
  prefs.end();
}

void nvsLoadSchedule() {
  prefs.begin("security", true);
  g_schedule.enabled       = prefs.getUChar("sched_en", 0) != 0;
  g_schedule.armMinutes    = prefs.getUShort("sched_arm", SCHEDULE_DEFAULT_ARM_MIN);
  g_schedule.disarmMinutes = prefs.getUShort("sched_dis", SCHEDULE_DEFAULT_DISARM_MIN);
  g_schedule.weekdaysOnly  = prefs.getUChar("sched_wd", 1) != 0;
  prefs.end();
  Serial.printf("[NVS] Schedule: en=%d arm=%u disarm=%u wd=%d\n",
                g_schedule.enabled, g_schedule.armMinutes,
                g_schedule.disarmMinutes, g_schedule.weekdaysOnly);
}

void nvsSaveSchedule(const ScheduleConfig& cfg) {
  prefs.begin("security", false);
  prefs.putUChar("sched_en", cfg.enabled ? 1 : 0);
  prefs.putUShort("sched_arm", cfg.armMinutes);
  prefs.putUShort("sched_dis", cfg.disarmMinutes);
  prefs.putUChar("sched_wd", cfg.weekdaysOnly ? 1 : 0);
  prefs.end();
}

// ===================== Agendamento (helpers) =====================
bool parseHourMinute(const char* hhmm, uint16_t& outMinutes) {
  if (!hhmm || strlen(hhmm) != 5 || hhmm[2] != ':') return false;
  if (!isdigit((unsigned char)hhmm[0]) || !isdigit((unsigned char)hhmm[1]) ||
      !isdigit((unsigned char)hhmm[3]) || !isdigit((unsigned char)hhmm[4])) {
    return false;
  }
  int h = (hhmm[0] - '0') * 10 + (hhmm[1] - '0');
  int m = (hhmm[3] - '0') * 10 + (hhmm[4] - '0');
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  outMinutes = (uint16_t)(h * 60 + m);
  return true;
}

void formatHourMinute(uint16_t minutes, char* out, size_t n) {
  uint16_t m = minutes % 1440;
  snprintf(out, n, "%02u:%02u", (unsigned)(m / 60), (unsigned)(m % 60));
}

bool isWeekday(const struct tm& t) {
  // tm_wday: 0=domingo … 6=sábado → dias úteis 1–5
  return t.tm_wday >= 1 && t.tm_wday <= 5;
}

// Janela ativa no minuto atual; trata virada de meia-noite (ex.: 18:00→08:00).
bool isScheduleWindowActive(const ScheduleConfig& cfg, const struct tm& t) {
  if (!cfg.enabled) return false;
  if (cfg.weekdaysOnly && !isWeekday(t)) return false;

  uint16_t nowMin = (uint16_t)(t.tm_hour * 60 + t.tm_min);
  if (cfg.armMinutes == cfg.disarmMinutes) {
    // Janela nula: considera ativa o minuto exato do início (raro).
    return nowMin == cfg.armMinutes;
  }
  if (cfg.armMinutes > cfg.disarmMinutes) {
    // Cruzamento de meia-noite: [arm, 24h) ∪ [0, disarm)
    return nowMin >= cfg.armMinutes || nowMin < cfg.disarmMinutes;
  }
  // Mesmo dia: [arm, disarm)
  return nowMin >= cfg.armMinutes && nowMin < cfg.disarmMinutes;
}

ScheduleConfig copyScheduleConfig() {
  ScheduleConfig snap;
  xSemaphoreTake(statusMutex, portMAX_DELAY);
  snap = g_schedule;
  xSemaphoreGive(statusMutex);
  return snap;
}

const char* scheduleLabel(const ScheduleConfig& cfg, const struct tm* t, bool hasTime, bool manualHold) {
  if (!cfg.enabled || !hasTime || !t) return "idle";
  if (!isScheduleWindowActive(cfg, *t)) return "idle";
  return manualHold ? "override" : "active";
}

// Lê o relógio local sem bloquear a task por muito tempo.
bool readLocalTime(struct tm& out) {
  return getLocalTime(&out, 50);
}

// ===================== Fila TX =====================
void enqueueTx(const char* topic, const char* payload, bool retain = false) {
  TxMessage m{};
  strncpy(m.topic, topic, sizeof(m.topic) - 1);
  strncpy(m.payload, payload, sizeof(m.payload) - 1);
  m.retain = retain;
  if (xQueueSend(txQueue, &m, 0) != pdTRUE) {
    Serial.println("[TX] fila cheia");
  }
}

// Descarta eventos velhos da fila (ex.: PIR ainda alto ao armar).
void drainEventQueue() {
  Event discarded;
  while (xQueueReceive(eventQueue, &discarded, 0) == pdTRUE) {
  }
}

// ===================== Publicação MQTT =====================
void publishState() {
  StaticJsonDocument<160> doc;
  doc["armed"] = (g_status.state != STATE_DISARMED);
  doc["alarm"] = (g_status.state == STATE_ALARM);

  struct tm timeinfo;
  bool hasTime = readLocalTime(timeinfo);
  ScheduleConfig cfg = copyScheduleConfig();
  doc["schedule"] = scheduleLabel(cfg, hasTime ? &timeinfo : nullptr,
                                  hasTime, g_scheduleManualHold);

  char buf[160];
  serializeJson(doc, buf, sizeof(buf));
  enqueueTx(TOPIC_STATE, buf);
}

void publishSchedule() {
  StaticJsonDocument<192> doc;
  ScheduleConfig cfg = copyScheduleConfig();

  char armBuf[8];
  char disarmBuf[8];
  formatHourMinute(cfg.armMinutes, armBuf, sizeof(armBuf));
  formatHourMinute(cfg.disarmMinutes, disarmBuf, sizeof(disarmBuf));

  doc["enabled"]      = cfg.enabled;
  doc["armAt"]        = armBuf;
  doc["disarmAt"]     = disarmBuf;
  doc["weekdaysOnly"] = cfg.weekdaysOnly;

  char buf[192];
  serializeJson(doc, buf, sizeof(buf));
  // Retained: o painel reexibe a config ao reconectar.
  enqueueTx(TOPIC_SCHEDULE, buf, true);
}

void publishAlarm(const char* type, uint16_t value) {
  StaticJsonDocument<160> doc;
  doc["type"]  = type;
  doc["value"] = value;
  doc["ts"]    = millis();
  char buf[160];
  serializeJson(doc, buf, sizeof(buf));
  enqueueTx(TOPIC_ALARM, buf);
}

void publishLog(const char* event) {
  StaticJsonDocument<128> doc;
  doc["event"] = event;
  doc["state"] = (int)g_status.state;
  doc["ts"]    = millis();
  char buf[128];
  serializeJson(doc, buf, sizeof(buf));
  enqueueTx(TOPIC_LOG, buf);
}

void publishSensors() {
  StaticJsonDocument<192> doc;
  doc["pir"]         = g_status.pir;
  doc["dist"]        = g_status.distance_cm;
  doc["ldr"]         = g_status.ldr;
  doc["ldr_anomaly"] = g_status.ldrAnomaly;
  char buf[192];
  serializeJson(doc, buf, sizeof(buf));
  enqueueTx(TOPIC_SENSORS, buf);
}

// ===================== WiFi =====================
void setupWifi() {
  Serial.printf("[WiFi] Conectando a %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    vTaskDelay(pdMS_TO_TICKS(300));
    Serial.print(".");
  }
  Serial.printf("\n[WiFi] %s — IP: %s\n",
                WiFi.status() == WL_CONNECTED ? "OK" : "FALHA",
                WiFi.localIP().toString().c_str());

  // NTP para o agendamento de armamento (só após WiFi OK).
  if (WiFi.status() == WL_CONNECTED) {
    configTime(NTP_GMT_OFFSET_SEC, NTP_DST_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
    Serial.println("[NTP] configTime solicitado (pool.ntp.org / time.google.com)");
  }
}

void buildClientId() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(g_clientId, sizeof(g_clientId),
           "esp32-sec-%02X%02X%02X-%04X",
           mac[3], mac[4], mac[5],
           (uint16_t)esp_random());
}

// ===================== LDR =====================
int readLdrAverage() {
  long sum = 0;
  for (uint8_t i = 0; i < LDR_AVG_SAMPLES; i++) {
    sum += analogRead(PIN_LDR);
  }
  return sum / LDR_AVG_SAMPLES;
}

// Média de calibração define base e limiares de histerese do LDR.
void calibrateLdr() {
  Serial.println("[LDR] Calibrando...");
  long sum = 0;
  for (uint8_t i = 0; i < LDR_CAL_SAMPLES; i++) {
    sum += analogRead(PIN_LDR);
    vTaskDelay(pdMS_TO_TICKS(40));
  }
  g_ldrBase = sum / LDR_CAL_SAMPLES;
  g_ldrOn   = g_ldrBase + LDR_DELTA_ON;
  g_ldrOff  = g_ldrBase + LDR_DELTA_OFF;
  if (g_ldrOn  > 4095) g_ldrOn  = 4095;
  if (g_ldrOff > 4095) g_ldrOff = 4095;
  g_ldrAnomaly = false;
  g_ldrStart   = 0;
  Serial.printf("[LDR] Base=%d ON=%d OFF=%d\n",
                g_ldrBase, g_ldrOn, g_ldrOff);
}

// Só reporta anomalia depois de LDR_CONFIRM_MS acima do limiar (evita falso positivo).
bool validateLdrAnomaly(int reading) {
  uint32_t now = millis();
  if (reading > g_ldrOn) {
    if (!g_ldrAnomaly) {
      g_ldrAnomaly = true;
      g_ldrStart   = now;
    }
  } else if (reading < g_ldrOff) {
    g_ldrAnomaly = false;
  }
  return g_ldrAnomaly && (now - g_ldrStart >= LDR_CONFIRM_MS);
}

// ===================== MQTT callback =====================
// Aplica config de agendamento vinda do painel (tópico .../schedule).
void handleScheduleConfigMessage(const uint8_t* payload, unsigned int length) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload, length)) {
    Serial.println("[SCHED] JSON inválido");
    publishLog("schedule_invalid");
    return;
  }

  ScheduleConfig next;
  next.enabled       = doc["enabled"] | false;
  next.weekdaysOnly  = doc["weekdaysOnly"] | true;

  const char* armAt    = doc["armAt"] | "";
  const char* disarmAt = doc["disarmAt"] | "";
  if (!parseHourMinute(armAt, next.armMinutes) ||
      !parseHourMinute(disarmAt, next.disarmMinutes)) {
    Serial.println("[SCHED] Horário inválido");
    publishLog("schedule_invalid");
    return;
  }

  ScheduleConfig prev = copyScheduleConfig();
  bool configChanged =
      prev.enabled != next.enabled ||
      prev.armMinutes != next.armMinutes ||
      prev.disarmMinutes != next.disarmMinutes ||
      prev.weekdaysOnly != next.weekdaysOnly;

  xSemaphoreTake(statusMutex, portMAX_DELAY);
  g_schedule = next;
  xSemaphoreGive(statusMutex);

  nvsSaveSchedule(next);

  // Religar o agendamento (ou mudar a janela) limpa o override manual.
  if (!next.enabled || configChanged) {
    g_scheduleManualHold = false;
  }

  char armBuf[8];
  char disarmBuf[8];
  formatHourMinute(next.armMinutes, armBuf, sizeof(armBuf));
  formatHourMinute(next.disarmMinutes, disarmBuf, sizeof(disarmBuf));
  Serial.printf("[SCHED] saved %s-%s wd=%d en=%d\n",
                armBuf, disarmBuf, next.weekdaysOnly, next.enabled);

  publishSchedule();
  publishLog("schedule_saved");
  publishState();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("[MQTT] RX %s (%u)\n", topic, length);

  if (!strcmp(topic, TOPIC_SCHEDULE)) {
    handleScheduleConfigMessage(payload, length);
    return;
  }

  StaticJsonDocument<160> doc;
  if (deserializeJson(doc, payload, length)) return;

  const char* action = doc["action"] | "";
  Event evt{};

  if      (!strcmp(action, "arm"))         evt.type = EVT_CMD_ARM;
  else if (!strcmp(action, "disarm"))      evt.type = EVT_CMD_DISARM;
  else if (!strcmp(action, "reset_alarm")) evt.type = EVT_CMD_RESET;
  else if (!strcmp(action, "test"))        evt.type = EVT_CMD_TEST;
  else if (!strcmp(action, "recalibrate")) evt.type = EVT_CMD_RECAL;
  else {
    Serial.printf("[MQTT] ação desconhecida: %s\n", action);
    return;
  }
  xQueueSend(eventQueue, &evt, 0);
}

// ===================== Task: MQTT (core 0) =====================
void mqttTask(void* pv) {
  setupWifi();
  buildClientId();
  Serial.printf("[MQTT] ClientId = %s\n", g_clientId);

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(30);

  uint32_t lastReconnect = 0;
  uint32_t lastSensorPub = 0;
  uint32_t count = 0;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) { setupWifi(); lastReconnect = 0; }

    if (!mqttClient.connected()) {
      g_mqttConnected = false;

      if (millis() - lastReconnect > 3000) {
        lastReconnect = millis();
        Serial.printf("[MQTT] Tentativa #%lu ... ", (unsigned long)++count);

        if (mqttClient.connect(g_clientId, nullptr, nullptr,
                               TOPIC_STATUS, 0, true, "offline")) {
          Serial.println("OK");
          count = 0;
          g_mqttConnected = true;
          mqttClient.publish(TOPIC_STATUS, "online", true);
          mqttClient.subscribe(TOPIC_CMD);
          mqttClient.subscribe(TOPIC_SCHEDULE);
          publishState();
          publishSchedule();  // retained: painel reexibe a janela
          publishLog("boot");
        } else {
          Serial.printf("rc=%d\n", mqttClient.state());
        }
      }
    } else {
      mqttClient.loop();

      TxMessage m;
      while (xQueueReceive(txQueue, &m, 0) == pdTRUE) {
        mqttClient.publish(m.topic, m.payload, m.retain);
      }

      // Marca se o NTP já respondeu (alarmTask usa getLocalTime direto).
      struct tm probe;
      g_timeSynced = getLocalTime(&probe, 0);

      if (millis() - lastSensorPub > 2000) {
        lastSensorPub = millis();
        xSemaphoreTake(statusMutex, portMAX_DELAY);
        publishSensors();
        xSemaphoreGive(statusMutex);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ===================== Task: Sensores (core 1) =====================
uint16_t readDistanceCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  uint32_t dur = pulseIn(PIN_ECHO, HIGH, 30000);
  float cm = (dur == 0) ? 400.0f : (dur * 0.0343f) / 2.0f;
  if (cm < 2.0f || cm > 400.0f) cm = 400.0f;

  // Filtro de média móvel
  g_ultraSum -= g_ultraBuffer[g_ultraIdx];
  g_ultraBuffer[g_ultraIdx] = cm;
  g_ultraSum += cm;
  g_ultraIdx = (g_ultraIdx + 1) % ULTRA_SAMPLES;

  float media = g_ultraSum / ULTRA_SAMPLES;
  if (media >= 400.0f) return 999;
  return (uint16_t)media;
}

void sensorTask(void* pv) {
  pinMode(PIN_PIR,    INPUT);
  pinMode(PIN_TRIG,   OUTPUT);
  pinMode(PIN_ECHO,   INPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  analogReadResolution(12);

  uint32_t nearStart = 0;
  uint32_t lastButton = 0;
  bool lastButtonReading = HIGH;

  // Borda de transição: evita enfileirar PIR/LDR repetidamente enquanto o
  // sinal permanece ativo (fila de 16 estouraria e causaria falso alarme).
  bool pirPrev = false;
  bool ldrAnomalyPrev = false;

  for (;;) {
    // ─── Recalibração pedida pelo alarmTask (não bloqueia sirene/LED) ───
    if (g_ldrRecalRequested) {
      g_ldrRecalRequested = false;
      calibrateLdr();
      ldrAnomalyPrev = false;
      publishLog("recalibrated");
    }

    // ─── PIR ───
    bool pirNow = digitalRead(PIN_PIR) == HIGH;
    if (pirNow) {
      xSemaphoreTake(statusMutex, portMAX_DELAY);
      g_status.pir = true;
      g_status.lastPirMs = millis();
      xSemaphoreGive(statusMutex);

      // Só na borda quieto→ativo (não repete enquanto PIR fica alto)
      if (!pirPrev) {
        Event e{ EVT_PIR, 0 };
        xQueueSend(eventQueue, &e, 0);
      }
    } else {
      xSemaphoreTake(statusMutex, portMAX_DELAY);
      if (millis() - g_status.lastPirMs > PIR_HOLD_MS) g_status.pir = false;
      xSemaphoreGive(statusMutex);
    }
    pirPrev = pirNow;

    // ─── Distância ───
    uint16_t dist = readDistanceCm();
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    g_status.distance_cm = dist;
    xSemaphoreGive(statusMutex);

    if (dist < DIST_THRESHOLD_CM) {
      if (nearStart == 0) nearStart = millis();
      else if (millis() - nearStart > DIST_CONFIRM_MS) {
        Event e{ EVT_PROXIMITY, dist };
        xQueueSend(eventQueue, &e, 0);
        nearStart = millis();
      }
    } else {
      nearStart = 0;
    }

    // ─── LDR ───
    int ldr = readLdrAverage();
    bool anomaly = validateLdrAnomaly(ldr);

    xSemaphoreTake(statusMutex, portMAX_DELAY);
    g_status.ldr        = ldr;
    g_status.ldrAnomaly = anomaly;
    xSemaphoreGive(statusMutex);

    // Só na transição para anomalia confirmada (sem flood na fila)
    if (anomaly && !ldrAnomalyPrev) {
      Event e{ EVT_LDR, (uint16_t)ldr };
      xQueueSend(eventQueue, &e, 0);
    }
    ldrAnomalyPrev = anomaly;

    // ─── Botão ───
    bool b = digitalRead(PIN_BUTTON);
    if (b == LOW && lastButtonReading == HIGH &&
        millis() - lastButton > BUTTON_DEBOUNCE_MS) {
      Event e{ EVT_BUTTON, 0 };
      xQueueSend(eventQueue, &e, 0);
      lastButton = millis();
    }
    lastButtonReading = b;

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ===================== Task: Sirene (core 1) =====================
// Task independente que toca a sirene em ciclo infinito.
// Responde ao g_sirenActive em ~50ms.
void sirenTask(void* pv) {
  for (;;) {
    // ─── Desligada? Só espera ───
    if (!g_sirenActive) {
      ledcWriteTone(PIN_BUZZER_1, 0);
      ledcWriteTone(PIN_BUZZER_2, 0);
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // ─── Subida 400 → 1600 Hz ───
    for (int f = 400; f < 1600 && g_sirenActive; f += 30) {
      ledcWriteTone(PIN_BUZZER_1, f);
      ledcWriteTone(PIN_BUZZER_2, f + 20);
      vTaskDelay(pdMS_TO_TICKS(15));
    }

    // ─── Descida 1600 → 400 Hz ───
    for (int f = 1600; f > 400 && g_sirenActive; f -= 30) {
      ledcWriteTone(PIN_BUZZER_1, f);
      ledcWriteTone(PIN_BUZZER_2, f + 20);
      vTaskDelay(pdMS_TO_TICKS(15));
    }

    // ─── Pausa curta entre ciclos ───
    ledcWriteTone(PIN_BUZZER_1, 0);
    ledcWriteTone(PIN_BUZZER_2, 0);

    // Pausa "respirável": se desligar no meio, sai na hora
    for (int i = 0; i < 30 && g_sirenActive; i++) {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
}

// ===================== Task: Alarme (core 1) =====================
void playBeep(uint16_t freq, uint16_t ms) {
  ledcWriteTone(PIN_BUZZER_1, freq);
  ledcWriteTone(PIN_BUZZER_2, freq + 10);
  vTaskDelay(pdMS_TO_TICKS(ms));
  ledcWriteTone(PIN_BUZZER_1, 0);
  ledcWriteTone(PIN_BUZZER_2, 0);
}

// Transição para ARMADO (manual ou agendamento). Retorna true se mudou de estado.
bool requestArm(const char* reasonLog) {
  if (g_status.state != STATE_DISARMED) return false;
  g_status.state = STATE_ARMED;
  nvsSaveState(STATE_ARMED);
  drainEventQueue();
  publishLog(reasonLog);
  playBeep(1200, 150); vTaskDelay(pdMS_TO_TICKS(80));
  playBeep(1600, 200);
  return true;
}

// Transição para DESARMADO. Retorna true se mudou de estado.
bool requestDisarm(const char* reasonLog) {
  if (g_status.state == STATE_DISARMED) return false;
  g_status.state = STATE_DISARMED;
  nvsSaveState(STATE_DISARMED);
  drainEventQueue();
  g_sirenActive = false;
  digitalWrite(PIN_LED, LOW);
  publishLog(reasonLog);
  playBeep(1600, 150); vTaskDelay(pdMS_TO_TICKS(80));
  playBeep(1000, 250);
  return true;
}

// Desarma se ainda estiver armado/alarme no fim da janela de agendamento.
void applyScheduleExit() {
  if (g_status.state != STATE_DISARMED) {
    requestDisarm("schedule_disarmed");
  }
}

void alarmTask(void* pv) {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  Event evt;
  bool ledBlink = false;
  uint32_t lastBlink = 0;
  uint32_t alarmStart = 0;
  bool scheduleWasInside = false;
  bool scheduleTimeMissingLogged = false;

  for (;;) {
    // ─── Processa eventos (sempre responde em 50ms) ───
    if (xQueueReceive(eventQueue, &evt, pdMS_TO_TICKS(50)) == pdTRUE) {
      SystemState prev = g_status.state;
      bool logIt = false;
      const char* logMsg = nullptr;

      switch (evt.type) {

        case EVT_CMD_ARM:
          if (g_status.state == STATE_DISARMED) {
            // Armar manual limpa o hold: o usuário assumiu o controle.
            g_scheduleManualHold = false;
            requestArm("armed");
            logIt = false;  // requestArm já publica o log
          }
          break;

        case EVT_CMD_DISARM:
          if (g_status.state != STATE_DISARMED) {
            // Override: desarmar dentro da janela impede rearme até o fim dela.
            ScheduleConfig cfg = copyScheduleConfig();
            struct tm timeinfo;
            if (cfg.enabled && readLocalTime(timeinfo) &&
                isScheduleWindowActive(cfg, timeinfo)) {
              g_scheduleManualHold = true;
              Serial.println("[SCHED] override manual (hold até o fim da janela)");
            }
            requestDisarm("disarmed");
            logIt = false;
          }
          break;

        case EVT_CMD_RESET:
          if (g_status.state == STATE_ALARM) {
            g_status.state = STATE_ARMED;
            nvsSaveState(STATE_ARMED);
            drainEventQueue();
            logMsg = "alarm_reset"; logIt = true;
            g_sirenActive = false;              // ← desliga sirene PRIMEIRO
            digitalWrite(PIN_LED, LOW);
            playBeep(900, 100); vTaskDelay(pdMS_TO_TICKS(60));
            playBeep(900, 100);
          }
          break;

        case EVT_CMD_TEST:
          logMsg = "test"; logIt = true;
          playBeep(1000, 100); vTaskDelay(pdMS_TO_TICKS(80));
          playBeep(1500, 100); vTaskDelay(pdMS_TO_TICKS(80));
          playBeep(2000, 150);
          break;

        case EVT_CMD_RECAL:
          // Calibra no sensorTask (~2s) para não travar sirene/LED/eventos
          g_ldrRecalRequested = true;
          logMsg = "recalibrating"; logIt = true;
          Serial.println("[CMD] Recalibração LDR solicitada");
          playBeep(1200, 100); vTaskDelay(pdMS_TO_TICKS(60));
          playBeep(1500, 100);
          break;

        case EVT_BUTTON:
          if (g_status.state == STATE_DISARMED) {
            g_scheduleManualHold = false;
            requestArm("armed_button");
            logIt = false;
          } else {
            ScheduleConfig cfg = copyScheduleConfig();
            struct tm timeinfo;
            if (cfg.enabled && readLocalTime(timeinfo) &&
                isScheduleWindowActive(cfg, timeinfo)) {
              g_scheduleManualHold = true;
            }
            requestDisarm("disarmed_button");
            logIt = false;
          }
          break;

        case EVT_PIR:
          if (g_status.state == STATE_ARMED) {
            g_status.state = STATE_ALARM;
            nvsSaveState(STATE_ALARM);
            g_status.lastAlarmMs = millis();
            alarmStart = millis();
            logMsg = "alarm_pir"; logIt = true;
            publishAlarm("pir", 0);
          }
          break;

        case EVT_PROXIMITY:
          if (g_status.state == STATE_ARMED) {
            g_status.state = STATE_ALARM;
            nvsSaveState(STATE_ALARM);
            g_status.lastAlarmMs = millis();
            alarmStart = millis();
            logMsg = "alarm_proximity"; logIt = true;
            publishAlarm("proximity", evt.value);
          }
          break;

        case EVT_LDR:
          if (g_status.state == STATE_ARMED) {
            g_status.state = STATE_ALARM;
            nvsSaveState(STATE_ALARM);
            g_status.lastAlarmMs = millis();
            alarmStart = millis();
            logMsg = "alarm_ldr"; logIt = true;
            publishAlarm("ldr", evt.value);
          }
          break;
      }

      if (g_status.state != prev) publishState();
      if (logIt) { publishLog(logMsg); Serial.printf("[EVT] %s\n", logMsg); }
    }

    // ─── Agendamento: bordas de janela via NTP (não bloqueia o loop) ───
    {
      ScheduleConfig cfg = copyScheduleConfig();
      struct tm timeinfo;
      bool hasTime = readLocalTime(timeinfo);
      g_timeSynced = hasTime;

      bool inside = false;
      if (cfg.enabled && hasTime) {
        inside = isScheduleWindowActive(cfg, timeinfo);
      } else if (cfg.enabled && !hasTime && !scheduleTimeMissingLogged) {
        publishLog("schedule_time_missing");
        scheduleTimeMissingLogged = true;
        Serial.println("[SCHED] NTP ainda não sincronizado");
      }

      if (hasTime) scheduleTimeMissingLogged = false;

      // Borda de entrada na janela
      if (inside && !scheduleWasInside) {
        if (g_scheduleManualHold) {
          publishLog("schedule_skipped");
          Serial.println("[SCHED] início com hold manual — não rearmou");
        } else if (g_status.state == STATE_DISARMED) {
          requestArm("schedule_armed");
          publishState();
          Serial.println("[SCHED] ARM (agendamento)");
        }
      }

      // Borda de saída da janela (ou agendamento desligado / sem hora)
      if (!inside && scheduleWasInside) {
        g_scheduleManualHold = false;
        applyScheduleExit();
        publishState();
        Serial.println("[SCHED] DISARM (fim da janela)");
      }

      // Sem hora e agendamento ativo: se estava "dentro" no cache, trata como saída
      if (cfg.enabled && !hasTime && scheduleWasInside) {
        g_scheduleManualHold = false;
        applyScheduleExit();
        publishState();
        scheduleWasInside = false;
      }

      scheduleWasInside = inside;
    }

    // ─── Estado atual: apenas sinaliza para a sirenTask ───
    if (g_status.state == STATE_ALARM) {
      if (millis() - lastBlink > 200) {
        lastBlink = millis();
        ledBlink = !ledBlink;
        digitalWrite(PIN_LED, ledBlink ? HIGH : LOW);
      }
      g_sirenActive = true;

      // Auto-reset
      if (millis() - alarmStart > ALARM_AUTO_RESET_MS) {
        g_status.state = STATE_ARMED;
        nvsSaveState(STATE_ARMED);
        drainEventQueue();
        g_sirenActive = false;                // ← desliga sirene
        digitalWrite(PIN_LED, LOW);
        publishState();
        publishLog("alarm_timeout");
      }
      vTaskDelay(pdMS_TO_TICKS(20));
    } else {
      g_sirenActive = false;
      digitalWrite(PIN_LED, LOW);
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

// ===================== Task: Display OLED (core 0) =====================
void displayTask(void* pv) {
  Wire.begin(21, 22);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[OLED] Falha ao iniciar");
    vTaskDelete(NULL);
  }
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println("Seguranca Patrim.");
  oled.display();

  for (;;) {
    SystemState st;
    bool pir;
    uint16_t dist;
    int ldr;
    bool ldrAnom;

    xSemaphoreTake(statusMutex, portMAX_DELAY);
    st      = g_status.state;
    pir     = g_status.pir;
    dist    = g_status.distance_cm;
    ldr     = g_status.ldr;
    ldrAnom = g_status.ldrAnomaly;
    xSemaphoreGive(statusMutex);

    // Flag volatile: não chama PubSubClient de outra task
    bool mqttOk = g_mqttConnected;

    oled.clearDisplay();
    oled.setTextSize(2);
    oled.setCursor(0, 0);
    switch (st) {
      case STATE_DISARMED: oled.println("DESARM.");  break;
      case STATE_ARMED:    oled.println("ARMADO");   break;
      case STATE_ALARM:    oled.println("INVASAO!"); break;
    }

    oled.setTextSize(1);
    oled.setCursor(0, 22);
    oled.print("Dist: ");
    if (dist >= 999) oled.println("--- cm");
    else             { oled.print(dist); oled.println(" cm"); }

    oled.setCursor(0, 32);
    oled.print("PIR: ");
    oled.println(pir ? "ATIVO" : "quieto");

    oled.setCursor(0, 42);
    oled.print("LDR: ");
    oled.print(ldr);
    oled.println(ldrAnom ? " !" : "");

    oled.setCursor(0, 54);
    oled.print("W:");
    oled.print(WiFi.status() == WL_CONNECTED ? "OK" : "--");
    oled.print(" M:");
    oled.print(mqttOk ? "OK" : "--");
    oled.print(" S:");
    // Schedule: A=ativo, O=override, -=ocioso (flag volatile lida sem mutex extra)
    if (g_scheduleManualHold) oled.print("O");
    else if (g_timeSynced && g_schedule.enabled) {
      struct tm probe;
      if (readLocalTime(probe) && isScheduleWindowActive(g_schedule, probe)) oled.print("A");
      else oled.print("-");
    } else oled.print("-");

    oled.display();
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// ===================== setup / loop =====================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Sistema de Segurança Patrimonial v3.3 (agendamento) ===");

  statusMutex = xSemaphoreCreateMutex();
  eventQueue  = xQueueCreate(16, sizeof(Event));
  txQueue     = xQueueCreate(16, sizeof(TxMessage));

  if (!statusMutex || !eventQueue || !txQueue) {
    Serial.println("Falha ao criar primitivas RTOS");
    ESP.restart();
  }

  nvsLoad();
  nvsLoadSchedule();
  Serial.printf("[NVS] Estado restaurado: %d\n", (int)g_status.state);

  // LEDC configurado UMA vez no setup (fora das tasks)
  ledcAttach(PIN_BUZZER_1, 2000, 10);
  ledcAttach(PIN_BUZZER_2, 2000, 10);

  calibrateLdr();

  xTaskCreatePinnedToCore(mqttTask,    "MQTT",    6144, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(displayTask, "Display", 4096, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(sensorTask,  "Sensor",  4096, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(alarmTask,   "Alarm",   4096, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(sirenTask,   "Siren",   3072, nullptr, 2, nullptr, 1);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
