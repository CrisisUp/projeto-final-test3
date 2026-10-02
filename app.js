/*
 * app.js — Segurança Patrimonial (Dashboard Web)
 *
 * Papel: lógica de apresentação e integração MQTT do painel.
 * Não contém firmware ESP32; apenas consome e publica em tópicos MQTT.
 *
 * Fluxo resumido:
 *   1. Conecta ao broker via WebSocket
 *   2. Assina tópicos de estado, alarme, log, sensores e status
 *   3. Atualiza a UI conforme as mensagens recebidas
 *   4. Publica comandos (arm/disarm/...) quando o usuário clica nos botões
 *
 * Identificadores de código em inglês (padrão Clean Code);
 * comentários e mensagens visíveis em português.
 */

/* =========================================================
   1. Configuração e constantes
   ========================================================= */

// Broker público HiveMQ — adequado a demonstração/didática.
// Em produção, usar broker próprio com autenticação e tópico único.
const MQTT_BROKER_URL = 'wss://broker.hivemq.com:8884/mqtt';
const MQTT_TOPIC_BASE = 'seguranca/patrimonio/meu-esp32';

const MQTT_TOPIC_COMMAND = `${MQTT_TOPIC_BASE}/cmd`;
const MQTT_TOPIC_STATE   = `${MQTT_TOPIC_BASE}/state`;
const MQTT_TOPIC_ALARM   = `${MQTT_TOPIC_BASE}/alarm`;
const MQTT_TOPIC_LOG     = `${MQTT_TOPIC_BASE}/log`;
const MQTT_TOPIC_SENSORS = `${MQTT_TOPIC_BASE}/sensors`;
const MQTT_TOPIC_STATUS  = `${MQTT_TOPIC_BASE}/status`;
const MQTT_TOPIC_SCHEDULE = `${MQTT_TOPIC_BASE}/schedule`;

// Reconexão e timeout evitam travar a UI se o broker cair.
const MQTT_RECONNECT_MS = 3000;
const MQTT_CONNECT_TIMEOUT_MS = 10000;

// Limite de linhas no histórico: evita crescimento ilimitado do DOM.
const MAX_LOG_ENTRIES = 30;

// Limiares de distância (cm) — espelham a lógica do firmware.
// 999 é sentinela de "sensor ausente / fora de alcance".
const DISTANCE_UNKNOWN_CM = 999;
const DISTANCE_ALERT_CM = 40;
const DISTANCE_WARNING_CM = 100;

// Bipe de alarme no navegador (WebAudio).
const ALARM_BEEP_FREQUENCY_HZ = 880;
const ALARM_BEEP_DURATION_MS = 600;
const ALARM_BEEP_GAIN = 0.3;

// Ações de comando — valores exatos aceitos pelo firmware ESP32.
const COMMAND_ARM = 'arm';
const COMMAND_DISARM = 'disarm';
const COMMAND_RESET_ALARM = 'reset_alarm';
const COMMAND_TEST_SIREN = 'test';
const COMMAND_RECALIBRATE = 'recalibrate';

// Valores do campo opcional "schedule" no payload de estado (firmware).
const SCHEDULE_STATE_IDLE = 'idle';
const SCHEDULE_STATE_ACTIVE = 'active';
const SCHEDULE_STATE_OVERRIDE = 'override';

// HH:MM 24h — mesmo formato usado no firmware (parseHourMinute).
const SCHEDULE_TIME_PATTERN = /^([01]\d|2[0-3]):[0-5]\d$/;

// Último estado de NTP recebido em .../state (null = ainda não informado).
let lastNtpSynced = null;

/* =========================================================
   2. Referências ao DOM
   ========================================================= */

const connDot = document.getElementById('connDot');
const connLabel = document.getElementById('connLabel');
const stateCard = document.getElementById('stateCard');
const stateIcon = document.getElementById('stateIcon');
const stateTitle = document.getElementById('stateTitle');
const stateSub = document.getElementById('stateSub');
const metricDistance = document.getElementById('mDist');
const metricPir = document.getElementById('mPir');
const metricLight = document.getElementById('mLdr');
const metricEsp = document.getElementById('mEsp');
const logElement = document.getElementById('log');
const buttonArm = document.getElementById('btnArm');
const buttonDisarm = document.getElementById('btnDisarm');
const buttonReset = document.getElementById('btnReset');
const buttonTest = document.getElementById('btnTest');
const buttonRecalibrate = document.getElementById('btnRecal');

const scheduleArmInput = document.getElementById('schedArmAt');
const scheduleDisarmInput = document.getElementById('schedDisarmAt');
const scheduleWeekdaysCheckbox = document.getElementById('schedWeekdaysOnly');
const scheduleEnabledCheckbox = document.getElementById('schedEnabled');
const scheduleSaveButton = document.getElementById('btnSaveSchedule');
const scheduleStatusElement = document.getElementById('schedStatus');

/* =========================================================
   3. Helpers puros
   ========================================================= */

/**
 * Converte um timestamp (epoch millis) em HH:MM:SS.
 * Sem argumento, usa o horário atual do navegador.
 * @param {number} [timestamp]
 * @returns {string}
 */
function formatTimestamp(timestamp) {
  if (!timestamp) {
    return new Date().toTimeString().slice(0, 8);
  }

  const totalSeconds = Math.floor(timestamp / 1000) % 86400;
  const hours = String(Math.floor(totalSeconds / 3600)).padStart(2, '0');
  const minutes = String(Math.floor((totalSeconds % 3600) / 60)).padStart(2, '0');
  const seconds = String(totalSeconds % 60).padStart(2, '0');
  return `${hours}:${minutes}:${seconds}`;
}

/**
 * Classifica a mensagem de log para aplicar a cor correta no painel.
 * Ordem preservada do código original: "arm" casa antes de "disarm",
 * então mensagens que contenham "disarm" também entram na classe "arm".
 * Eventos de agendamento seguem a mesma lógica (schedule_armed → arm).
 * @param {string} message
 * @returns {string} classe CSS do .log-line ('' | 'alarm' | 'arm' | 'disarm')
 */
function classifyLogMessage(message) {
  if (message.includes('alarm')) return 'alarm';
  if (message.includes('arm')) return 'arm';
  if (message.includes('disarm')) return 'disarm';
  return '';
}

/**
 * Valida um horário no formato HH:MM 24h (aceito pelo firmware).
 * @param {string} value
 * @returns {boolean}
 */
function isValidTimeString(value) {
  return SCHEDULE_TIME_PATTERN.test(value);
}

/**
 * Monta o payload JSON do tópico .../schedule.
 * @param {{enabled: boolean, armAt: string, disarmAt: string, weekdaysOnly: boolean}} config
 * @returns {object}
 */
function buildSchedulePayload(config) {
  return {
    enabled: config.enabled,
    armAt: config.armAt,
    disarmAt: config.disarmAt,
    weekdaysOnly: config.weekdaysOnly,
  };
}

/**
 * Resume a configuração de agendamento em texto legível.
 * @param {{enabled?: boolean, armAt?: string, disarmAt?: string, weekdaysOnly?: boolean}} config
 * @returns {string}
 */
function formatScheduleSummary(config) {
  const armAt = config.armAt || '--:--';
  const disarmAt = config.disarmAt || '--:--';
  const dayScope = config.weekdaysOnly ? 'só dias úteis' : 'todos os dias';
  const active = config.enabled ? 'ativo' : 'inativo';
  return `Armado ${armAt}–${disarmAt} · ${dayScope} · ${active}`;
}

/**
 * Traduz o campo "schedule" do payload de estado em texto pt-BR.
 * @param {string} scheduleField
 * @returns {string}
 */
function describeScheduleField(scheduleField) {
  if (scheduleField === SCHEDULE_STATE_ACTIVE) {
    return 'janela ativa (dentro do horário)';
  }
  if (scheduleField === SCHEDULE_STATE_OVERRIDE) {
    return 'override manual (desarmado no meio da janela)';
  }
  return 'ociosa (fora do horário ou desligada)';
}

/**
 * Lê o formulário de agendamento do DOM.
 * @returns {{enabled: boolean, armAt: string, disarmAt: string, weekdaysOnly: boolean}}
 */
function readScheduleForm() {
  return {
    enabled: scheduleEnabledCheckbox.checked,
    armAt: scheduleArmInput.value,
    disarmAt: scheduleDisarmInput.value,
    weekdaysOnly: scheduleWeekdaysCheckbox.checked,
  };
}

/**
 * Preenche o formulário a partir de um payload recebido/retained.
 * @param {{enabled?: boolean, armAt?: string, disarmAt?: string, weekdaysOnly?: boolean}} config
 */
function fillScheduleForm(config) {
  if (typeof config.armAt === 'string' && isValidTimeString(config.armAt)) {
    scheduleArmInput.value = config.armAt;
  }
  if (typeof config.disarmAt === 'string' && isValidTimeString(config.disarmAt)) {
    scheduleDisarmInput.value = config.disarmAt;
  }
  if (typeof config.weekdaysOnly === 'boolean') {
    scheduleWeekdaysCheckbox.checked = config.weekdaysOnly;
  }
  if (typeof config.enabled === 'boolean') {
    scheduleEnabledCheckbox.checked = config.enabled;
  }
}

/**
 * Descreve o estado do NTP do ESP32 para o painel.
 * @param {boolean|null} ntpSynced
 * @returns {string}
 */
function describeNtpStatus(ntpSynced) {
  if (ntpSynced === true) return 'NTP: sincronizado';
  if (ntpSynced === false) return 'NTP: aguardando relógio';
  return 'NTP: —';
}

/**
 * Atualiza o status textual do card de agendamento.
 * @param {{enabled?: boolean, armAt?: string, disarmAt?: string, weekdaysOnly?: boolean}} config
 * @param {string} [scheduleField] campo opcional do tópico de estado
 * @param {'ok'|'warn'|'error'|''} [tone]
 * @param {boolean|null} [ntpSynced]
 */
function renderScheduleStatus(config, scheduleField = '', tone = 'ok', ntpSynced = lastNtpSynced) {
  const summary = formatScheduleSummary(config);
  let text = summary;

  if (scheduleField) {
    text += ` · Status atual: ${describeScheduleField(scheduleField)}`;
  }

  text += ` · ${describeNtpStatus(ntpSynced)}`;

  // Sem NTP, o agendamento não transiciona — destaque em âmbar.
  if (ntpSynced === false && config.enabled) {
    tone = 'warn';
  }

  scheduleStatusElement.textContent = text;
  scheduleStatusElement.className = `schedule-status ${tone}`.trim();
}

/**
 * Interpreta o payload de distância e devolve a cor da métrica.
 * @param {number} distanceCm
 * @returns {string}
 */
function resolveDistanceClass(distanceCm) {
  if (distanceCm < DISTANCE_ALERT_CM) return 'red';
  if (distanceCm < DISTANCE_WARNING_CM) return 'amber';
  return 'green';
}

/**
 * Converte o payload bruto de status MQTT em booleano online.
 * @param {string} rawPayload
 * @returns {boolean}
 */
function parseEspOnlineStatus(rawPayload) {
  return rawPayload === 'online';
}

/* =========================================================
   4. Renderização da UI
   ========================================================= */

/**
 * Atualiza o card de estado principal e o botão de silenciar alarme.
 * Prioridade: alarme > armado > desarmado.
 * @param {boolean} isArmed
 * @param {boolean} isAlarm
 */
function renderSystemState(isArmed, isAlarm) {
  stateCard.classList.remove('disarmed', 'armed', 'alarm');

  if (isAlarm) {
    stateCard.classList.add('alarm');
    stateIcon.textContent = '🚨';
    stateTitle.textContent = 'INVASÃO';
    stateSub.textContent = 'Alarme disparado!';
    buttonReset.disabled = false;
    return;
  }

  if (isArmed) {
    stateCard.classList.add('armed');
    stateIcon.textContent = '🔒';
    stateTitle.textContent = 'ARMADO';
    stateSub.textContent = 'Monitorando sensores';
    buttonReset.disabled = true;
    return;
  }

  stateCard.classList.add('disarmed');
  stateIcon.textContent = '🔓';
  stateTitle.textContent = 'DESARMADO';
  stateSub.textContent = 'Sistema inativo';
  buttonReset.disabled = true;
}

/**
 * Atualiza a métrica de distância com cores por faixa de proximidade.
 * >= 999 cm é sentinela de sensor ausente: mostra "---" e mantém a cor
 * calculada pelos limiares (o original usava verde para 999).
 * @param {number} distanceCm
 */
function renderDistanceMetric(distanceCm) {
  const isUnknown = distanceCm >= DISTANCE_UNKNOWN_CM;

  metricDistance.textContent = isUnknown ? '---' : `${distanceCm} cm`;
  metricDistance.className = `metric-value ${resolveDistanceClass(distanceCm)}`;
}

/**
 * Atualiza a métrica de movimento (PIR).
 * @param {boolean} isActive
 */
function renderPirMetric(isActive) {
  metricPir.textContent = isActive ? 'ATIVO' : 'quieto';
  metricPir.className = `metric-value ${isActive ? 'red' : 'muted'}`;
}

/**
 * Atualiza a métrica de luminosidade (LDR) e anomalias.
 * @param {number} ldrValue
 * @param {boolean} hasAnomaly
 */
function renderLightMetric(ldrValue, hasAnomaly) {
  metricLight.textContent = ldrValue;
  metricLight.className = `metric-value ${hasAnomaly ? 'red' : 'green'}`;
}

/**
 * Atualiza o status de presença do ESP32 no painel.
 * @param {boolean} isOnline
 */
function renderEspStatus(isOnline) {
  metricEsp.textContent = isOnline ? 'online' : 'offline';
  metricEsp.className = `metric-value ${isOnline ? 'green' : 'red'}`;
}

/**
 * Insere uma linha no histórico de eventos (mais recente no topo).
 * Mantém no máximo MAX_LOG_ENTRIES linhas na tela.
 * @param {string} message
 * @param {string} [className]
 */
function appendLogEntry(message, className = '') {
  const line = document.createElement('div');
  line.className = `log-line ${className}`.trim();
  line.innerHTML = `<span class="time">${formatTimestamp()}</span><span class="event">${message}</span>`;
  logElement.insertBefore(line, logElement.firstChild);

  while (logElement.children.length > MAX_LOG_ENTRIES) {
    logElement.removeChild(logElement.lastChild);
  }
}

/**
 * Emite um bipe curto de alarme no navegador.
 * O try/catch contorna bloqueios de autoplay do navegador
 * antes da primeira interação do usuário.
 */
function playAlarmBeep() {
  try {
    const AudioContextClass = window.AudioContext || window.webkitAudioContext;
    const context = new AudioContextClass();
    const oscillator = context.createOscillator();
    const gain = context.createGain();

    oscillator.frequency.value = ALARM_BEEP_FREQUENCY_HZ;
    oscillator.connect(gain);
    gain.connect(context.destination);

    gain.gain.setValueAtTime(ALARM_BEEP_GAIN, context.currentTime);
    gain.gain.exponentialRampToValueAtTime(0.001, context.currentTime + ALARM_BEEP_DURATION_MS / 1000);
    oscillator.start();
    oscillator.stop(context.currentTime + ALARM_BEEP_DURATION_MS / 1000);
  } catch (error) {
    // Áudio bloqueado ou API indisponível — o painel continua funcional.
  }
}

/* =========================================================
   5. Integração MQTT
   ========================================================= */

// Cliente MQTT é criado em connectMqttClient(); aqui fica a referência única.
let mqttClient = null;

/**
 * Publica um comando de controle no tópico de comandos.
 * QoS 0: sem reentrega — aceitável em painel/demo; em produção avaliar QoS 1+.
 * @param {string} action
 */
function publishCommand(action) {
  if (!mqttClient || !mqttClient.connected) return;

  mqttClient.publish(MQTT_TOPIC_COMMAND, JSON.stringify({ action }), { qos: 0 });
  appendLogEntry(`→ comando: ${action}`, 'arm');
}

/**
 * Publica a configuração de agendamento no tópico .../schedule.
 * retain: true — se o ESP32 estiver offline, o broker guarda e entrega no reconnect.
 * O ESP32 persiste em NVS e re-broadcasta o payload (retained).
 * @param {{enabled: boolean, armAt: string, disarmAt: string, weekdaysOnly: boolean}} config
 */
function publishSchedule(config) {
  if (!mqttClient || !mqttClient.connected) {
    scheduleStatusElement.textContent =
      'Sem conexão MQTT — horário não enviado.';
    scheduleStatusElement.className = 'schedule-status warn';
    return;
  }

  const payload = buildSchedulePayload(config);
  mqttClient.publish(MQTT_TOPIC_SCHEDULE, JSON.stringify(payload), { qos: 0, retain: true });
  const tone = lastNtpSynced === false && payload.enabled ? 'warn' : 'ok';
  renderScheduleStatus(payload, '', tone, lastNtpSynced);
  appendLogEntry(
    `→ agendamento: ${payload.armAt}–${payload.disarmAt}${payload.enabled ? '' : ' (inativo)'}`,
    'arm'
  );
}

/**
 * Handler do tópico de estado (armed / alarm / schedule).
 * @param {string} rawPayload
 */
function handleStateMessage(rawPayload) {
  const data = JSON.parse(rawPayload);
  renderSystemState(data.armed, data.alarm);

  if (typeof data.ntp === 'boolean') {
    lastNtpSynced = data.ntp;
  }

  // Complementa o card de agendamento se o firmware publicar o campo schedule.
  if (typeof data.schedule === 'string') {
    const form = readScheduleForm();
    const tone = lastNtpSynced === false && form.enabled ? 'warn' : 'ok';
    renderScheduleStatus(form, data.schedule, tone, lastNtpSynced);
  }
}

/**
 * Handler do tópico de alarme: registra no log e toca o bipe.
 * @param {string} rawPayload
 */
function handleAlarmMessage(rawPayload) {
  const data = JSON.parse(rawPayload);
  appendLogEntry(`🚨 ALARME: ${data.type} (${data.value || '-'})`, 'alarm');
  playAlarmBeep();
}

/**
 * Handler do tópico de log vindo do firmware.
 * @param {string} rawPayload
 */
function handleLogMessage(rawPayload) {
  const data = JSON.parse(rawPayload);
  appendLogEntry(data.event, classifyLogMessage(data.event || ''));
}

/**
 * Handler do tópico de sensores: distância, PIR e LDR.
 * @param {string} rawPayload
 */
function handleSensorMessage(rawPayload) {
  const data = JSON.parse(rawPayload);

  renderDistanceMetric(data.dist);
  renderPirMetric(data.pir);

  if (data.ldr !== undefined) {
    renderLightMetric(data.ldr, data.ldr_anomaly);
  }
}

/**
 * Handler do tópico de status (LWT) do ESP32.
 * @param {string} rawPayload
 */
function handleStatusMessage(rawPayload) {
  const isOnline = parseEspOnlineStatus(rawPayload);
  renderEspStatus(isOnline);
  appendLogEntry(`ESP32 ${rawPayload}`, isOnline ? 'arm' : 'alarm');
}

/**
 * Handler do tópico de agendamento (config retained do ESP32).
 * Preenche o formulário e o status do card.
 * @param {string} rawPayload
 */
function handleScheduleMessage(rawPayload) {
  const data = JSON.parse(rawPayload);
  fillScheduleForm(data);
  const tone = lastNtpSynced === false && data.enabled ? 'warn' : 'ok';
  renderScheduleStatus(data, '', tone, lastNtpSynced);
}

/**
 * Roteia a mensagem MQTT para o handler correspondente ao tópico.
 * Erros de parse são engolidos para não derrubar o listener.
 * @param {string} topic
 * @param {string} payload
 */
function routeMqttMessage(topic, payload) {
  const rawPayload = payload.toString();

  try {
    if (topic === MQTT_TOPIC_STATE) {
      handleStateMessage(rawPayload);
    } else if (topic === MQTT_TOPIC_ALARM) {
      handleAlarmMessage(rawPayload);
    } else if (topic === MQTT_TOPIC_LOG) {
      handleLogMessage(rawPayload);
    } else if (topic === MQTT_TOPIC_SENSORS) {
      handleSensorMessage(rawPayload);
    } else if (topic === MQTT_TOPIC_STATUS) {
      handleStatusMessage(rawPayload);
    } else if (topic === MQTT_TOPIC_SCHEDULE) {
      handleScheduleMessage(rawPayload);
    }
  } catch (error) {
    console.warn('Falha ao processar mensagem MQTT:', error);
  }
}

/**
 * Assina os tópicos de telemetria, estado e agendamento do sistema.
 */
function subscribeToSystemTopics() {
  mqttClient.subscribe([
    MQTT_TOPIC_STATE,
    MQTT_TOPIC_ALARM,
    MQTT_TOPIC_LOG,
    MQTT_TOPIC_SENSORS,
    MQTT_TOPIC_STATUS,
    MQTT_TOPIC_SCHEDULE,
  ]);
}

/**
 * Cria a conexão MQTT e liga os listeners de conexão/mensagens.
 * clientId aleatório por sessão evita colisão entre abas.
 */
function connectMqttClient() {
  const clientId = `web-sec-${Math.random().toString(16).slice(2, 10)}`;

  mqttClient = mqtt.connect(MQTT_BROKER_URL, {
    clientId,
    clean: true,
    reconnectPeriod: MQTT_RECONNECT_MS,
    connectTimeout: MQTT_CONNECT_TIMEOUT_MS,
  });

  mqttClient.on('connect', () => {
    connDot.classList.add('online');
    connDot.classList.remove('offline');
    connLabel.textContent = 'Conectado';
    appendLogEntry('Conectado ao broker', 'arm');
    subscribeToSystemTopics();
  });

  mqttClient.on('reconnect', () => {
    connLabel.textContent = 'Reconectando…';
  });

  mqttClient.on('offline', () => {
    connDot.classList.add('offline');
    connDot.classList.remove('online');
    connLabel.textContent = 'Desconectado';
    metricEsp.textContent = '--';
    metricEsp.className = 'metric-value muted';
    appendLogEntry('Conexão perdida', 'alarm');
  });

  mqttClient.on('message', routeMqttMessage);
}

/* =========================================================
   6. Fiação de eventos e bootstrap
   ========================================================= */

/**
 * Liga os botões de controle aos respectivos comandos MQTT.
 */
function wireControlButtons() {
  buttonArm.addEventListener('click', () => publishCommand(COMMAND_ARM));
  buttonDisarm.addEventListener('click', () => publishCommand(COMMAND_DISARM));
  buttonReset.addEventListener('click', () => publishCommand(COMMAND_RESET_ALARM));
  buttonTest.addEventListener('click', () => publishCommand(COMMAND_TEST_SIREN));
  buttonRecalibrate.addEventListener('click', () => publishCommand(COMMAND_RECALIBRATE));
}

/**
 * Liga o formulário de agendamento: valida e publica em .../schedule.
 */
function wireScheduleForm() {
  scheduleSaveButton.addEventListener('click', () => {
    const config = readScheduleForm();

    if (!isValidTimeString(config.armAt) || !isValidTimeString(config.disarmAt)) {
      renderScheduleStatus(config, '', 'error', lastNtpSynced);
      scheduleStatusElement.textContent =
        'Horários inválidos — use o formato HH:MM (00:00–23:59).';
      return;
    }

    publishSchedule(config);
  });
}

/**
 * Inicializa o painel no estado padrão e estabelece a conexão MQTT.
 */
function initializeDashboard() {
  renderSystemState(false, false);
  wireControlButtons();
  wireScheduleForm();
  renderScheduleStatus(readScheduleForm(), '', 'ok', lastNtpSynced);
  connectMqttClient();
}

initializeDashboard();
