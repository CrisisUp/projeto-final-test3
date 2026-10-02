/*
  ==================================================================
  FIRMWARE DE TESTE — Validar o dashboard no Wokwi
  ==================================================================
  Papel: simular o ESP32 "de verdade" com o MESMO contrato MQTT do
  sketch.ino de produção, sem OLED nem FreeRTOS complexo.

  Objetivo: provar que o painel web atualiza e que os comandos
  do dashboard chegam ao firmware (e vice-versa).

  Hardware mínimo (Wokwi):
    - ESP32 DevKit
    - Botão no GPIO 4 (INPUT_PULLUP) — opcional, para controle local
    - LED no GPIO 2 — opcional, feedback visual

  Rede: Wokwi-GUEST  |  Broker: broker.hivemq.com:1883
  Tópicos: seguranca/patrimonio/meu-esp32/...

  Como usar no Wokwi:
    1. Abra o projeto Wokwi do painel (ou crie um com ESP32)
    2. Cole este código no arquivo do sketch (substituindo o de produção,
       ou use um sketch separado só para o teste)
    3. Inicie a simulação
    4. Abra o index.html e observe: conexão, sensores e comandos

  Comportamento do teste:
    - Publica .../status  = online (LWT = offline)
    - Publica .../state   ao conectar e em cada mudança
    - Publica .../sensors a cada 2 s com valores variados (cores mudam)
    - Responde .../cmd: arm, disarm, reset_alarm, test, recalibrate
    - Com sistema ARMADO, dispara alarme automático após ~10 s
      (simula intrusão — o painel deve ir para INVASÃO)
    - Botão local: DESARMADO → ARMADO → ALARME → DESARMADO
    - Serial 115200 mostra tudo que entra/sai
  ==================================================================
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ===================== Configurações =====================
constexpr char WIFI_SSID[]     = "Wokwi-GUEST";
constexpr char WIFI_PASSWORD[] = "";
constexpr char MQTT_SERVER[]   = "broker.hivemq.com";
constexpr uint16_t MQTT_PORT   = 1883;

// ⚠️ Deve ser EXATAMENTE o mesmo prefixo do dashboard (app.js)
constexpr char TOPIC_CMD[]     = "seguranca/patrimonio/meu-esp32/cmd";
constexpr char TOPIC_STATE[]   = "seguranca/patrimonio/meu-esp32/state";
constexpr char TOPIC_ALARM[]   = "seguranca/patrimonio/meu-esp32/alarm";
constexpr char TOPIC_LOG[]     = "seguranca/patrimonio/meu-esp32/log";
constexpr char TOPIC_SENSORS[] = "seguranca/patrimonio/meu-esp32/sensors";
constexpr char TOPIC_STATUS[]  = "seguranca/patrimonio/meu-esp32/status";

constexpr int PIN_BUTTON = 4;
constexpr int PIN_LED    = 2;

constexpr uint32_t SENSOR_PUBLISH_MS = 2000;
constexpr uint32_t AUTO_ALARM_MS     = 10000;  // armado → alarme "falso" p/ teste
constexpr uint32_t BUTTON_DEBOUNCE_MS = 300;

// ===================== Estado =====================
enum SystemState : uint8_t {
  STATE_DISARMED = 0,
  STATE_ARMED    = 1,
  STATE_ALARM    = 2
};

SystemState g_state = STATE_DISARMED;

// Valores de sensor simulados (o painel deve reagir às cores)
int      g_distance = 200;
bool     g_pir      = false;
int      g_ldr      = 800;
bool     g_ldrAnomaly = false;

uint32_t g_armTimeMs = 0;
uint32_t g_lastSensorPub = 0;
uint32_t g_lastButtonMs  = 0;
bool     g_lastButtonReading = HIGH;

// Demo de distância: alterna faixas para ver verde / âmbar / vermelho
const int DISTANCE_CYCLE[] = { 200, 150, 80, 35, 25, 80, 150, 200 };
constexpr uint8_t DISTANCE_CYCLE_LEN = sizeof(DISTANCE_CYCLE) / sizeof(DISTANCE_CYCLE[0]);
uint8_t g_distanceIdx = 0;

WiFiClient   espClient;
PubSubClient mqttClient(espClient);
char g_clientId[40] = {0};

// ===================== Helpers de publicação =====================
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
  StaticJsonDocument<64> doc;
  doc["armed"] = (g_state != STATE_DISARMED);
  doc["alarm"] = (g_state == STATE_ALARM);
  char buf[64];
  serializeJson(doc, buf, sizeof(buf));
  mqttClient.publish(TOPIC_STATE, buf);
  Serial.printf("[STATE] armed=%d alarm=%d\n",
                g_state != STATE_DISARMED, g_state == STATE_ALARM);
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
  // Sem buzzer dedicado no teste: só feedback no Serial + LED
  Serial.printf("[BEEP] %u Hz / %u ms\n", freq, ms);
  digitalWrite(PIN_LED, HIGH);
  delay(ms);
  digitalWrite(PIN_LED, g_state == STATE_ALARM ? HIGH : LOW);
}

// ===================== Sensores simulados =====================
void updateSimulatedSensors() {
  // Distância: percorre faixas de cor do painel
  g_distanceIdx = (g_distanceIdx + 1) % DISTANCE_CYCLE_LEN;
  g_distance = DISTANCE_CYCLE[g_distanceIdx];

  // PIR: pulsa em algumas amostras
  g_pir = (g_distanceIdx % 3 == 0);

  // LDR: varia e occasionalmente sinaliza anomalia
  g_ldr = 400 + (g_distanceIdx * 150) % 2000;
  g_ldrAnomaly = (g_distanceIdx == 5);

  // Só "intruso" conta se o sistema estiver armado
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
      setState(STATE_ARMED, "armed");
      beep(1200, 120);
      beep(1600, 180);
    } else {
      publishLog("arm_ignored");
    }
  } else if (!strcmp(action, "disarm")) {
    if (g_state != STATE_DISARMED) {
      setState(STATE_DISARMED, "disarmed");
      beep(1600, 120);
      beep(1000, 200);
    } else {
      publishLog("disarm_ignored");
    }
  } else if (!strcmp(action, "reset_alarm")) {
    if (g_state == STATE_ALARM) {
      // Volta a ARMADO (mesmo contrato do firmware de produção)
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
    // Simula calibração rápida (sem bloquear demais o loop)
    delay(200);
    g_ldr = 900;
    g_ldrAnomaly = false;
    publishLog("recalibrated");
  } else {
    Serial.printf("[CMD] ação desconhecida: %s\n", action);
    publishLog("cmd_unknown");
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("[MQTT] RX %s (%u bytes)\n", topic, length);

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

  // LWT: se cair, o painel deve mostrar ESP32 offline
  if (mqttClient.connect(g_clientId, nullptr, nullptr,
                         TOPIC_STATUS, 0, true, "offline")) {
    Serial.println("OK");
    mqttClient.publish(TOPIC_STATUS, "online", true);
    mqttClient.subscribe(TOPIC_CMD);
    publishState();
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
      setState(STATE_ARMED, "armed_button");
      beep(1200, 120);
    } else if (g_state == STATE_ARMED) {
      // Segunda pressão no armado: simula intrusão
      triggerAlarm("button", 0);
    } else {
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
  Serial.println("\n=== Firmware de TESTE — Segurança Patrimonial ===");
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

  // Sensores simulados + publicação periódica
  if (millis() - g_lastSensorPub >= SENSOR_PUBLISH_MS) {
    g_lastSensorPub = millis();
    updateSimulatedSensors();
  }

  // Alarme automático de demonstração (só se armado)
  if (g_state == STATE_ARMED && millis() - g_armTimeMs >= AUTO_ALARM_MS) {
    Serial.println("[TESTE] Auto-alarme de demonstração");
    triggerAlarm("pir", 0);
  }

  pollButton();

  // LED pisca durante alarme
  if (g_state == STATE_ALARM) {
    digitalWrite(PIN_LED, (millis() / 200) % 2 ? HIGH : LOW);
  }

  delay(20);
}
