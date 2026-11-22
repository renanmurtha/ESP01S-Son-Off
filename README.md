# esp01s-rele SonOff

Firmware para **ESP-01S (1 MB)** que controla um relé via **página web**, **MQTT (opcional)** e **agendamento diário**, com configuração por **WiFiManager** e atualização por **OTA**.

## Builds (com e sem MQTT)

O MQTT é **opcional em tempo de compilação**: quem não usa o recurso compila o firmware sem ele (código C++ e seção da página web compilados fora — flash e RAM menores, nenhum endpoint MQTT exposto).

O projeto tem **um único ambiente** (`esp01_1m`). O MQTT é controlado pela flag `USE_MQTT` no `platformio.ini`:

```ini
; COM MQTT (padrão do repositório):
build_flags = -DUSE_MQTT

; SEM MQTT: comente a linha acima
; build_flags = -DUSE_MQTT
```

| Estado da flag | Flash | RAM |
| --- | --- | --- |
| Ativa (com MQTT) | ~467 KB (48,7%) | 43,4% |
| Comentada (sem MQTT) | ~455 KB (47,5%) | 42,0% |

Compile com `pio run` (ou pelo botão do PlatformIO) após ajustar a flag.

## Hardware

| Item | Valor |
| --- | --- |
| Placa | ESP-01S (1 MB Flash) |
| Pino do relé | GPIO0 (`RELAY_PIN`) |
| Módulo do relé | Ativo em LOW por padrão (`RELAY_ACTIVE_LOW`) |
| Hostname / mDNS | `esp01s-rele` (`http://esp01s-rele.local`) |

> O GPIO0 é o pino de boot e precisa estar em `HIGH` ao iniciar. Por isso o firmware escreve `HIGH` no registrador da saída **antes** de configurar o pino como `OUTPUT`, evitando o clique do relé no boot.

Se o seu módulo de relé for ativo em `HIGH`, comente a linha `#define RELAY_ACTIVE_LOW`.

## Funcionalidades

### Controle do relé
- Liga/desliga por página web, por MQTT (build com MQTT) e por agendamento.
- O estado é persistido em `config.json` no SPIFFS e restaurado no boot.
- **Queda de energia:** se o reset foi causado por falta de energia (`Power On`/`External System`), o relé é reconciliado com a janela do agendamento assim que o relógio sincroniza — não fica no estado antigo esperando o próximo horário.
- No build com MQTT, o estado é publicado **somente quando muda** (mensagem retida) e a entrega é feita por um único ponto, com pendência em RAM: se o broker estiver fora, o próximo ciclo entrega o estado atual. Presença (`online`/`offline`) **não** é publicada nem gravada — ela é sessão e aparece apenas na página.

### Página web (`/`)
Interface em português, responsiva e em tema escuro, com:
- Cartão do relé com interruptor (switch) e rótulo `LIGADO`/`DESLIGADO` — o rótulo mostra apenas o estado do relé.
- Label de aviso na **linha do hostname, à direita** (11 px, uma linha): `Sincronizando...` enquanto o toggle aguarda o backend e `SEM CONEXÃO COM O APARELHO` quando a página perde contato com o ESP. Nesse estado o interruptor fica desabilitado. A linha tem altura fixa e não quebra, então a mensagem aparece e some sem mover o cartão nem o switch.
- Dois badges de texto fixo, **`Rede`** e **`MQTT`** (este só no build com MQTT e só quando o MQTT está configurado): a **cor** informa o estado — verde = ok, vermelho = fora, cinza = sem contato com o aparelho. O estado completo vai em `title`/`aria-label`.
- Exibição do endereço de acesso (hostname) e do relógio do aparelho.
- Rodapé separado por uma linha, com `Ligado há <tempo>` (uptime do aparelho — `min`, `h`, `d` até 366 dias e depois `a`, ex.: `37min`, `5h`, `9d`, `366d`, `1a`), heap livre e RSSI do Wi-Fi (RSSI só quando há estação conectada).
- `SEM CONEXÃO COM O APARELHO` aparece quando a página perde contato (falhas seguidas de sincronização); o cartão mantém o último estado conhecido e a mensagem some no primeiro sync bem-sucedido.
- Sincronização automática do estado a cada 2 segundos, sem sobrepor requisições (timeout de 5 s).
- Seções expansíveis de **Configuração** (nome mDNS, comum aos dois builds), **Automação** e, no build com MQTT, **MQTT**.

### Servidor web + API
| Rota | Método | Descrição |
| --- | --- | --- |
| `/` | GET | Página de controle |
| `/api/state` | GET | Estado do relé, diagnóstico de rede (Wi-Fi + motivo da última queda), MQTT (build com MQTT), relógio, uptime, heap, RSSI e mDNS |
| `/relay` | GET | Controla o relé (`?state=on\|off\|toggle`) |
| `/api/mqtt` | GET/POST | Configuração de MQTT (build **com** MQTT; o POST reinicia o aparelho). Não grava mais o nome mDNS |
| `/api/mdns` | POST | Salva o nome mDNS (**nos dois builds**; reinicia o aparelho) |
| `/api/schedule` | GET | Lê os agendamentos |
| `/api/schedule` | POST | Salva os agendamentos |

Resposta de `/api/state`:
```json
{ "relay": "on", "gpio": 0, "mqtt": true, "mqtt_connected": true, "time_synced": true, "clock": "08:30:00", "uptime": 3600, "heap": 38896, "rssi": -66, "wifi": 3, "wifi_reason": 0, "wifi_dropped_at": 0, "wifi_drops": 0, "mdns": "esp01s-rele" }
```

> `wifi` é o status do `WiFi.status()` (3 = conectado); `wifi_reason` é o motivo da última desconexão (0 = nenhuma desde o boot); `wifi_drops` conta as quedas desde o boot; `rssi` é `null` sem estação conectada. Esses campos existem **nos dois builds**.
> No build **sem** MQTT os campos `mqtt`/`mqtt_connected` não existem no JSON.

### MQTT *(build com MQTT)*
- **Habilitado** apenas quando há um servidor e uma porta válida (1–65535).
- **Tópico base** configurável (padrão `iot/leds/output`), com barra final removida automaticamente.
- Publica o estado no tópico base (**retido**) **apenas quando o estado muda**; a publicação existe em um único ponto (`mqttFlush()`).
- Se o MQTT estiver fora quando o relé muda, a publicação fica **pendente em RAM** (só o estado final, coalescido) e sai quando a conexão volta; um reboot antes disso descarta o envio, mas o relé continua no estado correto (persistido em `config.json`).
- Assina os comandos em `iot/leds/input`.
- Comandos aceitos: `ON`/`1`, `OFF`/`0` e `TOGGLE`.
- **Sem LWT**: presença (`online`/`offline`) é sessão, não status — não é publicada nem gravada; a página mostra o estado da rede e do MQTT em badges separados.
- Reconexão **não bloqueante** com backoff de 2s, 5s, 15s, 30s (a página web continua respondendo).
- Reconexão imediata quando o Wi-Fi volta.
- KeepAlive de 15s, socket timeout de 3s e buffer de 512 bytes.

Payload publicado no tópico base:
```json
{
  "device": "<hostname>",
  "collection": "iluminacao",
  "sensor": "Iluminação",
  "macAddress": "<MAC>",
  "chipId": "<chip id>",
  "message": { "state": "ON" }
}
```

O campo `collection` corresponde ao nome do **"banco de dados" do Mongo** (no ESP-01S, definido como `iluminacao`). Ele pode ser tratado no **Node-RED** para direcionar o documento à coleção correta antes de gravar no MongoDB.

### Integração com Node-RED *(build com MQTT)*
O MQTT pode rodar **dentro do próprio Node-RED** com o nó **`node-red-contrib-aedes`**, sem instalar nada além do Node-RED (dispensa um broker externo como Mosquitto).

No fluxo, o payload do ESP-01S é normalizado por uma **Function** e enviado ao MongoDB pelo nó **`node-red-contrib-mongodb4`**:

```javascript
/* payload complete */
let xyz = msg.payload

/* Get MQTT Topic by node topic */
var topic = msg.topic;
/* Dategenerated by node DateEvent */
var time = msg.time;

function formatDate(date) {
    var d = new Date(date),
        month = '' + (d.getMonth() + 1),
        day = '' + d.getDate(),
        year = d.getFullYear();

    if (month.length < 2)
        month = '0' + month;
    if (day.length < 2)
        day = '0' + day;

    return [year, month, day].join('/');
}

var date = formatDate(time)

/* Handling payload to generate notification and write to database */
var title = xyz.title;
var message = xyz.message;
var collection = xyz.collection;
var device = xyz.device;
var sensor = xyz.sensor;
var macaddress = xyz.macAddress;
var chipid = xyz.chipId;

/* If there is no message, stop the flow */
if (message) {
    /* If the message doesn't have a collection ("table"), I set one */
    collection == null ? collection = sensor : collection;

    /* If there is no device identified */
    device == null ? device = "Não identificado" : device;

    /*If the message doesn't have a title*/
    title == null ? title = "Importante" : title;

    /* New Payload */
    msg.payload = {}
    msg.title = title
    msg.collection = collection
    msg.payload.message = message
    msg.payload.timestamp = time
    msg.payload.date = date
    msg.payload.device = device
    msg.payload.macaddress = macaddress
    msg.payload.topic = topic
    msg.payload.sensor = sensor
    msg.payload.chipid = chipid
} else {
    msg.payload = null
}
return msg;
```

Fluxo resumido: **broker Aedes (Node-RED) → nó MQTT → Function (acima) → `node-red-contrib-mongodb4`**.

### Agendamento diário
- Horários de **ligar** e **desligar** independentes, no formato `HH:MM`, com opção de habilitar cada um.
- Relógio via **NTP** (`a.ntp.br`, `b.ntp.br`, `c.ntp.br`) com fuso fixo **BRT (UTC-3)**, sem horário de verão.
- Dispara uma única vez por dia (deduplicação por ano + dia do ano).
- Detecta o cruzamento do horário entre duas amostras do loop: se o loop atrasar, dispara no próximo ciclo; a primeira amostra após o boot/sincronismo não dispara **pela borda** (não há disparo duplicado ao ligar).
- **Reconciliação pós-queda de energia:** quando o reset foi causado por falta de energia (`Power On`/`External System`), o estado do relé é recalculado a partir da janela do agendamento na primeira leitura válida do relógio (depois do NTP) e corrigido se necessário — recupera a borda que ocorreu com o aparelho desligado. Com os dois horários habilitados vale a janela (inclusive virada de meia-noite); com apenas um, o evento só força o estado depois do horário. Reinícios por OTA, crash ou salvamento de configuração **não** reconciliam: mantêm o estado salvo.
- Se ligar e desligar estiverem no mesmo horário, os dois são ignorados com aviso no serial.
- Nada acontece enquanto o NTP não sincroniza.

### Configuração e persistência
- `config.json` (SPIFFS) guarda: hostname mDNS, estado do relé e agendamentos — e, no build com MQTT, servidor/porta/usuário/senha/tópico MQTT. (Campos MQTT de um config antigo são ignorados pelo build sem MQTT.)
- Gravação **atômica**: o novo conteúdo vai para `config.tmp` e só substitui o `config.json` depois de completo e fechado; o arquivo anterior vira `config.bak`. Se uma queda de energia truncar o `config.json`, o boot lê o `config.bak` — a programação não se perde.
- Normalizações automáticas: porta padrão `1883` quando há servidor sem porta, tópico padrão quando vazio, horários inválidos voltam para `06:00`/`22:00`, e o nome mDNS é truncado em 31 caracteres e limitado a `[A-Za-z0-9-_.]` (com remoção do sufixo `.local`).
- Salvar a configuração pela página **reinicia o aparelho** para aplicar tudo.
- Na página, deixar o campo de senha vazio **mantém a senha atual**.

### WiFiManager
- Sem credenciais salvas (primeiro uso): `autoConnect` abre o AP `ESP01S-RELE` normalmente.
- Com credenciais salvas: o boot tenta conectar por até **60 s** e, sem sucesso, **não abre o portal** — segue para o loop, onde a política de reconexão assume (portal só depois de 8 h offline).
- Parâmetros do portal: nome do aparelho na rede — e, no build com MQTT, servidor MQTT, porta, usuário, senha e tópico.
- Configuração salva é relida e normalizada após a conexão (vale para o portal do boot e para o portal de 8 h).

### Resiliência de Wi-Fi e diagnóstico
- **Uptime imune ao overflow de `millis()`**: o contador de 32 bits estoura em ~49,7 dias; o firmware acumula os segundos antes do estouro (`uptimeSeconds()`), então `Ligado há` continua correto depois disso.
- **Modem sleep desativado** (`WIFI_NONE_SLEEP`) em todos os cenários — evita quedas por *beacon timeout* com roteadores sensíveis.
- **Reconexão ativa com fallback** (o auto-reconnect do core pode falhar silenciosamente). Queda de rede nunca abre o configurador — nem no boot, nem durante o ciclo:
  | Tempo sem rede | Ação |
  | --- | --- |
  | Boot | Tenta conectar por até 60 s; falhou, segue offline para o loop |
  | 0–30 s | Apenas observa (tolera queda proposital, ex. manutenção do roteador) |
  | ≥ 30 s | Primeira tentativa de `WiFi.reconnect()`, depois a cada 5 minutos |
  | ≥ 8 h | Abre o portal do WiFiManager por até 5 min (cobre senha errada / rede removida) |

  No portal de 8 h: se houver configuração salva, o aparelho grava no SPIFFS e reinicia; fechou sem conectar (ou sem ninguém no AP), recomeça o ciclo de reconexão e o portal reabre a cada novo ciclo de 8 h.
- **Diagnóstico integrado** para identificar a causa de quedas e travamentos:
  - No boot, imprime o motivo do último reset (`ESP.getResetReason()` — distingue crash/soft WDT/brownout de um reboot normal).
  - Na desconexão, loga o **reason code** do core (ex. `BEACON_TIMEOUT`, `NO_AP_FOUND`); na reconexão, IP e RSSI.
  - A cada 60 s, log no serial: uptime, heap livre, RSSI e status Wi-Fi/MQTT (heap caindo ao longo de horas indica fragmentação).
- Ao voltar a rede: mDNS refeito (com fallback para `esp01s-rele`), backoff do MQTT zerado e reconexão imediata.

### Reset de fábrica (endpoint secreto)
Para recuperação do aparelho (ex.: perdido na rede com nome/IP que não lembra), existe um endpoint que apaga **toda** a configuração e volta ao estado de primeira utilização.

- **Defina a sua senha após clonar o projeto** (não commita): descomente e edite em `src/main.cpp`:
  ```cpp
  #define RESET_PASSWORD "sua-senha-aqui"
  ```
- **Sem o `#define` (padrão do repositório), o endpoint nem é registrado** — toda tentativa responde 404, como qualquer rota inexistente.
- Uso: acesse no navegador `http://<ip>/reset/p=<senha>`.
  - **Senha correta:** apaga `config.json`, `config.bak` e `config.tmp` (MQTT, mDNS, agendamentos, estado do relé), apaga as credenciais Wi-Fi (`ESP.eraseConfig()`) e reinicia — no boot, o WiFiManager abre o AP `ESP01S-RELE` para configurar do zero. Resposta: `{"ok":true,"restart":1}`.
  - **Senha errada:** responde 404 com 1 segundo de atraso (dificulta força bruta), sem revelar que a rota existe.
- Funciona nos dois builds (com e sem MQTT). Vale apenas na rede local, via HTTP — use para recuperação, não como mecanismo remoto de administração.

### OTA e rede
- **OTA via rede** (PlatformIO: `upload_protocol = espota` + `upload_port` apontando para o IP do aparelho ou `esp01s-rele.local`), com o hostname do OTA igual ao do mDNS. Para upload por USB, comente essas duas linhas que o PlatformIO detecta a porta serial automaticamente.
- Durante o OTA os reinícios agendados são cancelados, o loop de MQTT/agendamento é pausado e (no build com MQTT) o cliente MQTT é desconectado.
- Progresso do OTA exibido no serial a cada 10%.
- mDNS reiniciado automaticamente quando o Wi-Fi volta, com fallback para `esp01s-rele` se o hostname normalizado falhar.
- Ao salvar um nome mDNS novo, a página espera o aparelho reiniciar e redireciona para `http://<novo-nome>.local/` — comportamento idêntico nos dois builds (acesso por IP apenas recarrega).
- Logs no serial (115200 baud): estado do relé, mensagens MQTT (build com MQTT) e os diagnósticos da seção acima.

## Bibliotecas utilizadas

`ESP8266WiFi`, `FS (SPIFFS)`, `WiFiManager`, `ArduinoJson`, `ESP8266WebServer`, `ESP8266mDNS`, `ArduinoOTA` e `coredecls` — além de `PubSubClient` no build com MQTT.
