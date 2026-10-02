# Segurança Patrimonial — Dashboard Web + ESP32

Projeto completo de sistema de segurança patrimonial com duas camadas:

1. **Dashboard web** — painel em português para monitorar e controlar o sistema via MQTT
2. **Firmware ESP32** — leitores (PIR, HC-SR04, LDR), atuadores (sirene/LED), display OLED e máquina de estados

O firmware e a interface falam o mesmo contrato MQTT (mesmos tópicos e payloads). Este repositório contém **ambas** as camadas.

---

## Funcionalidades

### Dashboard web

- **Estado do sistema**: DESARMADO, ARMADO ou INVASÃO (alarme disparado)
- **Sensores em tempo real**: distância (ultrassônico), PIR (movimento) e LDR (luminosidade)
- **Status do hardware**: presença online/offline do ESP32 (via Last Will and Testament)
- **Controles**: armar, desarmar, silenciar alarme, testar sirene e recalibrar LDR
- **Histórico de eventos**: log em tempo real no painel (limitado a 30 linhas visíveis)
- **Feedback sonoro**: bipe curto no navegador quando chega um alarme

### Firmware ESP32

- Detecção por **PIR**, **proximidade** (HC-SR04) e **anomalia de luz** (LDR)
- **Sirene** em task dedicada (não bloqueia comandos nem o LED)
- **Display OLED SSD1306** com estado, sensores e status de rede
- **Botão físico** para armar/desarmar (emergência local)
- **Persistência NVS** do estado após reboot
- **Auto-reset** do alarme após **60 segundos** (volta para ARMADO)
- **Calibração de LDR** no boot e por comando MQTT
- Arquitetura **FreeRTOS** em 5 tasks (2 núcleos)

---

## Arquitetura

```
┌─────────────────┐   WSS :8884    ┌──────────────────────┐   TCP :1883   ┌─────────┐
│   Navegador     │ ◄────────────► │  Broker MQTT         │ ◄───────────► │  ESP32  │
│  (dashboard)    │                │  (HiveMQ público)    │               │ firmware│
│   index.html    │                │  broker.hivemq.com   │               │  + I/O  │
│   app.js        │                │                      │               │         │
└─────────────────┘                └──────────────────────┘               └─────────┘
```

1. O dashboard conecta ao broker por **WebSocket** (`wss://broker.hivemq.com:8884/mqtt`).
2. O ESP32 conecta por **TCP** (`broker.hivemq.com:1883`).
3. O firmware publica telemetria (sensores, estado, alarmes, logs) e assina comandos.
4. O usuário envia comandos pelo painel (ou pelo botão físico); o firmware reage (sirene/LED/beeps).
5. O ESP32 publica `online`/`offline` no tópico de status (LWT).

### Tasks FreeRTOS (firmware)

| Task | Core | Prioridade | Responsabilidade |
|------|------|------------|------------------|
| `mqttTask` | 0 | 2 | WiFi, MQTT, publicações, sensores a cada 2 s |
| `displayTask` | 0 | 1 | OLED SSD1306 a cada 250 ms |
| `sensorTask` | 1 | 2 | PIR, HC-SR04, LDR, botão → fila de eventos |
| `alarmTask` | 1 | 3 | Máquina de estados, LED, publicação de estado |
| `sirenTask` | 1 | 2 | Sirene contínua controlada por flag |

Primitivas: mutex de status (`statusMutex`), fila de eventos (`eventQueue`), fila TX MQTT (`txQueue`).

---

## Tópicos MQTT

Prefixo comum: `seguranca/patrimonio/meu-esp32`

| Tópico | Direção | Payload (JSON) | Observação |
|--------|---------|----------------|------------|
| `.../cmd` | web → ESP32 | `{"action":"arm"}` | Comandos de controle |
| `.../state` | ESP32 → web | `{"armed":true,"alarm":false}` | Estado atual do sistema |
| `.../alarm` | ESP32 → web | `{"type":"pir","value":0,"ts":...}` | Disparo de alarme |
| `.../log` | ESP32 → web | `{"event":"armed","state":1,"ts":...}` | Eventos do firmware |
| `.../sensors` | ESP32 → web | `{"pir":false,"dist":120,"ldr":800,"ldr_anomaly":false}` | Leituras dos sensores |
| `.../status` | ESP32 → web | `online` \| `offline` | LWT (texto puro) |

### Ações aceitas em `.../cmd`

| Ação | Significado |
|------|-------------|
| `arm` | Armear o sistema |
| `disarm` | Desarmar o sistema |
| `reset_alarm` | Silenciar / limpar o alarme ativo (volta a ARMADO) |
| `test` | Testar a sirene (beeps curtos) |
| `recalibrate` | Recalibrar o sensor LDR |

### Eventos de log relevantes

| `event` | Quando |
|---------|--------|
| `boot` | ESP32 conectou ao broker |
| `armed` / `disarmed` | Comando MQTT de arm/desarm |
| `armed_button` / `disarmed_button` | Botão físico |
| `alarm_pir` / `alarm_proximity` / `alarm_ldr` | Intrusão detectada |
| `alarm_reset` | Alarme silenciado pelo painel/botão |
| `alarm_timeout` | Auto-reset após 60 s |
| `recalibrating` | Recalibração de LDR solicitada |
| `recalibrated` | Recalibração concluída |
| `test` | Teste de sirene |

---

## Hardware (ESP32)

| Componente | Pino | Observação |
|------------|------|------------|
| PIR (HC-SR501 ou similar) | GPIO 16 | Saída digital |
| HC-SR04 TRIG | GPIO 5 | — |
| HC-SR04 ECHO | GPIO 18 | — |
| LDR ( divisor ) | GPIO 34 | ADC1 (apenas leitura) |
| Buzzer 1 | GPIO 19 | LEDC |
| Buzzer 2 | GPIO 23 | LEDC |
| Botão | GPIO 4 | `INPUT_PULLUP` |
| LED | GPIO 2 | onboard / status |
| OLED SSD1306 | SDA 21 / SCL 22 | I2C, endereço `0x3C` |

> Simulação sugerida: **Wokwi** (o sketch usa `Wokwi-GUEST` por padrão). Em hardware real, troque `WIFI_SSID` e `WIFI_PASSWORD`.

---

## Limiares de detecção

### Firmware (decisão de alarme)

| Parâmetro | Valor | Significado |
|-----------|-------|-------------|
| `DIST_THRESHOLD_CM` | 40 cm | Proximidade a partir da qual o alarme pode disparar |
| `DIST_CONFIRM_MS` | 800 ms | Tempo que a proximidade deve permanecer para confirmar |
| `PIR_HOLD_MS` | 3000 ms | Mantém o PIR “ativo” no display após o sinal |
| `LDR_DELTA_ON` / `OFF` | 300 / 200 | Histerese da anomalia de luz sobre a base calibrada |
| `LDR_CONFIRM_MS` | 2000 ms | Confirmação da anomalia LDR |
| `ALARM_AUTO_RESET_MS` | 60000 ms | Auto-reset do alarme → ARMADO |
| Distância ausente | média ≥ 400 cm → `999` | Sensor fora de alcance / sem eco |

### Dashboard (apresentação)

| Métrica | Condição | Comportamento no painel |
|---------|----------|-------------------------|
| Distância | `>= 999` cm | Exibe `---` |
| Distância | `< 40` cm | Valor em **vermelho** |
| Distância | `< 100` cm | Valor em **âmbar** |
| Distância | `>= 100` cm | Valor em **verde** |
| PIR | `true` | Exibe `ATIVO` em vermelho |
| PIR | `false` | Exibe `quieto` em cinza |
| LDR | `ldr_anomaly: true` | Valor em vermelho |
| LDR | `ldr_anomaly: false` | Valor em verde |
| ESP32 | `online` / `offline` | Métrica verde / vermelha |

---

## Controles

| Botão (web) | Comando MQTT | Payload |
|-------------|--------------|---------|
| 🔒 Armar | `.../cmd` | `{"action":"arm"}` |
| 🔓 Desarmar | `.../cmd` | `{"action":"disarm"}` |
| 🔕 Silenciar Alarme | `.../cmd` | `{"action":"reset_alarm"}` |
| 🔊 Testar Sirene | `.../cmd` | `{"action":"test"}` |
| 🌗 Recalibrar LDR | `.../cmd` | `{"action":"recalibrate"}` |

- O botão **Silenciar Alarme** só fica habilitado quando o estado recebido indica alarme ativo.
- O **botão físico** no ESP32 alterna: desarmado → armado; armado/alarme → desarmado (e silencia a sirene).

---

## Como executar

### Dashboard web

**Pré-requisitos:** navegador moderno e internet (CDN do MQTT.js + broker público).

**Opção 1 — Abrir o arquivo**

1. Abra `index.html` no navegador (duplo clique).
2. Aguarde o badge indicar **Conectado**.

**Opção 2 — Servidor estático**

```bash
# Node.js
npx serve .

# ou Python
python -m http.server 8080
```

Acesse `http://localhost:8080` (ou a porta indicada).

### Firmware ESP32

**Bibliotecas** (Arduino IDE → Gerenciador de Bibliotecas):

- `WiFi` (núcleo ESP32)
- `PubSubClient` (Nick O’Leary)
- `ArduinoJson` (Benoit Blanchon)
- `Adafruit GFX Library`
- `Adafruit SSD1306`
- `Preferences` (núcleo ESP32)

**Arduino IDE**

1. Selecione a placa **ESP32 Dev Module** (ou a sua).
2. Abra `sketch.ino`.
3. Ajuste `WIFI_SSID` / `WIFI_PASSWORD` (se for sair do Wokwi).
4. Compile e envie.

**Wokwi — firmware de produção (`sketch.ino`)**

O projeto Wokwi da versão de produção já está na **raiz** do repositório:

| Arquivo | Papel |
|---------|--------|
| `sketch.ino` | Firmware FreeRTOS (sensores, sirene, OLED, MQTT) |
| `.wokwi.toml` | Aponta o firmware para o Wokwi |
| `diagram.toml` | Circuito completo (OLED, PIR, HC-SR04, LDR, buzzers, botão, LED) |

**Como abrir**

1. **VS Code:** abra a pasta **raiz** do projeto com a extensão **Wokwi for VS Code** → **Start Simulator**.
2. **Online:** em [wokwi.com](https://wokwi.com), projeto ESP32 + cole os arquivos da raiz (`sketch.ino`, `.wokwi.toml`, `diagram.toml`).
3. **CLI:** na raiz, `wokwi-cli` já lê `.wokwi.toml` + `diagram.toml`.

**Circuito de produção (`diagram.toml`)**

| Componente | Pinos ESP32 | Observação |
|------------|-------------|------------|
| OLED SSD1306 | SDA 21 / SCL 22 / 3V3 / GND | I2C, addr `0x3C` |
| PIR | OUT 16 / 5V / GND | Movimento |
| HC-SR04 | TRIG 5 / ECHO 18 / 5V / GND | Distância |
| LDR + 10k | div. em GPIO 34 | 3V3—LDR—GPIO34—10k—GND |
| Buzzer 1 | GPIO 19 → GND | Sirene / beeps |
| Buzzer 2 | GPIO 23 → GND | Sirene / beeps |
| Botão | GPIO 4 → GND | `INPUT_PULLUP` |
| LED | GPIO 2 → 220Ω → GND | Status |

4. Inicie a simulação (rede `Wokwi-GUEST`).
5. Abra o `index.html` e use o mesmo checklist do firmware de teste (comandos MQTT + sensores no OLED).

> **Dica:** para validar **só o painel** sem montar o circuito completo, use `firmware_teste/` (botão + LED apenas). Para validar firmware + hardware, use a raiz com `diagram.toml` de produção.

**Wokwi — firmware de teste do painel (`firmware_teste/`)**

Para validar **só a interface** (sem OLED nem FreeRTOS), use o projeto pronto em `firmware_teste/`:

| Arquivo | Papel |
|---------|--------|
| `firmware_teste.ino` | Sketch de teste (mesmo contrato MQTT do painel) |
| `.wokwi.toml` | Aponta o firmware para o Wokwi |
| `diagram.toml` | Circuito: ESP32 + botão (GPIO 4) + LED (GPIO 2) + resistor 220Ω |

**Como abrir no Wokwi**

1. **Online:** em [wokwi.com](https://wokwi.com), crie um projeto ESP32 e cole os três arquivos (ou use “Import from GitHub/folder” se tiver o projeto versionado).
2. **VS Code:** instale a extensão **Wokwi for VS Code**, abra a pasta `firmware_teste/` e clique em **Start Simulator**.
3. **CLI (`wokwi-cli`):** a partir de `firmware_teste/`, `wokwi-cli` já lê `.wokwi.toml` + `diagram.toml`.

**Circuito já definido em `diagram.toml`**

| Pino ESP32 | Componente | Observação |
|------------|------------|------------|
| GPIO 4 | Botão → GND | `INPUT_PULLUP` (sem resistor externo) |
| GPIO 2 | Resistor 220Ω → LED → GND | LED vermelho de status |

4. Inicie a simulação (rede `Wokwi-GUEST` já é a padrão).
5. Abra o `index.html` no navegador e siga o checklist abaixo.

O firmware de teste usa **os mesmos tópicos e payloads** do painel e do `sketch.ino` de produção.

**PlatformIO**

O repositório já inclui `platformio.ini` na raiz (sem precisar copiar o `.ino` para `src/`).

| Env | Fonte | Para quê |
|-----|--------|----------|
| `esp32dev` | `sketch.ino` | Firmware de produção |
| `esp32dev_teste` | `firmware_teste/firmware_teste.ino` | Validação do painel |

```powershell
# na raiz do projeto
pio run -e esp32dev
pio run -t upload -e esp32dev
pio device monitor -e esp32dev

pio run -e esp32dev_teste
pio run -t upload -e esp32dev_teste
```

Bibliotecas (resolvidas pelo PlatformIO): PubSubClient, ArduinoJson (v6), Adafruit GFX, Adafruit SSD1306.

> A pasta `.vscode/` atual foi gerada para **C/C++ desktop (gcc/gdb)** e **não** serve para compilar/debugar o ESP32. Use **Arduino IDE**, **PlatformIO** ou o **Wokwi**.

### Validar o painel com `firmware_teste` (Wokwi)

Fluxo recomendado de ponta a ponta:

| Passo | O que fazer | O que observar no painel |
|------|-------------|---------------------------|
| 1 | Iniciar o Wokwi com `firmware_teste` | Serial: `[WiFi] OK`, `[MQTT] Conectando...OK`, `[LOG] boot` |
| 2 | Abrir `index.html` | Badge **Conectado**; ESP32 **online**; estado **DESARMADO** |
| 3 | Clicar **🔊 Testar Sirene** | Log: `→ comando: test` e, no firmware, `[CMD] action=test` + beep no LED |
| 4 | Clicar **🔒 Armar** | Card **ARMADO**; log `armed`; LED (GPIO 2) acende |
| 5 | Aguardar ~10 s **ou** pressionar o botão no Wokwi | Auto-alarme / `alarm_pir` → card **INVASÃO** + bipe; LED pisca |
| 6 | Clicar **🔕 Silenciar Alarme** | Volta a **ARMADO**; log `alarm_reset` |
| 7 | Clicar **🔓 Desarmar** | Card **DESARMADO**; log `disarmed` |
| 8 | Clicar **🌗 Recalibrar LDR** | Logs `recalibrating` → `recalibrated` |
| 9 | Observar métricas a cada ~2 s | Distância percorre faixas (verde → âmbar → vermelho); PIR/LDR variam |

Checklist de aceitação:

- [ ] Conexão MQTT web ↔ ESP32 (mesmo broker e tópico)
- [ ] Comandos do painel chegam ao firmware (Serial do Wokwi)
- [ ] Estado e sensores do firmware atualizam a UI
- [ ] Alarme dispara no painel (INVASÃO + bipe) quando armado
- [ ] Silenciar / Desarmar recuperam o sistema
- [ ] Botão físico do Wokwi também alterna o estado

> **Importante:** o tópico padrão é `seguranca/patrimonio/meu-esp32`. No firmware, troque **apenas** `SECURITY_TOPIC_PREFIX` em `sketch.ino` e `firmware_teste.ino`; no web, o prefixo está em `app.js` (`MQTT_TOPIC_BASE`). Os valores devem ser idênticos.

Exemplo de estado armado:

- Tópico: `seguranca/patrimonio/meu-esp32/state`
- Payload: `{"armed":true,"alarm":false}`

### Teste sem firmware (só MQTT Explorer)

1. Suba o dashboard no navegador.
2. Conecte o [MQTT Explorer](https://mqtt-explorer.com/) ao `broker.hivemq.com`.
3. Publique payloads nos tópicos do contrato — a UI reage na hora.

---

## Estrutura do projeto

```
projeto-final-test3/
├── index.html                    # Markup do dashboard
├── styles.css                    # Tema visual (dark)
├── app.js                        # Lógica web + integração MQTT (WSS)
├── sketch.ino                    # Firmware de produção (FreeRTOS + OLED + MQTT)
├── platformio.ini                # Builds PlatformIO (produção + teste)
├── .wokwi.toml                   # Config Wokwi da produção (raiz)
├── diagram.toml                  # Circuito completo de produção (raiz)
├── firmware_teste/
│   ├── firmware_teste.ino        # Firmware leve p/ validar o painel
│   ├── .wokwi.toml               # Config Wokwi do teste
│   └── diagram.toml              # Circuito: botão GPIO4 + LED GPIO2
├── README.md                     # Esta documentação
└── .vscode/                      # Config C/C++ desktop (não é o toolchain do ESP32)
```

| Arquivo | Responsabilidade |
|---------|------------------|
| `index.html` | Estrutura semântica, IDs do DOM, carga de CSS/JS |
| `styles.css` | Aparência, estados visuais, responsivo |
| `app.js` | Constantes, renderização, handlers MQTT, bootstrap |
| `sketch.ino` | Sensores, sirene, OLED, máquina de estados, MQTT |
| `platformio.ini` | Env `esp32dev` e `esp32dev_teste` (PlatformIO) |
| `.wokwi.toml` / `diagram.toml` | Projeto Wokwi da **produção** (hardware completo) |
| `firmware_teste/*` | Projeto Wokwi **leve** só para o painel (botão + LED) |
| `README.md` | Documentação do projeto e do contrato MQTT |

### Convenções de código

- **Identificadores** em inglês (Clean Code); **comentários e UI** em português.
- **IDs do DOM** estáveis (`mDist`, `btnArm`…) — contrato entre HTML e JS.
- **Constantes nomeadas** no topo de `app.js` e de `sketch.ino` (sem números mágicos espalhados).
- **Tópicos MQTT** no firmware derivam de `SECURITY_TOPIC_PREFIX` (fonte única); no web, de `MQTT_TOPIC_BASE`.
- Firmware: eventos por **fila FreeRTOS**; estado compartilhado por **mutex**; sirene/status MQTT por **flags volatile**.

---

## Stack

| Camada | Tecnologia |
|--------|------------|
| Markup / estilo / lógica web | HTML5, CSS3, JS ES6+ (sem framework) |
| MQTT (browser) | MQTT.js via CDN (unpkg) — WebSocket 8884 |
| MQTT (ESP32) | PubSubClient — TCP 1883 |
| Firmware | Arduino ESP32, FreeRTOS, ArduinoJson, Preferences |
| Build firmware | Arduino IDE **ou** PlatformIO (`platformio.ini`) |
| Display | Adafruit SSD1306 (I2C) |
| Broker | HiveMQ público (`broker.hivemq.com`) |
| Simulação | Wokwi (opcional) |

---

## Limitações conhecidas

1. **Broker público sem autenticação** — qualquer pessoa que conheça o tópico pode ler e publicar comandos. Adequado a estudo/demo; **não** a proteção real de patrimônio.
2. **Tópico genérico** (`meu-esp32`) — colisão possível se vários projetos usarem o mesmo nome. Troque por um prefixo único.
3. **QoS 0** — mensagens podem ser perdidas sem reentrega.
4. **Sem persistência no web** — o histórico de eventos some ao recarregar a página.
5. **Sem validação estrita de payloads** no dashboard — JSON inválido é descartado com aviso no console.
6. **Áudio do navegador** pode ser bloqueado por autoplay policy antes da primeira interação.
7. **Dependência de CDN e rede** — sem internet ou com unpkg/broker fora do ar, o painel não conecta.
8. **WiFi hardcoded** no firmware (`Wokwi-GUEST`) — ajuste para a sua rede.
9. **`.vscode` incompatível com ESP32** — configs de gcc desktop; use Arduino IDE/PlatformIO para o sketch.

---

## Robustez do firmware (v3.2)

Correções aplicadas para operação estável com o painel:

| Tema | Implementação |
|------|----------------|
| Falso alarme ao armar | PIR e LDR enfileiram apenas na **borda de transição**; fila drenada ao mudar de estado |
| Flood de eventos | Evita reenviar `EVT_PIR`/`EVT_LDR` enquanto o sinal permanece ativo |
| Calibração LDR | Executada no `sensorTask` via flag — **não bloqueia** sirene, LED nem eventos |
| PubSubClient multi-task | `displayTask` lê `g_mqttConnected` (flag volatile), sem tocar no client |
| Sirene | Task independente; desligada em disarm/reset/timeout |
| Clean Code (v3.2) | Funções em inglês (`calibrateLdr`, `validateLdrAnomaly`…); tópicos derivados de `SECURITY_TOPIC_PREFIX` |

---

## Próximos passos (sugestões)

Itens que elevariam o projeto para uso mais sério — **não implementados** nesta versão:

- Tópico único com prefixo/UUID por instância
- Broker próprio (Mosquitto, EMQX, CloudMQTT) com TLS e credenciais
- Autenticação MQTT (usuário/senha ou certificados)
- Validação e tipagem dos payloads recebidos
- Persistência do log (localStorage ou backend)
- Debounce/timeout de comandos repetidos no painel
- Testes automatizados dos handlers de mensagem
- Migração ArduinoJson v6 → v7 (`JsonDocument`)
- Diagrama de fiação físico (Fritzing) além do Wokwi

---

## Licença e uso

Projeto didático/profissional. Adapte tópicos, limiares, WiFi e broker conforme o seu hardware e o seu ambiente de rede. **Não** use broker público sem autenticação em cenários reais de proteção de patrimônio.
