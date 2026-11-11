#include <ESP8266WiFi.h>
#include <FS.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#ifdef USE_MQTT
#include <PubSubClient.h>
#endif
#include <ArduinoOTA.h>
#include <time.h>
#include <coredecls.h>

// ESP-01S - 1 MB Flash

// Relé: GPIO0 (pino de boot - precisa estar HIGH ao iniciar)
// O módulo padrão é ativo em LOW. Se o seu for ativo em HIGH,
// comente a linha abaixo.
#define RELAY_ACTIVE_LOW

#ifdef RELAY_ACTIVE_LOW
#define RELAY_ON LOW
#define RELAY_OFF HIGH
#else
#define RELAY_ON HIGH
#define RELAY_OFF LOW
#endif

#define RELAY_PIN 0

// --------------------------------------------------
// ENDPOINT SECRETO DE RESET DE FÁBRICA (/reset/p=<senha>)
// Defina a SUA senha após clonar (não commita a senha).
// Com a linha comentada, o endpoint nem é registrado.
// --------------------------------------------------
//#define RESET_PASSWORD "sua-senha-aqui"
/* AP */
#define HOST_NAME "ESP01S-RELE"
/* Servidor web + mDNS (http://esp01s-rele.local) */
/* hostname para MDNS.begin, sem .local */
#define MDNS_HOST "esp01s-rele"

/* JSON config names (usados apenas no payload MQTT) */
#ifdef USE_MQTT
#define SENSOR_NAME "Iluminação"
#define MECADDRESS String(WiFi.macAddress())
#define CHIP_ID String(ESP.getChipId())
#define DEVICE_NAME String(WiFi.hostname())

#define COLLECTION "iluminacao"
#define MQTT_OUTPUT_TOPIC "iot/leds/output"
#define MQTT_INPUT_TOPIC "iot/leds/input"
#endif

#ifdef USE_MQTT
// Configurações MQTT
char mqtt_server[64] = "";
char mqtt_port[6] = "";
char mqtt_user[32] = "";
char mqtt_pass[64] = "";
char mqtt_topic[64] = "";
#endif
char mdns_host[32] = "";

bool shouldSaveConfig = false;
#ifdef USE_MQTT
bool mqttEnabled = false;
#endif

// Estado atual do relé (espelhado na página web)
bool relayState = false;

// Agendamento diário (fuso fixo BRT, UTC-3, horários no formato "HH:MM")
char schedOn[6] = "06:00";
char schedOff[6] = "22:00";
bool schedOnEn = false;
bool schedOffEn = false;

// Último dia (ano + dia do ano) em que cada rotina disparou
// (evita disparar várias vezes durante o mesmo minuto e gastar SPIFFS)
int schedOnYear = -1, schedOnYday = -1;
int schedOffYear = -1, schedOffYday = -1;

// Reinício agendado (0 = sem reinício pendente)
uint32_t restartAt = 0;

// OTA em andamento (pausa o loop do MQTT durante a gravação)
bool otaActive = false;

ESP8266WebServer server(80);

#ifdef USE_MQTT
// Cliente MQTT (tópicos derivados de mqtt_topic)
WiFiClient netClient;
PubSubClient mqttClient(netClient);

// Backoff de reconexão MQTT (não-bloqueante)
uint32_t mqttNextAttempt = 0;
uint8_t mqttRetry = 0;
#endif

// Reconexão ativa de Wi-Fi (o auto-reconnect do core pode falhar)
// Política: 5 min sem rede -> WiFi.reconnect() a cada 5 min (gentil,
// tolera queda proposital por manutenção); 8 h sem rede -> reinicia.
#define WIFI_RECONNECT_AFTER 300000UL   // 5 min
#define WIFI_RESTART_AFTER 28800000UL   // 8 h
uint32_t wifiDownSince = 0;             // 0 = Wi-Fi conectado
uint32_t wifiLastReconnect = 0;
uint32_t wifiNextStats = 0;             // próximo log de status (60 s)

// --------------------------------------------------
// Callback do WiFiManager
// --------------------------------------------------
void saveConfigCallback()
{
  shouldSaveConfig = true;
}

// --------------------------------------------------
// Normaliza a config MQTT (porta e tópico padrão)
// --------------------------------------------------
#ifdef USE_MQTT
void normalizeMqttConfig()
{
  if (strlen(mqtt_server) > 0 && atoi(mqtt_port) <= 0)
  {
    strlcpy(mqtt_port, "1883", sizeof(mqtt_port));
  }

  if (strlen(mqtt_topic) == 0)
  {
    strlcpy(mqtt_topic, MQTT_OUTPUT_TOPIC, sizeof(mqtt_topic));
  }
}
#endif

// --------------------------------------------------
// Normaliza o nome mDNS (hostname para MDNS.begin)
// --------------------------------------------------
void normalizeMdnsHost()
{
  String h = mdns_host;
  h.trim();

  if (h.endsWith(".local"))
  {
    h.remove(h.length() - 6);
  }

  h.replace(" ", "-");
  h.trim();

  // Só [A-Za-z0-9-]: hostname válido e JSON seguro
  String limpo = "";
  for (unsigned int i = 0; i < h.length(); i++)
  {
    char c = h[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')
    {
      limpo += c;
    }
  }
  h = limpo;
  h.trim();

  if (h.length() == 0)
  {
    h = MDNS_HOST;
  }

  if (h.length() > 31)
  {
    h = h.substring(0, 31);
  }

  strlcpy(mdns_host, h.c_str(), sizeof(mdns_host));
}

// --------------------------------------------------
// Agendamento: validação de "HH:MM"
// --------------------------------------------------
bool isValidHhMm(const char *s)
{
  if (strlen(s) != 5 || s[2] != ':')
  {
    return false;
  }

  for (int i = 0; i < 5; i++)
  {
    if (i == 2)
    {
      continue;
    }

    if (s[i] < '0' || s[i] > '9')
    {
      return false;
    }
  }

  int h = (s[0] - '0') * 10 + (s[1] - '0');
  int m = (s[3] - '0') * 10 + (s[4] - '0');

  return h >= 0 && h <= 23 && m >= 0 && m <= 59;
}

// Converte "HH:MM" para minutos do dia (0..1439)
int hhMmToMinutes(const char *s)
{
  return ((s[0] - '0') * 10 + (s[1] - '0')) * 60 + ((s[3] - '0') * 10 + (s[4] - '0'));
}

// Corrige horários inválidos vindo do config.json
void normalizeSchedule()
{
  if (!isValidHhMm(schedOn))
  {
    strlcpy(schedOn, "06:00", sizeof(schedOn));
  }

  if (!isValidHhMm(schedOff))
  {
    strlcpy(schedOff, "22:00", sizeof(schedOff));
  }
}

// Relógio do sistema já sincronizado via NTP (epoch > 2020-08-13)
bool timeSynced()
{
  return (int64_t)time(nullptr) > 1600000000;
}

// --------------------------------------------------
// Relógio local atual no formato "HH:MM:SS"
// (vazio enquanto o NTP não sincronizar)
// --------------------------------------------------
String relogioAtual()
{
  if (!timeSynced())
  {
    return "";
  }

  time_t agora = time(nullptr);
  struct tm t;
  localtime_r(&agora, &t);

  char buf[9];
  strftime(buf, sizeof(buf), "%H:%M:%S", &t);

  return String(buf);
}

// --------------------------------------------------
// Carrega as configurações
// --------------------------------------------------
void loadConfig()
{
  // Estado padrão: se qualquer etapa abaixo abortar,
  // o relé fica desligado.
  relayState = false;

  if (!SPIFFS.exists("/config.json"))
  {
    Serial.println("Configuracao inicial.");
    return;
  }

  File file = SPIFFS.open("/config.json", "r");

  if (!file)
  {
    Serial.println("Erro ao abrir config.json.");
    return;
  }

  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error)
  {
    Serial.println("Erro ao ler JSON.");
    return;
  }

#ifdef USE_MQTT
  strlcpy(mqtt_server, doc["mqtt_server"] | "", sizeof(mqtt_server));
  strlcpy(mqtt_port, doc["mqtt_port"] | "", sizeof(mqtt_port));
  strlcpy(mqtt_user, doc["mqtt_user"] | "", sizeof(mqtt_user));
  strlcpy(mqtt_pass, doc["mqtt_pass"] | "", sizeof(mqtt_pass));
  strlcpy(mqtt_topic, doc["mqtt_topic"] | MQTT_OUTPUT_TOPIC, sizeof(mqtt_topic));
#endif
  strlcpy(mdns_host, doc["mdns_host"] | MDNS_HOST, sizeof(mdns_host));

  relayState = doc["relay_state"] | false;

  strlcpy(schedOn, doc["sched_on"] | "06:00", sizeof(schedOn));
  strlcpy(schedOff, doc["sched_off"] | "22:00", sizeof(schedOff));
  schedOnEn = doc["sched_on_en"] | false;
  schedOffEn = doc["sched_off_en"] | false;

#ifdef USE_MQTT
  normalizeMqttConfig();
#endif
  normalizeMdnsHost();
  normalizeSchedule();
}

// --------------------------------------------------
// Salva as configurações
// --------------------------------------------------
bool saveConfig()
{
  StaticJsonDocument<1024> doc;

#ifdef USE_MQTT
  doc["mqtt_server"] = mqtt_server;
  doc["mqtt_port"] = mqtt_port;
  doc["mqtt_user"] = mqtt_user;
  doc["mqtt_pass"] = mqtt_pass;
  doc["mqtt_topic"] = mqtt_topic;
#endif
  doc["mdns_host"] = mdns_host;
  doc["relay_state"] = relayState;
  doc["sched_on"] = schedOn;
  doc["sched_off"] = schedOff;
  doc["sched_on_en"] = schedOnEn;
  doc["sched_off_en"] = schedOffEn;

  File file = SPIFFS.open("/config.json", "w");

  if (!file)
  {
    Serial.println("Erro ao salvar config.json.");
    return false;
  }

  bool ok = serializeJson(doc, file) > 0;
  file.close();

  if (ok)
  {
    Serial.println("Configuracoes salvas.");
  }

  return ok;
}

// --------------------------------------------------
// Tópicos MQTT derivados do base configurado:
//   <base>/set | <base>/state | <base>/availability
// --------------------------------------------------
#ifdef USE_MQTT
String mqttTopic(const char *suffix)
{
  String base = mqtt_topic;

  while (base.endsWith("/"))
  {
    base.remove(base.length() - 1);
  }

  return base + suffix;
}

// --------------------------------------------------
// Monta o JSON enviado ao broker
// --------------------------------------------------
String buildMqttJson(const char *availability, bool state)
{
  StaticJsonDocument<512> json;

  json["device"] = DEVICE_NAME;
  json["collection"] = COLLECTION;
  json["sensor"] = SENSOR_NAME;
  json["macAddress"] = MECADDRESS;
  json["chipId"] = CHIP_ID;

  JsonObject mensagem = json.createNestedObject("message");
  mensagem["availability"] = availability;
  mensagem["state"] = state ? "ON" : "OFF";

  String out;
  serializeJson(json, out);
  return out;
}

// --------------------------------------------------
// Publica o estado do relé (retido)
// --------------------------------------------------
void publishRelayState()
{
  if (!mqttClient.connected())
  {
    return;
  }

  String payload = buildMqttJson("online", relayState);

  if (mqttClient.publish(mqtt_topic, payload.c_str(), true))
  {
    Serial.print("MQTT -> ");
    Serial.println(payload);
  }
}
#endif // USE_MQTT

// --------------------------------------------------
// Controle do relé
// --------------------------------------------------
void setRelay(bool on, bool persist = true)
{
  bool changed = (on != relayState);

  relayState = on;
  digitalWrite(RELAY_PIN, on ? RELAY_ON : RELAY_OFF);

  Serial.print("Rele: ");
  Serial.println(on ? "ON" : "OFF");

  if (persist && changed)
  {
    saveConfig();
  }

  if (changed)
  {
#ifdef USE_MQTT
    publishRelayState();
#endif
  }
}

// --------------------------------------------------
// Agendamento diário: dispara LIGAR/DESLIGAR no horário
// configurado, uma única vez por dia (dedupe por data + horário).
// O relógio vem do NTP (fuso fixo BRT/UTC-3).
// O gatilho é por cruzamento entre duas amostras do loop: se o
// loop atrasar (MQTT/OTA/reboot), dispara atrasado no próximo
// loop; a primeira amostra após boot/sincronismo não dispara
// (nunca atuar atrasado logo ao ligar).
// --------------------------------------------------
void handleSchedule()
{
  if (!schedOnEn && !schedOffEn)
  {
    return;
  }

  if (!timeSynced())
  {
    return;
  }

  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);

  int minuteOfDay = t.tm_hour * 60 + t.tm_min;

  // Última amostra do relógio vista pelo loop (mantida entre chamadas)
  static bool temAmostra = false;
  static int anoAnt = -1, ydayAnt = -1, minutoAnt = -1;

  // Minuto-alvo que já disparou hoje (um por schedule)
  static int onDisparado = -1, offDisparado = -1;

  bool trocouODia = temAmostra && (anoAnt != t.tm_year || ydayAnt != t.tm_yday);

  // O alvo foi cruzado entre a amostra anterior e a atual?
  auto cruzou = [&](int alvo) -> bool
  {
    if (!temAmostra)
    {
      return false; // primeira amostra (boot/sincronismo): não dispara atrasado
    }

    if (trocouODia)
    {
      return minuteOfDay >= alvo; // virou o dia com o loop rodando
    }

    return minutoAnt < alvo && minuteOfDay >= alvo; // dispara atrasado no próximo loop
  };

  // Já disparou este horário hoje (dedupe leva em conta o alvo,
  // para permitir trocar o horário no meio do dia)
  auto jaDisparou = [&](int alvo, int disparado, int anoDisp, int ydayDisp) -> bool
  {
    return disparado == alvo && anoDisp == t.tm_year && ydayDisp == t.tm_yday;
  };

  int onMin = isValidHhMm(schedOn) ? hhMmToMinutes(schedOn) : -1;
  int offMin = isValidHhMm(schedOff) ? hhMmToMinutes(schedOff) : -1;

  bool conflito = schedOnEn && schedOffEn && onMin >= 0 && onMin == offMin;

  if (conflito)
  {
    // Ligar e desligar no mesmo horário: ignora os dois e avisa
    // (só no momento em que o horário seria disparado)
    if (onMin >= 0 && cruzou(onMin) &&
        !jaDisparou(onMin, onDisparado, schedOnYear, schedOnYday))
    {
      onDisparado = onMin;
      offDisparado = onMin;
      schedOnYear = t.tm_year;
      schedOnYday = t.tm_yday;
      schedOffYear = t.tm_year;
      schedOffYday = t.tm_yday;

      Serial.println("Agendamento: horario de ligar e desligar iguais, ignorado.");
    }
  }
  else
  {
    if (schedOnEn && onMin >= 0 && cruzou(onMin) &&
        !jaDisparou(onMin, onDisparado, schedOnYear, schedOnYday))
    {
      onDisparado = onMin;
      schedOnYear = t.tm_year;
      schedOnYday = t.tm_yday;

      Serial.print("Agendamento: ligar as ");
      Serial.println(schedOn);

      setRelay(true);
    }

    if (schedOffEn && offMin >= 0 && cruzou(offMin) &&
        !jaDisparou(offMin, offDisparado, schedOffYear, schedOffYday))
    {
      offDisparado = offMin;
      schedOffYear = t.tm_year;
      schedOffYday = t.tm_yday;

      Serial.print("Agendamento: desligar as ");
      Serial.println(schedOff);

      setRelay(false);
    }
  }

  // Base para a próxima comparação de cruzamento
  temAmostra = true;
  anoAnt = t.tm_year;
  ydayAnt = t.tm_yday;
  minutoAnt = minuteOfDay;
}

// --------------------------------------------------
// Verifica se o MQTT foi configurado
// --------------------------------------------------
#ifdef USE_MQTT
bool hasMqttConfig()
{
  return strlen(mqtt_server) > 0 && atoi(mqtt_port) > 0 && atoi(mqtt_port) <= 65535;
}

// --------------------------------------------------
// Callback de mensagens MQTT (tópico <base>/set)
// --------------------------------------------------
void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  String msg;

  for (unsigned int i = 0; i < length && i < 16; i++)
  {
    msg += (char)payload[i];
  }

  msg.trim();
  msg.toUpperCase();

  Serial.print("MQTT <- ");
  Serial.print(topic);
  Serial.print(": ");
  Serial.println(msg);

  if (msg == "ON" || msg == "1")
  {
    setRelay(true);
  }
  else if (msg == "OFF" || msg == "0")
  {
    setRelay(false);
  }
  else if (msg == "TOGGLE")
  {
    setRelay(!relayState);
  }
  else
  {
    Serial.println("Payload ignorado.");
  }
}

// --------------------------------------------------
// Conexão MQTT com LWT (availability)
// --------------------------------------------------
bool mqttConnect()
{
  String clientId = String(HOST_NAME) + "-" + String(ESP.getChipId());
  String willPayload = buildMqttJson("offline", relayState);

  bool ok;

  if (strlen(mqtt_user) > 0)
  {
    ok = mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass, mqtt_topic, 0, true, willPayload.c_str());
  }
  else
  {
    ok = mqttClient.connect(clientId.c_str(), mqtt_topic, 0, true, willPayload.c_str());
  }

  if (!ok)
  {
    Serial.print("MQTT falhou, rc=");
    Serial.println(mqttClient.state());
    return false;
  }

  // Estado atual (retido)
  mqttClient.publish(mqtt_topic, buildMqttJson("online", relayState).c_str(), true);

  mqttClient.subscribe(MQTT_INPUT_TOPIC);

  Serial.print("MQTT conectado, clientId: ");
  Serial.println(clientId);
  Serial.print("Comando: ");
  Serial.println(MQTT_INPUT_TOPIC);
  Serial.print("Heap livre: ");
  Serial.println(ESP.getFreeHeap());

  return true;
}

// --------------------------------------------------
// Loop MQTT: reconexão com backoff, sem bloquear a web
// --------------------------------------------------
void mqttLoop()
{
  if (!mqttEnabled)
  {
    return;
  }

  if (mqttClient.connected())
  {
    mqttClient.loop();
    return;
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    return;
  }

  if ((int32_t)(millis() - mqttNextAttempt) < 0)
  {
    return;
  }

  // Backoff: 2s, 5s, 15s, 30s (máximo)
  static const uint32_t delays[] = {2000, 5000, 15000, 30000};
  uint8_t step = (mqttRetry < 4) ? mqttRetry : 3;

  Serial.print("MQTT conectando (tentativa ");
  Serial.print((uint16_t)(mqttRetry + 1));
  Serial.println(")...");

  if (mqttConnect())
  {
    mqttRetry = 0;
    return;
  }

  mqttNextAttempt = millis() + delays[step];

  if (mqttRetry < 4)
  {
    mqttRetry++;
  }
}
#endif // USE_MQTT

// --------------------------------------------------
// Página web (embutida em flash)
// --------------------------------------------------
static const char PAGE_INDEX[] PROGMEM =
  R"HTML(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP01S - Rele</title>
<style>
:root{color-scheme:dark;--on:#22c55e;--off:#3a4150;--blue:#3b82f6}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:12px;
background:#0f1115;color:#e8eaed;
font-family:system-ui,-apple-system,"Segoe UI",Roboto,Arial,sans-serif}
.card{width:min(94vw,340px);background:#171a21;border:1px solid #262b36;border-radius:14px;
padding:16px;box-shadow:0 8px 24px rgba(0,0,0,.45)}
.top{display:flex;align-items:baseline;justify-content:space-between;gap:8px;flex-wrap:wrap}
h1{margin:0;font-size:clamp(15px,4.5vw,18px);font-weight:600;letter-spacing:.4px}
.badge{font-size:11px;padding:2px 8px;border-radius:10px;background:#232833;color:#8a91a0}
.badge.on{background:rgba(34,197,94,.16);color:var(--on)}
.ip{margin:4px 0 14px;display:flex;justify-content:space-between;align-items:baseline;
font-size:clamp(11px,3vw,13px);color:#8a91a0;word-break:break-all}
.row{display:flex;align-items:center;justify-content:space-between;gap:12px;
background:#12151c;border:1px solid #262b36;border-radius:12px;padding:14px}
.row .lbl{font-size:15px;font-weight:600}
.sw{position:relative;display:inline-block;width:60px;height:32px;flex:0 0 auto}
.sw input{opacity:0;width:0;height:0}
.track{position:absolute;inset:0;cursor:pointer;background:var(--off);border-radius:32px;transition:.2s}
.track:before{content:"";position:absolute;left:4px;top:4px;width:24px;height:24px;border-radius:50%;
background:#fff;transition:.2s}
.sw input:checked+.track{background:var(--on);box-shadow:0 0 16px rgba(34,197,94,.45)}
.sw input:checked+.track:before{transform:translateX(28px)}
input:disabled+.track{opacity:.6}
.aviso{display:none;font-size:11px;font-weight:600;color:#f87171}
.aviso.on{display:block}
details{margin-top:14px;border-top:1px solid #262b36;padding-top:12px}
summary{cursor:pointer;font-size:14px;font-weight:600;color:#8a91a0;list-style:none}
summary::-webkit-details-marker{display:none}
summary:after{content:" +";color:var(--blue)}
details[open] summary:after{content:" \2212"}
form{margin-top:10px}
label{display:block;margin:10px 0 4px;font-size:12px;color:#8a91a0}
input{width:100%;padding:10px;border:1px solid #262b36;border-radius:8px;background:#12151c;
color:#e8eaed;font-size:16px}
input:focus{outline:none;border-color:var(--blue)}
input[type=time]{max-width:160px}
.chk{display:flex;align-items:center;gap:8px;margin:12px 0 4px;font-size:13px;color:#8a91a0;cursor:pointer}
.chk input{appearance:auto;flex:0 0 auto;width:auto;height:auto;padding:0;border:none;
background:none;margin:0;accent-color:var(--on)}
button{width:100%;margin-top:14px;padding:12px;border:none;border-radius:8px;cursor:pointer;
background:var(--blue);color:#fff;font-size:15px;font-weight:600}
button:active{opacity:.85}
.msg{margin-top:8px;text-align:center;font-size:13px;min-height:18px;color:var(--on)}
.hint{margin-top:14px;text-align:center;font-size:11px;color:#5c6470}
</style>
</head>
<body>
<div class="card">
<div class="top"><h1>ESP01S &middot; Rele</h1>
)HTML"
#ifdef USE_MQTT
  R"HTML(<span class="badge" id="mb">MQTT</span>)HTML"
#endif
  R"HTML(</div>
<p class="ip"><span id="ip"></span><span class="aviso" id="aviso"></span></p>
<div class="row">
<span class="lbl" id="lb">...</span>
<label class="sw"><input type="checkbox" id="sw" onchange="tgl(this.checked)"><span class="track"></span></label>
</div>
)HTML"
#ifdef USE_MQTT
  R"HTML(<details>
<summary>Configuracao</summary>
<form onsubmit="return saveMqtt(event)">
<label for="f6">URL (DNS)</label>
<input id="f6" name="mdns" autocomplete="off">
<button type="submit" id="sb">Salvar</button>
<div class="msg" id="msg"></div>
</form>
</details>
)HTML"
#else
  R"HTML(<details>
<summary>Configuracao</summary>
<form onsubmit="return saveMdns(event)">
<label for="f6">URL (DNS)</label>
<input id="f6" name="mdns" autocomplete="off">
<button type="submit" id="sb">Salvar</button>
<div class="msg" id="msg"></div>
</form>
</details>
)HTML"
#endif
  R"HTML(<details>
<summary>Automacao</summary>
<form onsubmit="return saveSched(event)">
<label class="chk" for="c1"><input type="checkbox" id="c1" name="on_en"> Ligar as</label>
<input type="time" id="f7" name="on" autocomplete="off">
<label class="chk" for="c2"><input type="checkbox" id="c2" name="off_en"> Desligar as</label>
<input type="time" id="f8" name="off" autocomplete="off">
<button type="submit" id="sb2">Salvar agendamento</button>
<div class="msg" id="msg2"></div>
</form>
<div class="hint" id="clk">Relogio: --</div>
</details>
)HTML"
#ifdef USE_MQTT
  R"HTML(<details>
<summary>MQTT</summary>
<form onsubmit="return saveMqtt(event)">
<label for="f1">Servidor</label>
<input id="f1" name="server" autocomplete="off">
<label for="f2">Porta</label>
<input id="f2" name="port" inputmode="numeric" autocomplete="off">
<label for="f3">Usuario</label>
<input id="f3" name="user" autocomplete="off">
<label for="f4">Senha</label>
<input id="f4" name="pass" type="password" autocomplete="new-password">
<label for="f5">Topico</label>
<input id="f5" name="topic" autocomplete="off">
<button type="submit" id="sb3">Salvar</button>
<div class="msg" id="msg3"></div>
</form>
</details>
)HTML"
#endif
  R"HTML(<div class="hint" id="ftr">Estado sincronizado a cada 2s</div>
</div>
<script>
var sw=document.getElementById('sw'),
lb=document.getElementById('lb'),
c1=document.getElementById('c1'),f7=document.getElementById('f7'),
c2=document.getElementById('c2'),f8=document.getElementById('f8'),
sb2=document.getElementById('sb2'),msg2=document.getElementById('msg2'),
clk=document.getElementById('clk'),ftr=document.getElementById('ftr'),
aviso=document.getElementById('aviso');
)HTML"
#ifdef USE_MQTT
  R"HTML(var mb=document.getElementById('mb');
)HTML"
#endif
  R"HTML(document.getElementById('ip').textContent=location.hostname;
var emAcao=false;
function avisoOn(t){aviso.textContent=t;aviso.className='aviso on';}
function avisoOff(){aviso.textContent='';aviso.className='aviso';}

function render(on){
 sw.checked=on;
 lb.textContent=on?'LIGADO':'DESLIGADO';
}
function sync(){
 fetch('/api/state',{cache:'no-store'})
  .then(function(r){return r.json();})
  .then(function(d){
    if(!emAcao)render(d.relay==='on');
)HTML"
#ifdef USE_MQTT
  R"HTML(    mb.textContent=d.mqtt_connected?'MQTT ativo':(d.mqtt?'MQTT offline':'MQTT off');
    mb.className='badge'+(d.mqtt_connected?' on':'');
)HTML"
#endif
  R"HTML(    clk.textContent=d.time_synced?('Relogio do aparelho: '+d.clock):'Aguardando sincronismo NTP';
    if(d.uptime!==undefined)ftr.textContent='Ativo '+fmtUp(d.uptime)+' \u00B7 Heap '+d.heap+' \u00B7 RSSI '+d.rssi+' dBm';
    avisoOff();
  })
  .catch(function(){avisoOn('SEM CONEXAO');});
}
function fmtUp(u){
 if(u>=86400)return (u/86400).toFixed(u>=864000?0:1)+'d';
 if(u>=3600)return (u/3600).toFixed(u>=36000?0:1)+'h';
 return Math.floor(u/60)+'min';
}
function tgl(on){
 emAcao=true;
 sw.disabled=true;
 avisoOn('Sincronizando...');
 fetch('/relay?state='+(on?'on':'off'))
  .then(function(r){return r.json();})
  .then(function(d){
   render(d.relay==='on');
   avisoOff();
  })
  .catch(sync)
  .finally(function(){
   emAcao=false;
   sw.disabled=false;
  });
}
)HTML"
#ifdef USE_MQTT
  R"HTML(function saveMqtt(e){
 e.preventDefault();
 // Formulario de origem (Configuracao ou MQTT): botao e msg sao locais ao form
 var form=e.target,btn=form.querySelector('button[type=submit]'),fm=form.querySelector('.msg');
 btn.disabled=true; fm.textContent='Salvando...';
 // O POST /api/mqtt grava todos os campos de uma vez: envia o payload completo
 var body={server:f1.value.trim(),port:f2.value.trim(),user:f3.value.trim(),
           pass:f4.value,topic:f5.value.trim(),mdns:f6.value.trim()};
 fetch('/api/mqtt',{method:'POST',body:JSON.stringify(body)})
  .then(function(r){return r.json().then(function(d){return {ok:r.ok,d:d};});})
  .then(function(o){
   if(!o.ok){fm.textContent=o.d.error||'Erro ao salvar';btn.disabled=false;return;}
   var n=o.d.restart||1;
   fm.textContent='Salvo! Reiniciando em '+n+'...';
   var iv=setInterval(function(){
    n--;
    if(n<=0){
     clearInterval(iv);
     fm.textContent='Reiniciando...';
     var novo=(o.d.mdns||'').toLowerCase();
     var atual=(location.hostname||'').toLowerCase().replace(/\.local$/,'');
     var ehIp=/^[0-9]{1,3}(\.[0-9]{1,3}){3}$/.test(location.hostname);
     if(novo && atual!==novo && !ehIp){
      esperaBoot('http://'+novo+'.local/',function(){location.href='http://'+novo+'.local/';});
     }else{
      esperaBoot('',function(){
       btn.disabled=false;
       fm.textContent='Configuracao aplicada!';
       carregaForm();
       sync();
      });
     }
     return;
    }
    fm.textContent='Salvo! Reiniciando em '+n+'...';
   },1000);
  })
  .catch(function(){fm.textContent='Falha ao salvar';btn.disabled=false;});
 return false;
}
function carregaForm(){
 fetch('/api/mqtt')
  .then(function(r){return r.json();})
  .then(function(d){
   f1.value=d.server||''; f2.value=d.port||''; f3.value=d.user||''; f5.value=d.topic||'';
   f6.value=d.mdns||'';
   f4.value='';
   f4.placeholder=d.pass_set?'Vazia = mantem a atual':'Sem senha';
  })
  .catch(function(){});
}
)HTML"
#endif
  R"HTML(function carregaSched(){
 fetch('/api/schedule')
  .then(function(r){return r.json();})
  .then(function(d){
   f7.value=d.on||''; f8.value=d.off||'';
   c1.checked=!!d.on_enabled; c2.checked=!!d.off_enabled;
  })
  .catch(function(){});
}
function saveSched(e){
 e.preventDefault();
 sb2.disabled=true; msg2.textContent='Salvando...';
 var body={on:f7.value,off:f8.value,on_enabled:c1.checked,off_enabled:c2.checked};
 fetch('/api/schedule',{method:'POST',body:JSON.stringify(body)})
  .then(function(r){return r.json().then(function(d){return {ok:r.ok,d:d};});})
  .then(function(o){
   sb2.disabled=false;
   if(!o.ok){msg2.textContent=o.d.error||'Erro ao salvar';return;}
   msg2.textContent='Agendamento salvo!';
   carregaSched();
  })
  .catch(function(){msg2.textContent='Falha ao salvar';sb2.disabled=false;});
 return false;
}
)HTML"
  R"HTML(// Espera o aparelho voltar (max 15s). alvo='' usa a origem atual.
function esperaBoot(alvo,cb){
 var t=0;
 var iv=setInterval(function(){
  t++;
  fetch(alvo+'/api/state',{cache:'no-store'})
   .then(function(r){clearInterval(iv);cb();})
   .catch(function(){if(t>=15){clearInterval(iv);cb();}});
 },1000);
}
)HTML"
#ifdef USE_MQTT
  R"HTML(carregaForm();
)HTML"
#else
  R"HTML(function saveMdns(e){
 e.preventDefault();
 var form=e.target,btn=form.querySelector('button[type=submit]'),fm=form.querySelector('.msg');
 btn.disabled=true; fm.textContent='Salvando...';
 fetch('/api/mdns',{method:'POST',body:JSON.stringify({mdns:f6.value.trim()})})
  .then(function(r){return r.json().then(function(d){return {ok:r.ok,d:d};});})
  .then(function(o){
   if(!o.ok){fm.textContent=o.d.error||'Erro ao salvar';btn.disabled=false;return;}
   var n=o.d.restart||1;
   fm.textContent='Salvo! Reiniciando em '+n+'...';
   var iv=setInterval(function(){
    n--;
    if(n<=0){
     clearInterval(iv);
     fm.textContent='Reiniciando...';
     esperaBoot('',function(){location.reload();});
     return;
    }
    fm.textContent='Salvo! Reiniciando em '+n+'...';
   },1000);
  })
  .catch(function(){fm.textContent='Falha ao salvar';btn.disabled=false;});
 return false;
}
function carregaMdns(){
 fetch('/api/state',{cache:'no-store'})
  .then(function(r){return r.json();})
  .then(function(d){f6.value=d.mdns||'';})
  .catch(function(){});
}
carregaMdns();
)HTML"
#endif
  R"HTML(carregaSched();
sync();
setInterval(sync,2000);
</script>
</body>
</html>
)HTML";

// --------------------------------------------------
// Rotas HTTP
// --------------------------------------------------
void handleIndex()
{
  server.send_P(200, "text/html", PAGE_INDEX);
}

void handleState()
{
  String json = "{\"relay\":\"";
  json += relayState ? "on" : "off";
  json += "\",\"gpio\":";
  json += String(RELAY_PIN);
#ifdef USE_MQTT
  json += ",\"mqtt\":";
  json += mqttEnabled ? "true" : "false";
  json += ",\"mqtt_connected\":";
  json += (mqttEnabled && mqttClient.connected()) ? "true" : "false";
#endif
  json += ",\"time_synced\":";
  json += timeSynced() ? "true" : "false";
  json += ",\"clock\":\"";
  json += relogioAtual();
  json += "\",\"uptime\":";
  json += String(millis() / 1000UL);
  json += ",\"heap\":";
  json += String(ESP.getFreeHeap());
  json += ",\"rssi\":";
  json += String(WiFi.RSSI());
  json += ",\"mdns\":\"";
  json += mdns_host;
  json += "\"}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleRelay()
{
  if (!server.hasArg("state"))
  {
    server.send(400, "application/json", "{\"error\":\"state required\"}");
    return;
  }

  String s = server.arg("state");
  s.toLowerCase();

  if (s == "on" || s == "off")
  {
    setRelay(s == "on");
  }
  else if (s == "toggle")
  {
    setRelay(!relayState);
  }
  else
  {
    server.send(400, "application/json", "{\"error\":\"invalid state\"}");
    return;
  }

  handleState();
}

#ifdef RESET_PASSWORD
// --------------------------------------------------
// Endpoint secreto: apaga config.json e as credenciais
// Wi-Fi e reinicia (volta para o AP do WiFiManager).
// Senha errada responde 404 com atraso (anti brute force),
// sem revelar que a rota existe.
// --------------------------------------------------
void handleSecretReset(const String &senha)
{
  if (senha != RESET_PASSWORD)
  {
    Serial.println("Reset: senha incorreta.");
    delay(1000);
    server.send(404, "text/plain", "Not found");
    return;
  }

  Serial.println("Reset: acionado, apagando configuracao...");

  if (SPIFFS.exists("/config.json"))
  {
    SPIFFS.remove("/config.json");
  }

  ESP.eraseConfig();

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "{\"ok\":true,\"restart\":1}");

  delay(100);
  ESP.restart();
}
#endif

void handleNotFound()
{
#ifdef RESET_PASSWORD
  const char *prefixo = "/reset/p=";

  if (server.uri().startsWith(prefixo))
  {
    handleSecretReset(server.uri().substring(strlen(prefixo)));
    return;
  }
#endif

  server.send(404, "text/plain", "Not found");
}

// --------------------------------------------------
// Configuração MQTT pela página
// --------------------------------------------------
#ifdef USE_MQTT
void handleMqttGet()
{
  String json = "{\"server\":\"";
  json += mqtt_server;
  json += "\",\"port\":\"";
  json += mqtt_port;
  json += "\",\"user\":\"";
  json += mqtt_user;
  json += "\",\"topic\":\"";
  json += mqtt_topic;
  json += "\",\"pass_set\":";
  json += strlen(mqtt_pass) > 0 ? "true" : "false";
  json += ",\"mdns\":\"";
  json += mdns_host;
  json += "\"}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleMqttPost()
{
  StaticJsonDocument<768> doc;

  if (deserializeJson(doc, server.arg("plain")))
  {
    server.send(400, "application/json", "{\"error\":\"json invalido\"}");
    return;
  }

  const char *port = doc["port"] | "";

  if (port[0] != '\0')
  {
    int p = atoi(port);

    if (p < 1 || p > 65535)
    {
      server.send(400, "application/json", "{\"error\":\"porta invalida\"}");
      return;
    }
  }

  strlcpy(mqtt_server, doc["server"] | "", sizeof(mqtt_server));
  strlcpy(mqtt_port, port, sizeof(mqtt_port));
  strlcpy(mqtt_user, doc["user"] | "", sizeof(mqtt_user));
  strlcpy(mqtt_topic, doc["topic"] | MQTT_OUTPUT_TOPIC, sizeof(mqtt_topic));
  strlcpy(mdns_host, doc["mdns"] | MDNS_HOST, sizeof(mdns_host));

  // Senha vazia = mantém a atual
  const char *pass = doc["pass"] | "";

  if (strlen(pass) > 0)
  {
    strlcpy(mqtt_pass, pass, sizeof(mqtt_pass));
  }

  normalizeMqttConfig();
  normalizeMdnsHost();

  if (!saveConfig())
  {
    server.sendHeader("Cache-Control", "no-store");
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"falha ao salvar\"}");
    return;
  }

  mqttEnabled = hasMqttConfig();

  restartAt = millis() + 1000;

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json",
              "{\"ok\":true,\"restart\":1,\"mdns\":\"" + String(mdns_host) + "\"}");
}
#else
// Build SEM MQTT: o formulário "Configuracao" salva só o nome mDNS
void handleMdnsPost()
{
  StaticJsonDocument<256> doc;

  if (deserializeJson(doc, server.arg("plain")))
  {
    server.send(400, "application/json", "{\"error\":\"json invalido\"}");
    return;
  }

  strlcpy(mdns_host, doc["mdns"] | MDNS_HOST, sizeof(mdns_host));
  normalizeMdnsHost();

  if (!saveConfig())
  {
    server.sendHeader("Cache-Control", "no-store");
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"falha ao salvar\"}");
    return;
  }

  restartAt = millis() + 1000;

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json",
              "{\"ok\":true,\"restart\":1,\"mdns\":\"" + String(mdns_host) + "\"}");
}
#endif // USE_MQTT

// --------------------------------------------------
// Agendamento diário (ligar/desligar em horário fixo)
// --------------------------------------------------
void handleScheduleGet()
{
  String json = "{\"on\":\"";
  json += schedOn;
  json += "\",\"off\":\"";
  json += schedOff;
  json += "\",\"on_enabled\":";
  json += schedOnEn ? "true" : "false";
  json += ",\"off_enabled\":";
  json += schedOffEn ? "true" : "false";
  json += "}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleSchedulePost()
{
  StaticJsonDocument<768> doc;

  if (deserializeJson(doc, server.arg("plain")))
  {
    server.send(400, "application/json", "{\"error\":\"json invalido\"}");
    return;
  }

  String on = doc["on"] | "";
  String off = doc["off"] | "";

  if (!isValidHhMm(on.c_str()) || !isValidHhMm(off.c_str()))
  {
    server.send(400, "application/json", "{\"error\":\"horario invalido (use HH:MM)\"}");
    return;
  }

  strlcpy(schedOn, on.c_str(), sizeof(schedOn));
  strlcpy(schedOff, off.c_str(), sizeof(schedOff));
  schedOnEn = doc["on_enabled"] | false;
  schedOffEn = doc["off_enabled"] | false;

  if (!saveConfig())
  {
    server.sendHeader("Cache-Control", "no-store");
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"falha ao salvar\"}");
    return;
  }

  Serial.print("Agendamento salvo: ligar=");
  Serial.print(schedOnEn ? schedOn : "off");
  Serial.print(" desligar=");
  Serial.println(schedOffEn ? schedOff : "off");

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "{\"ok\":true}");
}

// --------------------------------------------------
// Inicialização
// --------------------------------------------------
void setup()
{
  Serial.begin(115200);
  delay(100);

  // Diagnóstico: revela se o chip reiniciou sozinho (crash/WDT/brownout)
  Serial.print("Ultimo reset: ");
  Serial.println(ESP.getResetReason());

  // Primeiro lê o estado salvo, depois aplica no pino.
  // A ordem (digitalWrite antes do pinMode) evita o clique
  // no boot nos módulos ativos em LOW.
  bool fsOk = SPIFFS.begin();

  if (fsOk)
  {
    loadConfig();
  }
  else
  {
    Serial.println("Falha ao montar SPIFFS.");
  }

  // digitalWrite(RELAY_PIN, relayState ? RELAY_ON : RELAY_OFF);
  // pinMode(RELAY_PIN, OUTPUT);

  // 1. Garante que o registrador de saída está em HIGH antes de virar OUTPUT
  digitalWrite(RELAY_PIN, HIGH);
  pinMode(RELAY_PIN, OUTPUT);

  // 2. Logo em seguida, aplica o estado que estava salvo na memória
  digitalWrite(RELAY_PIN, relayState ? RELAY_ON : RELAY_OFF);

  if (fsOk)
  {
    setRelay(relayState, false);
  }

  // Rádio sempre ativo: o modem sleep padrão causa quedas periódicas
  // (beacon timeout) com alguns roteadores.
  WiFi.setSleepMode(WIFI_NONE_SLEEP);

  // Diagnóstico de queda/conexão com o motivo exato informado pelo core
  WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected &info)
                                 {
    Serial.print("Wi-Fi desconectado (motivo ");
    Serial.print(info.reason);
    Serial.print(": ");
    Serial.print(info.ssid);
    Serial.println(")"); });

  WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP &info)
                          {
    Serial.print("Wi-Fi conectado, IP: ");
    Serial.print(info.ip.toString());
    Serial.print(" RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm"); });

  WiFiManager wm;
  wm.setSaveConfigCallback(saveConfigCallback);

  WiFiManagerParameter p_mdns("mdns", "Nome do aparelho na rede", mdns_host, sizeof(mdns_host));

#ifdef USE_MQTT
  WiFiManagerParameter p_server("server", "Servidor MQTT (opcional)", mqtt_server, sizeof(mqtt_server));

  WiFiManagerParameter p_port("port", "Porta MQTT", mqtt_port, sizeof(mqtt_port));

  WiFiManagerParameter p_user("user", "Usuario MQTT (opcional)", mqtt_user, sizeof(mqtt_user));

  WiFiManagerParameter p_pass("pass", "Senha MQTT (opcional)", mqtt_pass, sizeof(mqtt_pass));

  WiFiManagerParameter p_topic("topic", "Topico MQTT", mqtt_topic, sizeof(mqtt_topic));

  wm.addParameter(&p_server);
  wm.addParameter(&p_port);
  wm.addParameter(&p_user);
  wm.addParameter(&p_pass);
  wm.addParameter(&p_topic);
#endif
  wm.addParameter(&p_mdns);

  if (!wm.autoConnect(HOST_NAME))
  {
    delay(1000);
    ESP.restart();
    return;
  }

#ifdef USE_MQTT
  strlcpy(mqtt_server, p_server.getValue(), sizeof(mqtt_server));
  strlcpy(mqtt_port, p_port.getValue(), sizeof(mqtt_port));
  strlcpy(mqtt_user, p_user.getValue(), sizeof(mqtt_user));
  strlcpy(mqtt_pass, p_pass.getValue(), sizeof(mqtt_pass));
  strlcpy(mqtt_topic, p_topic.getValue(), sizeof(mqtt_topic));
#endif
  strlcpy(mdns_host, p_mdns.getValue(), sizeof(mdns_host));

#ifdef USE_MQTT
  normalizeMqttConfig();
#endif
  normalizeMdnsHost();

  if (shouldSaveConfig)
  {
    saveConfig();
  }

#ifdef USE_MQTT
  mqttEnabled = hasMqttConfig();

  if (mqttEnabled)
  {
    mqttClient.setServer(mqtt_server, atoi(mqtt_port));
    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(15);
    mqttClient.setSocketTimeout(3);
    mqttClient.setBufferSize(512);
  }
#endif

  Serial.println();
  Serial.println("Wi-Fi conectado!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // Relógio do sistema: fuso fixo BRT (UTC-3, sem horário de verão)
  // e servidores NTP.br para o agendamento diário.
  configTzTime("<-03>3", "a.ntp.br", "b.ntp.br", "c.ntp.br");

#ifdef USE_MQTT
  if (mqttEnabled)
  {
    Serial.println("MQTT habilitado.");
    Serial.print("Servidor: ");
    Serial.println(mqtt_server);
    Serial.print("Porta: ");
    Serial.println(mqtt_port);
    Serial.print("Topico: ");
    Serial.println(mqtt_topic);
    Serial.print("Autenticacao: ");
    Serial.println(strlen(mqtt_user) > 0 ? "Configurada" : "Nao utilizada");
  }
  else
  {
    Serial.println("MQTT desabilitado.");
  }
#endif

  // Servidor web + mDNS (http://esp01s-rele.local)
  if (MDNS.begin(mdns_host))
  {
    Serial.println("mDNS ativo: http://" + String(mdns_host) + ".local");
  }

  // OTA via rede (PlatformIO: upload_protocol = espota)
  // O hostname precisa ser o mesmo do mDNS, senão o ArduinoOTA
  // registra outro nome e o esp01s-rele.local deixa de resolver.
  ArduinoOTA.setHostname(mdns_host);

  ArduinoOTA.onStart([]()
                     {
    Serial.println("OTA: iniciando...");
    restartAt = 0; // não reinicia no meio da gravação

#ifdef USE_MQTT
    if (mqttClient.connected())
    {
      mqttClient.disconnect();
    }
#endif

    otaActive = true; });

  ArduinoOTA.onEnd([]()
                   { Serial.println("\nOTA: concluido, reiniciando..."); });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        {
    static int lastDecade = -1;
    int pct = (progress * 100) / total;
    int decade = pct / 10;

    if (decade != lastDecade)
    {
      lastDecade = decade;
      Serial.printf("OTA: %u%%\n", pct);
    } });

  ArduinoOTA.onError([](ota_error_t error)
                     {
    Serial.printf("OTA erro [%u]\n", error);
    otaActive = false; });

  ArduinoOTA.begin();

  server.on("/", HTTP_GET, handleIndex);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/relay", HTTP_GET, handleRelay);
#ifdef USE_MQTT
  server.on("/api/mqtt", HTTP_GET, handleMqttGet);
  server.on("/api/mqtt", HTTP_POST, handleMqttPost);
#else
  server.on("/api/mdns", HTTP_POST, handleMdnsPost);
#endif
  server.on("/api/schedule", HTTP_GET, handleScheduleGet);
  server.on("/api/schedule", HTTP_POST, handleSchedulePost);
  server.onNotFound(handleNotFound);
  server.begin();

  MDNS.addService("http", "tcp", 80);
}

void loop()
{
  ArduinoOTA.handle();
  server.handleClient();
  MDNS.update();

  if (!otaActive)
  {
#ifdef USE_MQTT
    mqttLoop();
#endif

    // Agendamento diário (ligar/desligar em horário fixo)
    handleSchedule();
  }

  // Reinício agendado após salvar a configuração
  if (restartAt != 0 && (int32_t)(millis() - restartAt) >= 0)
  {
    Serial.println("Reiniciando para aplicar a nova configuracao...");
    delay(100);
    ESP.restart();
  }

  // Log de queda/restauração do Wi-Fi + reconexão ativa.
  // (o auto-reconnect do core pode falhar; aqui garantimos o fallback)
  static bool wasConnected = true;
  bool connected = (WiFi.status() == WL_CONNECTED);
  uint32_t agoraMs = millis();

  if (connected)
  {
    wifiDownSince = 0;
  }
  else
  {
    if (wifiDownSince == 0)
    {
      wifiDownSince = agoraMs;
      wifiLastReconnect = agoraMs;
    }

    // 8 h sem rede: reinicia (o relé volta com o estado salvo no SPIFFS
    // e o agendamento continua pelo relógio interno; com a rede fora,
    // ex. manutenção longa, o reboot não afeta a automação)
    if ((uint32_t)(agoraMs - wifiDownSince) >= WIFI_RESTART_AFTER)
    {
      Serial.println("Wi-Fi offline ha 8h, reiniciando...");
      delay(100);
      ESP.restart();
      return;
    }

    // 5 min sem rede: tenta reconectar a cada 5 min
    if ((uint32_t)(agoraMs - wifiLastReconnect) >= WIFI_RECONNECT_AFTER)
    {
      wifiLastReconnect = agoraMs;

      Serial.print("Wi-Fi offline ha ");
      Serial.print((agoraMs - wifiDownSince) / 60000UL);
      Serial.println(" min, tentando reconectar...");
      WiFi.reconnect();
    }
  }

  // Status periódico (60 s): estabilidade, sinal e heap ao longo do tempo
  if ((int32_t)(agoraMs - wifiNextStats) >= 0)
  {
    wifiNextStats = agoraMs + 60000UL;

    Serial.print("[status] uptime=");
    Serial.print(agoraMs / 1000UL);
    Serial.print("s heap=");
    Serial.print(ESP.getFreeHeap());
    Serial.print(" rssi=");
    Serial.print(WiFi.RSSI());
    Serial.print(" wifi=");
    Serial.print(connected ? "ok" : "down");
    Serial.print(" mqtt=");
#ifdef USE_MQTT
    Serial.println((mqttEnabled && mqttClient.connected()) ? "ok" : "down");
#else
    Serial.println("off");
#endif
  }

  if (connected != wasConnected)
  {
    wasConnected = connected;

    if (connected)
    {
      // Wi-Fi voltou: reinicia o mDNS
      MDNS.close();
      bool mdnsOk = MDNS.begin(mdns_host);
      if (!mdnsOk)
      {
        // Fallback: hostname normalizado inválido; tenta padrão "esp01s-rele"
        Serial.println("mDNS falhou com hostname normalizado; tentando padrão...");
        mdnsOk = MDNS.begin(MDNS_HOST);
      }
      if (mdnsOk)
      {
        MDNS.addService("http", "tcp", 80);
        MDNS.update();
      }

#ifdef USE_MQTT
      // Wi-Fi voltou: tenta MQTT imediatamente
      mqttRetry = 0;
      mqttNextAttempt = 0;
#endif

      Serial.print("Wi-Fi reconectado. IP: ");
      Serial.println(WiFi.localIP());
    }
    else
    {
      Serial.println("Wi-Fi desconectado, tentando reconectar...");
    }
  }
}
