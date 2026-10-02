/*
  ==================================================================
  SISTEMA DE SEGURANÇA PATRIMONIAL — ESP32 + MQTT + FreeRTOS
  ==================================================================
  Versão v3.1 — Sirene non-blocking + correções de robustez
  ------------------------------------------------------------------
  Sensores : PIR (16) + HC-SR04 (TRIG 5 / ECHO 18) + LDR (34)
  Atuadores: Buzzer 1 (19), Buzzer 2 (23), LED (2)
  Display  : OLED SSD1306 I2C (SDA 21 / SCL 22)
  Entrada  : Botão (4, INPUT_PULLUP)
  Rede     : Wokwi-GUEST
  Broker   : broker.hivemq.com (TCP 1883)
  NVS      : estado persistente

  Arquitetura: 5 tasks FreeRTOS em 2 cores
    mqttTask    (core 0) → WiFi + MQTT (publish/subscribe)
    displayTask (core 0) → OLED a cada 250ms
    sensorTask  (core 1) → PIR, HC-SR04, LDR, botão → eventQueue
    alarmTask   (core 1) → máquina de estados + LED
    sirenTask   (core 1) → sirene contínua controlada por flag

  Melhorias principais:
    - Sirene em task separada (resposta < 50ms a comandos)
    - Média móvel no HC-SR04 (elimina ruído)
    - LDR com calibração automática + histerese
    - Persistência NVS do estado
    - PIR/LDR enfileiram apenas na borda de transição (sem flood)
    - Fila de eventos drenada ao mudar de estado (evita falso alarme)
    - Recalibração do LDR fora do alarmTask (não bloqueia sirene/LED)
    - Status MQTT compartilhado via flag volatile (PubSubClient não é thread-safe)
  ==================================================================
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ===================== Configurações =====================
constexpr char WIFI_SSID[]     = "Wokwi-GUEST";
constexpr char WIFI_PASSWORD[] = "";
constexpr char MQTT_SERVER[]   = "broker.hivemq.com";
constexpr uint16_t MQTT_PORT   = 1883;

// ⚠️ Troque "meu-esp32" por algo único
constexpr char TOPIC_BASE[]    = "seguranca/patrimonio/meu-esp32";
constexpr char TOPIC_CMD[]     = "seguranca/patrimonio/meu-esp32/cmd";
constexpr char TOPIC_STATE[]   = "seguranca/patrimonio/meu-esp32/state";
constexpr char TOPIC_ALARM[]   = "seguranca/patrimonio/meu-esp32/alarm";
constexpr char TOPIC_LOG[]     = "seguranca/patrimonio/meu-esp32/log";
constexpr char TOPIC_SENSORS[] = "seguranca/patrimonio/meu-esp32/sensors";
constexpr char TOPIC_STATUS[]  = "seguranca/patrimonio/meu-esp32/status";

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

// ===================== Fila TX =====================
void enqueueTx(const char* topic, const char* payload) {
  TxMessage m{};
  strncpy(m.topic, topic, sizeof(m.topic) - 1);
  strncpy(m.payload, payload, sizeof(m.payload) - 1);
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
  StaticJsonDocument<128> doc;
  doc["armed"] = (g_status.state != STATE_DISARMED);
  doc["alarm"] = (g_status.state == STATE_ALARM);
  char buf[128];
  serializeJson(doc, buf, sizeof(buf));
  enqueueTx(TOPIC_STATE, buf);
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
void setupWiFi() {
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
int readLDRAvg() {
  long soma = 0;
  for (uint8_t i = 0; i < LDR_AVG_SAMPLES; i++) {
    soma += analogRead(PIN_LDR);
  }
  return soma / LDR_AVG_SAMPLES;
}

void calibrarLDR() {
  Serial.println("[LDR] Calibrando...");
  long soma = 0;
  for (uint8_t i = 0; i < LDR_CAL_SAMPLES; i++) {
    soma += analogRead(PIN_LDR);
    vTaskDelay(pdMS_TO_TICKS(40));
  }
  g_ldrBase = soma / LDR_CAL_SAMPLES;
  g_ldrOn   = g_ldrBase + LDR_DELTA_ON;
  g_ldrOff  = g_ldrBase + LDR_DELTA_OFF;
  if (g_ldrOn  > 4095) g_ldrOn  = 4095;
  if (g_ldrOff > 4095) g_ldrOff = 4095;
  g_ldrAnomaly = false;
  g_ldrStart   = 0;
  Serial.printf("[LDR] Base=%d ON=%d OFF=%d\n",
                g_ldrBase, g_ldrOn, g_ldrOff);
}

bool validarAnomaliaLDR(int leitura) {
  uint32_t now = millis();
  if (leitura > g_ldrOn) {
    if (!g_ldrAnomaly) {
      g_ldrAnomaly = true;
      g_ldrStart   = now;
    }
  } else if (leitura < g_ldrOff) {
    g_ldrAnomaly = false;
  }
  return g_ldrAnomaly && (now - g_ldrStart >= LDR_CONFIRM_MS);
}

// ===================== MQTT callback =====================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("[MQTT] RX %s (%u)\n", topic, length);
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
  setupWiFi();
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
    if (WiFi.status() != WL_CONNECTED) { setupWiFi(); lastReconnect = 0; }

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
          publishState();
          publishLog("boot");
        } else {
          Serial.printf("rc=%d\n", mqttClient.state());
        }
      }
    } else {
      mqttClient.loop();

      TxMessage m;
      while (xQueueReceive(txQueue, &m, 0) == pdTRUE) {
        mqttClient.publish(m.topic, m.payload);
      }

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
      calibrarLDR();
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
    int ldr = readLDRAvg();
    bool anomaly = validarAnomaliaLDR(ldr);

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

void alarmTask(void* pv) {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  Event evt;
  bool ledBlink = false;
  uint32_t lastBlink = 0;
  uint32_t alarmStart = 0;

  for (;;) {
    // ─── Processa eventos (sempre responde em 50ms) ───
    if (xQueueReceive(eventQueue, &evt, pdMS_TO_TICKS(50)) == pdTRUE) {
      SystemState prev = g_status.state;
      bool logIt = false;
      const char* logMsg = nullptr;

      switch (evt.type) {

        case EVT_CMD_ARM:
          if (g_status.state == STATE_DISARMED) {
            g_status.state = STATE_ARMED;
            nvsSaveState(STATE_ARMED);
            // Remove eventos velhos (PIR/LDR ainda ativos do período desarmado)
            drainEventQueue();
            logMsg = "armed"; logIt = true;
            playBeep(1200, 150); vTaskDelay(pdMS_TO_TICKS(80));
            playBeep(1600, 200);
          }
          break;

        case EVT_CMD_DISARM:
          if (g_status.state != STATE_DISARMED) {
            g_status.state = STATE_DISARMED;
            nvsSaveState(STATE_DISARMED);
            drainEventQueue();
            logMsg = "disarmed"; logIt = true;
            g_sirenActive = false;              // ← desliga sirene
            playBeep(1600, 150); vTaskDelay(pdMS_TO_TICKS(80));
            playBeep(1000, 250);
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
            g_status.state = STATE_ARMED;
            nvsSaveState(STATE_ARMED);
            drainEventQueue();
            logMsg = "armed_button"; logIt = true;
            playBeep(1200, 150); vTaskDelay(pdMS_TO_TICKS(80));
            playBeep(1600, 200);
          } else {
            // Qualquer outro estado → desarma e silencia
            g_status.state = STATE_DISARMED;
            nvsSaveState(STATE_DISARMED);
            drainEventQueue();
            logMsg = "disarmed_button"; logIt = true;
            g_sirenActive = false;              // ← desliga sirene
            digitalWrite(PIN_LED, LOW);
            playBeep(1600, 150); vTaskDelay(pdMS_TO_TICKS(80));
            playBeep(1000, 250);
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

    oled.display();
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// ===================== setup / loop =====================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Sistema de Segurança Patrimonial v3.1 ===");

  statusMutex = xSemaphoreCreateMutex();
  eventQueue  = xQueueCreate(16, sizeof(Event));
  txQueue     = xQueueCreate(16, sizeof(TxMessage));

  if (!statusMutex || !eventQueue || !txQueue) {
    Serial.println("Falha ao criar primitivas RTOS");
    ESP.restart();
  }

  nvsLoad();
  Serial.printf("[NVS] Estado restaurado: %d\n", (int)g_status.state);

  // LEDC configurado UMA vez no setup (fora das tasks)
  ledcAttach(PIN_BUZZER_1, 2000, 10);
  ledcAttach(PIN_BUZZER_2, 2000, 10);

  calibrarLDR();

  xTaskCreatePinnedToCore(mqttTask,    "MQTT",    6144, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(displayTask, "Display", 4096, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(sensorTask,  "Sensor",  4096, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(alarmTask,   "Alarm",   4096, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(sirenTask,   "Siren",   3072, nullptr, 2, nullptr, 1);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
