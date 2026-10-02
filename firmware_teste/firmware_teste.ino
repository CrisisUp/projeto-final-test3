/*
  ==================================================================
  FIRMWARE DE TESTE — Validar o dashboard no Wokwi
  ==================================================================
  Papel: simular o ESP32 "de verdade" com o MESMO contrato MQTT do
  sketch.ino de produção, sem OLED nem FreeRTOS complexo.

  Objetivo: provar que o painel web atualiza e que os comandos
  do dashboard chegam ao firmware (e vice-versa), incluindo o
  agendamento de armamento (.../schedule + NTP).

  Hardware mínimo (Wokwi):
    - ESP32 DevKit
    - Botão no GPIO 4 (INPUT_PULLUP) — opcional, para controle local
    - LED no GPIO 2 — opcional, feedback visual

  Rede: Wokwi-GUEST  |  Broker: broker.hivemq.com:1883
  Tópicos: seguranca/patrimonio/meu-esp32/...
  NTP: pool.ntp.org (UTC-3)

  Comportamento do teste:
    - Publica .../status  = online (LWT = offline)
    - Publica .../state   ao conectar e em cada mudança
    - Publica .../sensors a cada 2 s com valores variados (cores mudam)
    - Publica .../schedule retained (config ativa)
    - Responde .../cmd: arm, disarm, reset_alarm, test, recalibrate
    - Recebe .../schedule e aplica bordas de janela via NTP
    - Com sistema ARMADO, dispara alarme automático após ~10 s
    - Botão local: DESARMADO → ARMADO → ALARME → DESARMADO
    - Serial 115200 mostra tudo que entra/sai
  ==================================================================
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ctype.h>

// ===================== Configurações =====================
constexpr char WIFI_SSID[]     = "Wokwi-GUEST";
constexpr char WIFI_PASSWORD[] = "";
constexpr char MQTT_SERVER[]   = "broker.hivemq.com";
constexpr uint16_t MQTT_PORT   = 1883;

// ⚠️ Fonte única — deve ser EXATAMENTE o mesmo prefixo do dashboard (app.js)
#define SECURITY_TOPIC_PREFIX "seguranca/patrimonio/meu-esp32"

constexpr char TOPIC_CMD[]      = SECURITY_TOPIC_PREFIX "/cmd";
constexpr char TOPIC_STATE[]    = SECURITY_TOPIC_PREFIX "/state";
constexpr char TOPIC_ALARM[]    = SECURITY_TOPIC_PREFIX "/alarm";
constexpr char TOPIC_LOG[]      = SECURITY_TOPIC_PREFIX "/log";
constexpr char TOPIC_SENSORS[]  = SECURITY_TOPIC_PREFIX "/sensors";
constexpr char TOPIC_STATUS[]   = SECURITY_TOPIC_PREFIX "/status";
constexpr char TOPIC_SCHEDULE[] = SECURITY_TOPIC_PREFIX "/schedule";

// NTP — America/Sao_Paulo sem DST (UTC-3)
constexpr int32_t NTP_GMT_OFFSET_SEC = -3 * 3600;
constexpr char NTP_SERVER_1[] = "pool.ntp.org";
constexpr char NTP_SERVER_2[] = "time.google.com";

constexpr uint16_t SCHEDULE_DEFAULT_ARM_MIN    = 18 * 60;
constexpr uint16_t SCHEDULE_DEFAULT_DISARM_MIN = 8 * 60;

constexpr int PIN_BUTTON = 4;
constexpr int PIN_LED    = 2;

constexpr uint32_t SENSOR_PUBLISH_MS = 2000;
constexpr uint32_t AUTO_ALARM_MS     = 10000;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 300;

// ===================== Estado =====================
enum SystemState : uint8_t {
  STATE_DISARMED = 0,
  STATE_ARMED    = 1,
  STATE_ALARM    = 2
};

struct ScheduleConfig {
  bool     enabled       = false;
  uint16_t armMinutes    = SCHEDULE_DEFAULT_ARM_MIN;
  uint16_t disarmMinutes = SCHEDULE_DEFAULT_DISARM_MIN;
  bool     weekdaysOnly  = true;
};

SystemState g_state = STATE_DISARMED;
ScheduleConfig g_schedule;
bool g_scheduleManualHold = false;
bool g_scheduleWasInside  = false;
bool g_timeSynced         = false;
uint32_t g_lastSchedDebugMs = 0;

int      g_distance = 200;
bool     g_pir      = false;
int      g_ldr      = 800;
bool     g_ldrAnomaly = false;

uint32_t g_armTimeMs = 0;
uint32_t g_lastSensorPub = 0;
uint32_t g_lastButtonMs  = 0;
bool     g_lastButtonReading = HIGH;

const int DISTANCE_CYCLE[] = { 200, 150, 80, 35, 25, 80, 150, 200 };
constexpr uint8_t DISTANCE_CYCLE_LEN = sizeof(DISTANCE_CYCLE) / sizeof(DISTANCE_CYCLE[0]);
uint8_t g_distanceIdx = 0;

WiFiClient   espClient;
PubSubClient mqttClient(espClient);
char g_clientId[40] = {0};

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
  return t.tm_wday >= 1 && t.tm_wday <= 5;
}

bool isScheduleWindowActive(const ScheduleConfig& cfg, const struct tm& t) {
  if (!cfg.enabled) return false;
  if (cfg.weekdaysOnly && !isWeekday(t)) return false;

  uint16_t nowMin = (uint16_t)(t.tm_hour * 60 + t.tm_min);
  if (cfg.armMinutes == cfg.disarmMinutes) return nowMin == cfg.armMinutes;
  if (cfg.armMinutes > cfg.disarmMinutes) {
    return nowMin >= cfg.armMinutes || nowMin < cfg.disarmMinutes;
  }
  return nowMin >= cfg.armMinutes && nowMin < cfg.disarmMinutes;
}

const char* scheduleLabel(const ScheduleConfig& cfg, const struct tm* t, bool hasTime) {
  if (!cfg.enabled || !hasTime || !t) return "idle";
  if (!isScheduleWindowActive(cfg, *t)) return "idle";
  return g_scheduleManualHold ? "override" : "active";
}

// ===================== Publicação =====================
void publishLog(const char* event) {
  StaticJsonDocument<128> doc;
  doc["event"] = event;
  doc["state"] = (int)g_state;
  doc["ts"]    = millis();
  char buf[128];
  serializeJson(doc, buf, sizeof(buf));
  mqttClient.publish(TOPIC_LOG, buf);
  Serial.printf("[LOG] %s\n", event);
}

void publishState() {
  StaticJsonDocument<192> doc;
  doc["armed"] = (g_state != STATE_DISARMED);
  doc["alarm"] = (g_state == STATE_ALARM);

  struct tm timeinfo;
  bool hasTime = getLocalTime(&timeinfo, 0);
  const char* schedLabel = scheduleLabel(g_schedule, hasTime ? &timeinfo : nullptr, hasTime);
  doc["schedule"] = schedLabel;
  doc["ntp"] = hasTime;

  char buf[192];
  serializeJson(doc, buf, sizeof(buf));
  mqttClient.publish(TOPIC_STATE, buf);
  Serial.printf("[STATE] armed=%d alarm=%d schedule=%s ntp=%d\n",
                g_state != STATE_DISARMED, g_state == STATE_ALARM,
                schedLabel, hasTime);
}

void publishAlarm(const char* type, uint16_t value) {
  StaticJsonDocument<96> doc;
  doc["type"]  = type;
  doc["value"] = value;
  doc["ts"]    = millis();
  char buf[96];
  serializeJson(doc, buf, sizeof(buf));
  mqttClient.publish(TOPIC_ALARM, buf);
  Serial.printf("[ALARM] type=%s value=%u\n", type, value);
}

void publishSensors() {
  StaticJsonDocument<128> doc;
  doc["pir"]          = g_pir;
  doc["dist"]         = g_distance;
  doc["ldr"]          = g_ldr;
  doc["ldr_anomaly"]  = g_ldrAnomaly;
  char buf[128];
  serializeJson(doc, buf, sizeof(buf));
  mqttClient.publish(TOPIC_SENSORS, buf);
}

void publishSchedule() {
  StaticJsonDocument<192> doc;
  char armBuf[8];
  char disarmBuf[8];
  formatHourMinute(g_schedule.armMinutes, armBuf, sizeof(armBuf));
  formatHourMinute(g_schedule.disarmMinutes, disarmBuf, sizeof(disarmBuf));

  doc["enabled"]      = g_schedule.enabled;
  doc["armAt"]        = armBuf;
  doc["disarmAt"]     = disarmBuf;
  doc["weekdaysOnly"] = g_schedule.weekdaysOnly;

  char buf[192];
  serializeJson(doc, buf, sizeof(buf));
  mqttClient.publish(TOPIC_SCHEDULE, buf, true);
}

// ===================== Máquina de estados =====================
void setState(SystemState next, const char* reason) {
  if (next == g_state) return;

  g_state = next;

  if (next == STATE_ARMED) {
    g_armTimeMs = millis();
    digitalWrite(PIN_LED, LOW);
  } else if (next == STATE_ALARM) {
    digitalWrite(PIN_LED, HIGH);
  } else {
    digitalWrite(PIN_LED, LOW);
  }

  publishState();
  publishLog(reason);
}

void triggerAlarm(const char* type, uint16_t value) {
  if (g_state != STATE_ARMED) return;
  setState(STATE_ALARM, (String("alarm_") + type).c_str());
  publishAlarm(type, value);
}

void beep(uint16_t freq, uint16_t ms) {
  Serial.printf("[BEEP] %u Hz / %u ms\n", freq, ms);
  digitalWrite(PIN_LED, HIGH);
  delay(ms);
  digitalWrite(PIN_LED, g_state == STATE_ALARM ? HIGH : LOW);
}

// Override: desarmar dentro da janela segura até o fim dela.
void markManualHoldIfInWindow() {
  struct tm timeinfo;
  if (g_schedule.enabled && getLocalTime(&timeinfo, 0) &&
      isScheduleWindowActive(g_schedule, timeinfo)) {
    g_scheduleManualHold = true;
    Serial.println("[SCHED] override manual (hold até o fim da janela)");
  }
}

// Borda de janela via NTP (chamado no loop e após salvar config).
void evaluateSchedule() {
  struct tm timeinfo;
  bool hasTime = getLocalTime(&timeinfo, 0);

  // Publica estado quando o NTP passa a responder (indicador no painel).
  if (hasTime && !g_timeSynced) {
    g_timeSynced = true;
    g_scheduleWasInside = false;  // força reavaliar a janela com hora válida
    publishState();
    publishLog("ntp_synced");
    Serial.printf("[NTP] Relógio civil sincronizado: %02d:%02d:%02d (wday=%d)\n",
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, timeinfo.tm_wday);
  }

  bool inside = false;
  if (g_schedule.enabled && hasTime) {
    inside = isScheduleWindowActive(g_schedule, timeinfo);
  }

  if (inside && !g_scheduleWasInside) {
    if (g_scheduleManualHold) {
      publishLog("schedule_skipped");
      Serial.println("[SCHED] hold manual — não rearmou");
    } else if (g_state == STATE_DISARMED) {
      setState(STATE_ARMED, "schedule_armed");
      beep(1200, 120);
      beep(1600, 180);
      Serial.println("[SCHED] ARM (agendamento)");
    }
  }

  if (!inside && g_scheduleWasInside) {
    g_scheduleManualHold = false;
    if (g_state != STATE_DISARMED) {
      setState(STATE_DISARMED, "schedule_disarmed");
      beep(1600, 120);
      beep(1000, 200);
      Serial.println("[SCHED] DISARM (fim da janela)");
    } else {
      publishState();
    }
  }

  // Debug periódico no Serial
  if (hasTime && (millis() / 5000) != (g_lastSchedDebugMs / 5000)) {
    g_lastSchedDebugMs = millis();
    char armBuf[8], disarmBuf[8], nowBuf[8];
    formatHourMinute(g_schedule.armMinutes, armBuf, sizeof(armBuf));
    formatHourMinute(g_schedule.disarmMinutes, disarmBuf, sizeof(disarmBuf));
    formatHourMinute((uint16_t)(timeinfo.tm_hour * 60 + timeinfo.tm_min), nowBuf, sizeof(nowBuf));
    Serial.printf("[SCHED] now=%s win=%s-%s en=%d wd=%d inside=%d hold=%d state=%d\n",
                  nowBuf, armBuf, disarmBuf, g_schedule.enabled, g_schedule.weekdaysOnly,
                  inside, g_scheduleManualHold, (int)g_state);
  }

  g_scheduleWasInside = inside;
}

// ===================== Sensores simulados =====================
void updateSimulatedSensors() {
  g_distanceIdx = (g_distanceIdx + 1) % DISTANCE_CYCLE_LEN;
  g_distance = DISTANCE_CYCLE[g_distanceIdx];
  g_pir = (g_distanceIdx % 3 == 0);
  g_ldr = 400 + (g_distanceIdx * 150) % 2000;
  g_ldrAnomaly = (g_distanceIdx == 5);

  if (g_state == STATE_ARMED) {
    if (g_distance < 40) {
      triggerAlarm("proximity", (uint16_t)g_distance);
    } else if (g_pir) {
      triggerAlarm("pir", 0);
    } else if (g_ldrAnomaly) {
      triggerAlarm("ldr", (uint16_t)g_ldr);
    }
  }

  publishSensors();
}

// ===================== Comandos MQTT =====================
void handleCommand(const char* action) {
  Serial.printf("[CMD] action=%s\n", action);

  if (!strcmp(action, "arm")) {
    if (g_state == STATE_DISARMED) {
      g_scheduleManualHold = false;
      setState(STATE_ARMED, "armed");
      beep(1200, 120);
      beep(1600, 180);
    } else {
      publishLog("arm_ignored");
    }
  } else if (!strcmp(action, "disarm")) {
    if (g_state != STATE_DISARMED) {
      markManualHoldIfInWindow();
      setState(STATE_DISARMED, "disarmed");
      beep(1600, 120);
      beep(1000, 200);
    } else {
      publishLog("disarm_ignored");
    }
  } else if (!strcmp(action, "reset_alarm")) {
    if (g_state == STATE_ALARM) {
      setState(STATE_ARMED, "alarm_reset");
      beep(900, 100);
      beep(900, 100);
    } else {
      publishLog("reset_ignored");
    }
  } else if (!strcmp(action, "test")) {
    publishLog("test");
    beep(1000, 80);
    beep(1500, 80);
    beep(2000, 120);
  } else if (!strcmp(action, "recalibrate")) {
    publishLog("recalibrating");
    delay(200);
    g_ldr = 900;
    g_ldrAnomaly = false;
    publishLog("recalibrated");
  } else {
    Serial.printf("[CMD] ação desconhecida: %s\n", action);
    publishLog("cmd_unknown");
  }
}

void handleScheduleConfigMessage(const uint8_t* payload, unsigned int length) {
  // Payload bruto ajuda a achar quem publica en=0 no broker público.
  char raw[96];
  size_t n = length < sizeof(raw) - 1 ? length : sizeof(raw) - 1;
  memcpy(raw, payload, n);
  raw[n] = '\0';
  Serial.printf("[SCHED] RX raw (%u): %s\n", length, raw);

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload, length)) {
    Serial.println("[SCHED] JSON inválido");
    publishLog("schedule_invalid");
    return;
  }

  ScheduleConfig next;
  next.enabled      = doc["enabled"] | false;
  next.weekdaysOnly = doc["weekdaysOnly"] | true;

  const char* armAt    = doc["armAt"] | "";
  const char* disarmAt = doc["disarmAt"] | "";
  if (!parseHourMinute(armAt, next.armMinutes) ||
      !parseHourMinute(disarmAt, next.disarmMinutes)) {
    Serial.println("[SCHED] Horário inválido");
    publishLog("schedule_invalid");
    return;
  }

  bool configChanged =
      g_schedule.enabled != next.enabled ||
      g_schedule.armMinutes != next.armMinutes ||
      g_schedule.disarmMinutes != next.disarmMinutes ||
      g_schedule.weekdaysOnly != next.weekdaysOnly;

  // Config idêntica: não regrava nem re-broadcasta (evita loop se outro
  // cliente/simulação republicar o mesmo retained no prefixo público).
  if (!configChanged) {
    Serial.println("[SCHED] config igual — ignorado (sem republicar)");
    return;
  }

  g_schedule = next;
  if (!next.enabled || configChanged) {
    g_scheduleManualHold = false;
  }

  // Reavaliar já: save "tarde" (janela já começou) deve armar na hora.
  g_scheduleWasInside = false;
  evaluateSchedule();

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
  Serial.printf("[MQTT] RX %s (%u bytes)\n", topic, length);

  if (!strcmp(topic, TOPIC_SCHEDULE)) {
    handleScheduleConfigMessage(payload, length);
    return;
  }

  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, payload, length)) {
    Serial.println("[MQTT] JSON inválido");
    return;
  }

  const char* action = doc["action"] | "";
  if (action[0] != '\0') {
    handleCommand(action);
  }
}

// ===================== WiFi / MQTT =====================
void setupWiFi() {
  Serial.printf("[WiFi] Conectando a %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
    Serial.print(".");
  }
  Serial.printf("\n[WiFi] %s — IP: %s\n",
                WiFi.status() == WL_CONNECTED ? "OK" : "FALHA",
                WiFi.localIP().toString().c_str());

  if (WiFi.status() == WL_CONNECTED) {
    setenv("TZ", "BRT3", 1);
    tzset();
    configTime(NTP_GMT_OFFSET_SEC, 0, NTP_SERVER_1, NTP_SERVER_2);
    Serial.println("[NTP] configTime solicitado (TZ BRT3)");
  }
}

void buildClientId() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(g_clientId, sizeof(g_clientId),
           "esp32-test-%02X%02X%02X-%04X",
           mac[3], mac[4], mac[5],
           (uint16_t)esp_random());
}

bool connectMqtt() {
  if (mqttClient.connected()) return true;

  Serial.print("[MQTT] Conectando...");
  buildClientId();

  if (mqttClient.connect(g_clientId, nullptr, nullptr,
                         TOPIC_STATUS, 0, true, "offline")) {
    Serial.println("OK");
    mqttClient.publish(TOPIC_STATUS, "online", true);
    mqttClient.subscribe(TOPIC_CMD);
    mqttClient.subscribe(TOPIC_SCHEDULE);
    publishState();
    publishSchedule();
    publishLog("boot");
    publishSensors();
    return true;
  }

  Serial.printf("FALHA rc=%d\n", mqttClient.state());
  return false;
}

// ===================== Botão local =====================
void pollButton() {
  bool reading = digitalRead(PIN_BUTTON);
  uint32_t now = millis();

  if (reading == LOW && g_lastButtonReading == HIGH &&
      now - g_lastButtonMs > BUTTON_DEBOUNCE_MS) {
    g_lastButtonMs = now;

    if (g_state == STATE_DISARMED) {
      g_scheduleManualHold = false;
      setState(STATE_ARMED, "armed_button");
      beep(1200, 120);
    } else if (g_state == STATE_ARMED) {
      triggerAlarm("button", 0);
    } else {
      markManualHoldIfInWindow();
      setState(STATE_DISARMED, "disarmed_button");
      beep(1600, 150);
    }
  }

  g_lastButtonReading = reading;
}

// ===================== setup / loop =====================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Firmware de TESTE — Segurança Patrimonial (agendamento) ===");
  Serial.println("Valida o painel web via MQTT (sem OLED / FreeRTOS)");

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  setupWiFi();

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(256);

  connectMqtt();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    setupWiFi();
  }

  if (!mqttClient.connected()) {
    connectMqtt();
  }
  mqttClient.loop();

  evaluateSchedule();

  if (millis() - g_lastSensorPub >= SENSOR_PUBLISH_MS) {
    g_lastSensorPub = millis();
    updateSimulatedSensors();
  }

  if (g_state == STATE_ARMED && millis() - g_armTimeMs >= AUTO_ALARM_MS) {
    Serial.println("[TESTE] Auto-alarme de demonstração");
    triggerAlarm("pir", 0);
  }

  pollButton();

  if (g_state == STATE_ALARM) {
    digitalWrite(PIN_LED, (millis() / 200) % 2 ? HIGH : LOW);
  }

  delay(20);
}
