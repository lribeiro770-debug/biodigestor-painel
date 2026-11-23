// =============================================================================
//  biodigestor_monitor.ino — Monitoramento remoto de biodigestor  (v2)
//  UTFPR Campo Mourão — EEL6006
//
//  Sensores: 2x DS18B20 (digestato, 2 profundidades) · DHT11 (ambiente/UR)
//            MQ-2 e MQ-7 (detecção de VAZAMENTO, fora do biodigestor)
//            Pressão MPS20N0040D+HX710B (ou analógico) · reed (opcional)
//  Atuação : relé (alarme local OU válvula do método manométrico) · buzzer
//  Rede    : portal WiFi pelo celular · MQTT (JSON) · fila em flash · NTP
//            Telegram (alertas) · OTA por WiFi local e por URL (remoto)
//
//  Bibliotecas: PubSubClient, OneWire, DallasTemperature, ArduinoJson (v7),
//               WiFiManager (tzapu), DHTesp (beegee-tokyo)
//  Placa: "ESP32 Dev Module" — core esp32 3.x (testado 3.1.3)
//
//  Serial (115200) — digite "ajuda".  MQTT: TOPIC/cmd — ver README.
// =============================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHTesp.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>
#include <time.h>
#include <esp_wifi.h>
#include <esp_sntp.h>
#include "config.h"

#define FW_VERSAO "3.0.8"

// ============================================================================
//                         CONFIGURAÇÃO EM TEMPO DE EXECUÇÃO
// ============================================================================
struct Cfg {
  String host, user, pass, topic, token, tgToken, tgChat, apPass;
  uint16_t port;
  bool tls;
  float vHead;
} cfg;

Preferences prefs;

static void cfgCarregar() {
  prefs.begin("cfg", true);
  cfg.host    = prefs.getString("host",  DEF_MQTT_HOST);
  cfg.port    = prefs.getUShort("port",  DEF_MQTT_PORT);
  cfg.tls     = prefs.getBool("tls",     DEF_MQTT_TLS);
  cfg.user    = prefs.getString("user",  DEF_MQTT_USER);
  cfg.pass    = prefs.getString("pass",  DEF_MQTT_PASS);
  cfg.topic   = prefs.getString("topic", DEF_TOPIC_BASE);
  cfg.token   = prefs.getString("token", DEF_CMD_TOKEN);
  cfg.tgToken = prefs.getString("tgtok", DEF_TG_TOKEN);
  cfg.tgChat  = prefs.getString("tgchat",DEF_TG_CHAT);
  cfg.vHead   = prefs.getFloat("vhead",  DEF_V_HEAD_L);
  cfg.apPass  = prefs.getString("ap_pass", PORTAL_SENHA);
  prefs.end();
  if (!cfg.host.length()) cfg.host = DEF_MQTT_HOST;     // corrige configuração apagada pelo portal (fw ≤ 3.0.2)
  if (!cfg.port)          cfg.port = DEF_MQTT_PORT;
  if (!cfg.topic.length()) cfg.topic = DEF_TOPIC_BASE;
}

// Grava uma chave. Retorna false se a chave não existir.
static bool cfgSet(const String& k, const String& v) {
  prefs.begin("cfg", false);
  bool ok = true;
  if      (k == "host")   { cfg.host = v;          prefs.putString("host", v); }
  else if (k == "port")   { cfg.port = v.toInt();  prefs.putUShort("port", cfg.port); }
  else if (k == "tls")    { cfg.tls = v.toInt();   prefs.putBool("tls", cfg.tls); }
  else if (k == "user")   { cfg.user = v;          prefs.putString("user", v); }
  else if (k == "pass")   { cfg.pass = v;          prefs.putString("pass", v); }
  else if (k == "topic")  { cfg.topic = v;         prefs.putString("topic", v); }
  else if (k == "token")  { cfg.token = v;         prefs.putString("token", v); }
  else if (k == "tgtok")  { cfg.tgToken = v;       prefs.putString("tgtok", v); }
  else if (k == "tgchat") { cfg.tgChat = v;        prefs.putString("tgchat", v); }
  else if (k == "vhead")  { cfg.vHead = v.toFloat(); prefs.putFloat("vhead", cfg.vHead); }
  else if (k == "ap_pass") { if (v.length() < 8) { prefs.end(); return false; } cfg.apPass = v; prefs.putString("ap_pass", v); }
  else ok = false;
  prefs.end();
  return ok;
}

static String mascara(const String& s) { return s.length() ? String("***(") + s.length() + ")" : String("(vazio)"); }

static String cfgTexto() {
  return "host=" + cfg.host + " port=" + cfg.port + " tls=" + cfg.tls + " user=" + cfg.user +
         " pass=" + mascara(cfg.pass) + "\ntopic=" + cfg.topic + " token=" + mascara(cfg.token) +
         "\ntgtok=" + mascara(cfg.tgToken) + " tgchat=" + cfg.tgChat + " vhead=" + String(cfg.vHead, 2) + " L";
}

// ============================================================================
//                               OBJETOS
// ============================================================================
WiFiClient netPlain;
WiFiClientSecure netTls;
PubSubClient mqtt;
WiFiManager wm;
OneWire oneWire(PIN_ONEWIRE);
DallasTemperature ds(&oneWire);
DHTesp dht;

String tTelem, tStatus, tCmd, tEvento, tDiario, tDiarioHist;
static const char* FILA_ARQ = "/fila.jsonl";
static const char* FILA_OFF = "/fila.off";

// ============================================================================
//                               ESTADO
// ============================================================================
struct SensorMQ {
  const char* nome; uint8_t pino; float rl;
  float r0, r0T, r0UR;     // Rs em ar limpo + condições da calibração
  float vAdc, vAo, rs; bool valido;
};
SensorMQ mq2 = {"mq2", PIN_MQ2_AO, MQ2_RL, 0, NAN, NAN, 0, 0, NAN, false};
SensorMQ mq7 = {"mq7", PIN_MQ7_AO, MQ7_RL, 0, NAN, NAN, 0, 0, NAN, false};

DeviceAddress addr1 = ADDR_DIG1, addr2 = ADDR_DIG2;
float tDig1 = NAN, tDig2 = NAN, tAmb = NAN, urAmb = NAN;
bool dsConvertendo = false; uint32_t dsPedidoMs = 0;

// Taxa de variação da temperatura: amostra a cada 5 min, janela de 60 min
float histT[13]; uint8_t histN = 0; uint32_t tUltHist = 0;
float taxaTdig = NAN;

// Pressão
float pressaoPa = NAN;
bool pressaoOk = false;
#if PRESSAO_TIPO == 2
int32_t hxBuf[HX_MEDIA_N]; uint8_t hxIdx = 0, hxCnt = 0;
int32_t hxRaw = 0;
float hxOffset = 0, hxEscala = HX_ESCALA_PADRAO;
uint32_t hxUltLeitura = 0;
#endif

// Válvula (método manométrico)
enum EstValv { V_FECHADA, V_ABERTA, V_ESTAB };
EstValv valv = V_FECHADA;
uint32_t valvT0 = 0, valvCiclos = 0, ultCicloMs = 0;
float valvP0 = NAN, volNl = 0, vazaoNlh = 0;
uint32_t volSalvoMs = 0;

// Medidor por pulsos
volatile uint32_t pulsosIsr = 0, ultimoPulsoIsrMs = 0, intervaloPulsoMs = 0;
uint32_t pulsosTotal = 0, pulsosSalvos = 0, ultimoSaveNvsMs = 0;

// MQ-7 ciclo
bool mq7FaseAlta = true; uint32_t mq7FaseInicioMs = 0;
float mq7RsUltimoCiclo = NAN, mq7Ppm = NAN;

// Alarmes
enum Alarme : uint8_t {
  A_GAS_COMB, A_MQ7, A_CO, A_SOBREPRESSAO,                 // segurança (acionam relé/buzzer)
  A_T_BAIXA, A_T_ALTA, A_T_TAXA, A_ESTRAT, A_UR,           // processo
  A_S_T1, A_S_T2, A_S_DHT, A_S_MQ2, A_S_MQ7, A_S_PRESS,    // sensores
  A_MQ_NCAL, A_N
};
const char* ALARME_NOME[A_N] = {
  "GAS_COMBUSTIVEL", "MQ7_ALTERADO", "CO_ALTO", "SOBREPRESSAO",
  "T_DIG_BAIXA", "T_DIG_ALTA", "T_DIG_VARIACAO_RAPIDA", "ESTRATIFICACAO", "UMIDADE_ALTA",
  "SENSOR_T_DIG1", "SENSOR_T_DIG2", "SENSOR_DHT", "SENSOR_MQ2", "SENSOR_MQ7", "SENSOR_PRESSAO",
  "MQ_NAO_CALIBRADO"
};
const char* ALARME_TXT[A_N] = {
  "Possível VAZAMENTO de gás combustível (MQ-2)", "MQ-7 alterado (gás/CO)", "CO acima do limite",
  "SOBREPRESSÃO no biodigestor", "Temperatura do digestato BAIXA", "Temperatura do digestato ALTA",
  "Variação rápida de temperatura", "Estratificação (falta de mistura)", "Umidade alta (condensação)",
  "Sonda DS18B20 #1 com falha", "Sonda DS18B20 #2 com falha", "DHT com falha", "MQ-2 com falha",
  "MQ-7 com falha", "Sensor de pressão com falha", "MQ não calibrados"
};
const uint32_t ALARMES_SEGURANCA = (1u << A_GAS_COMB) | (1u << A_MQ7) | (1u << A_CO) | (1u << A_SOBREPRESSAO);
uint32_t alarmesAtivos = 0;            // após persistência
uint8_t  alCont[A_N] = {0};
uint32_t silenciadoAte = 0;
bool releLigado = false;

// Telegram (fila simples)
String tgFila[8]; uint8_t tgIni = 0, tgFim = 0; uint32_t tgUltEnvio = 0;

// Simulação em tempo de execução (comando simular:1 / simular:0 — gravado na flash)
bool simAtivo = MODO_SIMULADO;
uint32_t simVazamentoAte = 0, simUlt = 0, simCiclos = 0, simUltCicloMs = 0;
float simP = 200.0f, simVol = 0.0f, simVazao = 0.0f;
bool simValv = false;

uint32_t seq = 0, bootMs = 0;
uint32_t tUltPub = 0, tUltTemp = 0, tUltGas = 0, tUltDht = 0, tUltAlarme = 0, tUltMqttTry = 0, tUltWifiTry = 0;
bool fsOk = false, portalAtivo = false, otaIniciado = false, ntpIniciado = false;
// Coleta offline (coleta.h)
bool sdOk = false; uint32_t nUltimo = 0;
static void evento(const char* tipo, const String& detalhe);
static String diarioResumo();
static String sdStatus();

// ============================================================================
//                               UTILITÁRIOS
// ============================================================================
static bool aquecido() { return (millis() - bootMs) / 1000UL >= MQ_WARMUP_S; }
static uint32_t epochAgora() { time_t n = time(nullptr); return n > 1700000000 ? (uint32_t)n : 0; }
static bool addrZerado(const DeviceAddress a) { for (int i = 0; i < 8; i++) if (a[i]) return false; return true; }
static void addrStr(const DeviceAddress a, char* out) {
  for (int i = 0; i < 8; i++) sprintf(out + i * 5, "0x%02X%s", a[i], i < 7 ? "," : "");
}
static float r2(float v) { return isnan(v) ? v : roundf(v * 100.0f) / 100.0f; }
// Recebe o "proxy" do ArduinoJson (doc["chave"]) por referência: convertê-lo para
// JsonVariant antes de a chave existir descartaria a atribuição (bug da v2.0.0).
template <typename T>
static void jnum(T&& d, float v, int casas = 2) {
  if (isnan(v)) d = nullptr; else { float m = powf(10, casas); d = roundf(v * m) / m; }
}
static bool alarme(Alarme a) { return alarmesAtivos & (1u << a); }

static void wdtInit() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t c = { .timeout_ms = WDT_TIMEOUT_S * 1000, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_deinit(); esp_task_wdt_init(&c);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);
}

// ============================================================================
//                               TELEGRAM
// ============================================================================
static String urlEncode(const String& s) {
  String o; char b[4];
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { sprintf(b, "%%%02X", c); o += b; }
  }
  return o;
}

static void tgEnfileirar(const String& msg) {
  if (!cfg.tgToken.length() || !cfg.tgChat.length()) return;
  uint8_t prox = (tgFim + 1) % 8;
  if (prox == tgIni) tgIni = (tgIni + 1) % 8;     // descarta a mais antiga
  tgFila[tgFim] = msg; tgFim = prox;
}

static void tgTick() {
  if (tgIni == tgFim || !WiFi.isConnected() || millis() - tgUltEnvio < TG_MIN_INTERVALO_MS) return;
  tgUltEnvio = millis();
  WiFiClientSecure c; c.setInsecure(); c.setTimeout(8);
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(c, "https://api.telegram.org/bot" + cfg.tgToken + "/sendMessage")) return;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String corpo = "chat_id=" + urlEncode(cfg.tgChat) + "&text=" + urlEncode("[" DEVICE_ID "] " + tgFila[tgIni]);
  esp_task_wdt_reset();
  int rc = http.POST(corpo);
  http.end();
  if (rc == 200 || rc == 400 || rc == 401 || rc == 404) tgIni = (tgIni + 1) % 8;  // 4xx = config errada: não insiste
  if (rc != 200) Serial.printf("[TG] falha HTTP %d\n", rc);
}

// ============================================================================
//                               MQ (ADC)
// ============================================================================
static float lerTensaoPino(uint8_t pino) {
  uint32_t soma = 0; const int N = 32;
  for (int i = 0; i < N; i++) { soma += analogReadMilliVolts(pino); delayMicroseconds(200); }
  return (soma / (float)N) / 1000.0f;
}

static void lerMQ(SensorMQ& s) {
  s.vAdc = lerTensaoPino(s.pino);
  s.vAo  = s.vAdc * (DIV_R_TOP + DIV_R_BOT) / DIV_R_BOT;
  if (s.vAo < 0.02f || s.vAdc > ADC_V_MAX) { s.rs = NAN; s.valido = false; return; }
  const float rDiv = DIV_R_TOP + DIV_R_BOT, rlEf = s.rl * rDiv / (s.rl + rDiv);
  s.rs = rlEf * (MQ_VC - s.vAo) / s.vAo;
  s.valido = aquecido() && s.rs > 0;
}

// Leitura dentro das condições (T/UR) em que o R0 foi calibrado?
static bool mqCondOk(const SensorMQ& s) {
  if (isnan(s.r0T) || isnan(tAmb)) return true;
  if (fabsf(tAmb - s.r0T) > MQ_COND_DT_C) return false;
  if (!isnan(s.r0UR) && !isnan(urAmb) && fabsf(urAmb - s.r0UR) > MQ_COND_DUR) return false;
  return true;
}

static void carregarCalibracao() {
  prefs.begin("bio", true);
  mq2.r0 = prefs.getFloat("r0_mq2", 0); mq7.r0 = prefs.getFloat("r0_mq7", 0);
  mq2.r0T = mq7.r0T = prefs.getFloat("r0_t", NAN);
  mq2.r0UR = mq7.r0UR = prefs.getFloat("r0_ur", NAN);
  pulsosTotal = prefs.getUInt("pulsos", 0);
  volNl = prefs.getFloat("vol_nl", 0);
  simAtivo = prefs.getBool("sim", MODO_SIMULADO);
  valvCiclos = prefs.getUInt("ciclos", 0);
#if PRESSAO_TIPO == 2
  hxOffset = prefs.getFloat("hx_off", 0);
  hxEscala = prefs.getFloat("hx_esc", HX_ESCALA_PADRAO);
#endif
  prefs.end();
  pulsosSalvos = pulsosTotal;
}

static String calibrarMQ(bool forcar = false) {
  uint32_t up = (millis() - bootMs) / 1000UL;
  if (!forcar && up < MQ_CAL_MIN_S)
    return "calibração recusada: só " + String(up / 60) + " min ligado. Espere " + String((MQ_CAL_MIN_S - up + 59) / 60) +
           " min (no 1º uso, 24-48 h) em ar limpo. Para forçar em bancada: cal_forcar";
  if (!aquecido()) return "erro: sensores ainda aquecendo";
  lerMQ(mq2);
  float rs7 = MQ7_HEATER_CYCLE ? mq7RsUltimoCiclo : (lerMQ(mq7), mq7.rs);
  if (isnan(mq2.rs) || isnan(rs7)) return "erro: leitura inválida";
  mq2.r0 = mq2.rs; mq7.r0 = rs7;
  mq2.r0T = mq7.r0T = tAmb; mq2.r0UR = mq7.r0UR = urAmb;
  prefs.begin("bio", false);
  prefs.putFloat("r0_mq2", mq2.r0); prefs.putFloat("r0_mq7", mq7.r0);
  prefs.putFloat("r0_t", tAmb); prefs.putFloat("r0_ur", urAmb);
  prefs.end();
  char m[128];
  snprintf(m, sizeof m, "MQ calibrados: R0_mq2=%.0f ohm, R0_mq7=%.0f ohm (T=%.1f C, UR=%.0f%%)", mq2.r0, mq7.r0, tAmb, urAmb);
  return String(m);
}

#if MQ7_HEATER_CYCLE
static const uint32_t PWM_FREQ = 1000; static const uint8_t PWM_BITS = 10;
static const uint32_t DUTY_ALTO = (1 << PWM_BITS) - 1;
static const uint32_t DUTY_BAIXO = (uint32_t)(((1 << PWM_BITS) - 1) * 0.0784f);   // (1,4/5)^2
static void heaterDuty(uint32_t d) { ledcWrite(PIN_MQ7_HEATER, d); }
static void mq7HeaterInit() { ledcAttach(PIN_MQ7_HEATER, PWM_FREQ, PWM_BITS); mq7FaseInicioMs = millis(); heaterDuty(DUTY_ALTO); }
static void mq7HeaterTick() {
  uint32_t dt = millis() - mq7FaseInicioMs;
  if (mq7FaseAlta && dt >= MQ7_ALTO_MS) { mq7FaseAlta = false; mq7FaseInicioMs = millis(); heaterDuty(DUTY_BAIXO); }
  else if (!mq7FaseAlta) {
    if (dt >= MQ7_BAIXO_MS - MQ7_JANELA_LEIT_MS && dt < MQ7_BAIXO_MS) { lerMQ(mq7); if (mq7.valido) mq7RsUltimoCiclo = mq7.rs; }
    if (dt >= MQ7_BAIXO_MS) {
      mq7FaseAlta = true; mq7FaseInicioMs = millis(); heaterDuty(DUTY_ALTO);
      if (!isnan(mq7RsUltimoCiclo) && mq7.r0 > 0) mq7Ppm = MQ7_CO_A * powf(mq7RsUltimoCiclo / (mq7.r0 / MQ7_RAZAO_AR_LIMPO), MQ7_CO_B);
    }
  }
}
#endif

// ============================================================================
//                               TEMPERATURA / UMIDADE
// ============================================================================
static void dsInit() {
  ds.begin(); ds.setWaitForConversion(false);
  int n = ds.getDeviceCount();
  Serial.printf("[DS18B20] %d sonda(s) no GPIO%d\n", n, PIN_ONEWIRE);
  DeviceAddress a; char buf[48];
  for (int i = 0; i < n; i++) if (ds.getAddress(a, i)) {
    addrStr(a, buf); Serial.printf("  [%d] {%s}\n", i, buf); ds.setResolution(a, DS_RESOLUCAO_BITS);
  }
  if (addrZerado(addr1) && n >= 1) ds.getAddress(addr1, 0);
  if (addrZerado(addr2) && n >= 2) ds.getAddress(addr2, 1);
  if (n < 2) Serial.println("[DS18B20] AVISO: esperado 2 sondas (pull-up 4k7? ligações?)");
}

static float validaTemp(float t) {
  if (t == DEVICE_DISCONNECTED_C || t < -55 || t > 125 || t == 85.0f) return NAN;
  return t;
}

static void dsTick() {
  uint32_t agora = millis();
  if (!dsConvertendo && agora - tUltTemp >= TEMP_INTERVAL_MS) {
    ds.requestTemperatures(); dsConvertendo = true; dsPedidoMs = tUltTemp = agora;
  }
  if (dsConvertendo && agora - dsPedidoMs >= 800) {
    tDig1 = validaTemp(ds.getTempC(addr1)); tDig2 = validaTemp(ds.getTempC(addr2));
    dsConvertendo = false;
  }
  // histórico p/ taxa °C/h
  if (!isnan(tDig1) && (agora - tUltHist >= 300000UL || histN == 0)) {
    tUltHist = agora;
    if (histN < 13) histT[histN++] = tDig1;
    else { memmove(histT, histT + 1, 12 * sizeof(float)); histT[12] = tDig1; }
    if (histN == 13) taxaTdig = histT[12] - histT[0];   // 12 intervalos de 5 min = 1 h
  }
}

static void dhtTick() {
  if (millis() - tUltDht < DHT_INTERVAL_MS) return;
  tUltDht = millis();
  TempAndHumidity th = dht.getTempAndHumidity();
  if (dht.getStatus() == DHTesp::ERROR_NONE && !isnan(th.temperature) && th.humidity > 0) {
    tAmb = th.temperature; urAmb = th.humidity;
  } else { tAmb = NAN; urAmb = NAN; }
}

// ============================================================================
//                               PRESSÃO
// ============================================================================
#if PRESSAO_TIPO == 2
// HX710B: DOUT em LOW = dado pronto. 24 bits MSB primeiro + 1 pulso (entrada diferencial, 10 Hz).
static bool hxLer(int32_t& out) {
  if (digitalRead(PIN_HX_DOUT)) return false;
  uint32_t v = 0;
  noInterrupts();
  for (int i = 0; i < 24; i++) {
    digitalWrite(PIN_HX_SCK, HIGH); delayMicroseconds(1);
    v = (v << 1) | digitalRead(PIN_HX_DOUT);
    digitalWrite(PIN_HX_SCK, LOW); delayMicroseconds(1);
  }
  digitalWrite(PIN_HX_SCK, HIGH); delayMicroseconds(1); digitalWrite(PIN_HX_SCK, LOW);
  interrupts();
  if (v & 0x800000) v |= 0xFF000000;       // extensão de sinal
  out = (int32_t)v;
  return true;
}

static float hxMedia() {
  if (!hxCnt) return NAN;
  int64_t s = 0; for (uint8_t i = 0; i < hxCnt; i++) s += hxBuf[i];
  return (float)s / hxCnt;
}

static void pressaoTick() {
  int32_t v;
  if (hxLer(v)) {
    hxBuf[hxIdx] = v; hxIdx = (hxIdx + 1) % HX_MEDIA_N; if (hxCnt < HX_MEDIA_N) hxCnt++;
    hxRaw = v; hxUltLeitura = millis();
  }
  pressaoOk = hxCnt > 0 && millis() - hxUltLeitura < 2000 && hxRaw != 0x7FFFFF && hxRaw != (int32_t)0xFF800000;
  pressaoPa = pressaoOk ? (hxMedia() - hxOffset) / hxEscala : NAN;
}

static String pressaoZerar() {
  float m = hxMedia(); if (isnan(m)) return "erro: sem leitura do HX710B";
  hxOffset = m; prefs.begin("bio", false); prefs.putFloat("hx_off", m); prefs.end();
  return "pressão zerada (offset=" + String(m, 0) + ")";
}
static String pressaoCal(float pa) {
  float m = hxMedia(); if (isnan(m) || fabsf(pa) < 50) return "erro: use referência >= 50 Pa (5 cmH2O)";
  float e = (m - hxOffset) / pa; if (fabsf(e) < 1) return "erro: variação pequena demais — sensor ligado?";
  hxEscala = e; prefs.begin("bio", false); prefs.putFloat("hx_esc", e); prefs.end();
  return "pressão calibrada: " + String(e, 2) + " contagens/Pa";
}
#elif PRESSAO_TIPO == 1
static void pressaoTick() {
  static uint32_t t = 0; if (millis() - t < 1000) return; t = millis();
  float v = lerTensaoPino(PIN_PRESSAO_ANALOG) * (DIV_R_TOP + DIV_R_BOT) / DIV_R_BOT;
  pressaoOk = v > 0.2f;
  pressaoPa = pressaoOk ? constrain((v / MQ_VC * 5.0f - P_V_ZERO) / P_V_SPAN, -0.05f, 1.1f) * P_FUNDO_ESCALA_KPA * 1000.0f : NAN;
}
static String pressaoZerar() { return "não suportado no sensor analógico"; }
static String pressaoCal(float) { return "não suportado no sensor analógico"; }
#else
static void pressaoTick() {}
static String pressaoZerar() { return "sem sensor de pressão"; }
static String pressaoCal(float) { return "sem sensor de pressão"; }
#endif

// ============================================================================
//                               RELÉ / BUZZER / VÁLVULA
// ============================================================================
static void releSet(bool on) {
  releLigado = on;
#if RELE_MODO
#if RELE_DRENO_ABERTO && RELE_ATIVO_BAIXO
  if (on) { digitalWrite(PIN_RELE, LOW); pinMode(PIN_RELE, OUTPUT); digitalWrite(PIN_RELE, LOW); }
  else    { pinMode(PIN_RELE, INPUT); }          // solto: o próprio módulo puxa o IN para o VCC e desliga
#else
  pinMode(PIN_RELE, OUTPUT);
  digitalWrite(PIN_RELE, (on ^ RELE_ATIVO_BAIXO) ? HIGH : LOW);
#endif
#endif
}

static void salvarVolume(bool forcar) {
  if (!forcar && millis() - volSalvoMs < 60000UL) return;
  volSalvoMs = millis();
  prefs.begin("bio", false); prefs.putFloat("vol_nl", volNl); prefs.putUInt("ciclos", valvCiclos); prefs.end();
}

#if RELE_MODO == 2
static void valvulaTick() {
  uint32_t agora = millis();
  // Falha do sensor: válvula ABERTA (alívio) — seguro por padrão
  if (!pressaoOk) { if (!releLigado) releSet(true); valv = V_ABERTA; valvT0 = agora; valvP0 = NAN; return; }
  switch (valv) {
    case V_FECHADA:
      if (pressaoPa >= P_ABRE_PA) { valvP0 = pressaoPa; valvT0 = agora; valv = V_ABERTA; releSet(true); }
      break;
    case V_ABERTA:
      if (pressaoPa > P_MAX_PA) { valvT0 = agora; break; }                // mantém aberta
      if (pressaoPa <= P_FECHA_PA || agora - valvT0 > VALV_MAX_ABERTA_MS) { releSet(false); valv = V_ESTAB; valvT0 = agora; }
      break;
    case V_ESTAB:
      if (agora - valvT0 >= VALV_ESTAB_MS) {
        if (!isnan(valvP0)) {
          float dP = valvP0 - pressaoPa;
          float T = !isnan(tDig2) ? tDig2 : (!isnan(tAmb) ? tAmb : 25.0f);
          if (dP > 0) {
            float dV = cfg.vHead * dP / 101325.0f * 273.15f / (T + 273.15f);
            volNl += dV; valvCiclos++;
            if (ultCicloMs) vazaoNlh = dV * 3600000.0f / (agora - ultCicloMs);
            ultCicloMs = agora;
            salvarVolume(false);
          }
        }
        valv = V_FECHADA;
      }
      break;
  }
  if (ultCicloMs && agora - ultCicloMs > 6UL * 3600000UL) vazaoNlh = 0;
}
#endif

static void atuadoresTick() {
  bool seguranca = alarmesAtivos & ALARMES_SEGURANCA;
  bool mudo = millis() < silenciadoAte;
#if RELE_MODO == 1
  releSet(seguranca);
#endif
#if BUZZER_ATIVO
  // bipe 0,5 s ligado / 0,5 s desligado enquanto houver alarme de segurança não silenciado
  digitalWrite(PIN_BUZZER, (seguranca && !mudo && (millis() / 500) % 2) ? HIGH : LOW);
#endif
}

// ============================================================================
//                               MEDIDOR POR PULSOS
// ============================================================================
#if ENABLE_MEDIDOR_GAS
void IRAM_ATTR isrPulso() {
  uint32_t t = millis();
  if (t - ultimoPulsoIsrMs >= PULSO_DEBOUNCE_MS) {
    if (ultimoPulsoIsrMs) intervaloPulsoMs = t - ultimoPulsoIsrMs;
    ultimoPulsoIsrMs = t; pulsosIsr++;
  }
}
static void medidorTick() {
  noInterrupts(); uint32_t p = pulsosIsr; pulsosIsr = 0; interrupts();
  pulsosTotal += p;
  if (pulsosTotal != pulsosSalvos && millis() - ultimoSaveNvsMs > 60000UL) {
    prefs.begin("bio", false); prefs.putUInt("pulsos", pulsosTotal); prefs.end();
    pulsosSalvos = pulsosTotal; ultimoSaveNvsMs = millis();
  }
}
static float vazaoPulsosLh() {
  noInterrupts(); uint32_t iv = intervaloPulsoMs, ult = ultimoPulsoIsrMs; interrupts();
  if (!iv || millis() - ult > VAZAO_ZERO_APOS_MS) return 0.0f;
  uint32_t desde = millis() - ult, base = desde > iv ? desde : iv;
  return LITROS_POR_PULSO * 3600000.0f / base;
}
#endif

// ============================================================================
//                               ALARMES
// ============================================================================
static uint32_t avaliarCondicoes() {
  uint32_t m = 0;
  auto set = [&](Alarme a, bool c) { if (c) m |= (1u << a); };
  set(A_S_T1, isnan(tDig1));
  set(A_S_T2, isnan(tDig2));
  set(A_S_DHT, isnan(tAmb));
  set(A_T_BAIXA, !isnan(tDig1) && tDig1 < T_DIG_MIN_C);
  set(A_T_ALTA,  !isnan(tDig1) && tDig1 > T_DIG_MAX_C);
  set(A_T_TAXA,  !isnan(taxaTdig) && fabsf(taxaTdig) > T_TAXA_MAX_CH);
  set(A_ESTRAT,  !isnan(tDig1) && !isnan(tDig2) && fabsf(tDig1 - tDig2) > DT_ESTRAT_MAX_C);
  set(A_UR,      !isnan(urAmb) && urAmb > UR_MAX);
  if (aquecido()) {
    set(A_S_MQ2, isnan(mq2.rs));
    set(A_GAS_COMB, mq2.r0 > 0 && !isnan(mq2.rs) && mq2.rs / mq2.r0 < MQ2_ALARME_REL);
#if MQ7_HEATER_CYCLE
    set(A_CO, !isnan(mq7Ppm) && mq7Ppm > CO_ALARME_PPM);
#else
    set(A_S_MQ7, isnan(mq7.rs));
    set(A_MQ7, mq7.r0 > 0 && !isnan(mq7.rs) && mq7.rs / mq7.r0 < MQ7_ALARME_REL);
#endif
    set(A_MQ_NCAL, mq2.r0 <= 0);
  }
#if PRESSAO_TIPO
  set(A_S_PRESS, !pressaoOk);
  set(A_SOBREPRESSAO, pressaoOk && pressaoPa > P_MAX_PA);
#endif
  return m;
}

static String resumoLeituras() {
  char b[160];
  snprintf(b, sizeof b, "T1=%.1f T2=%.1f Tamb=%.1f UR=%.0f%% MQ2=%.2f P=%.0f Pa",
           tDig1, tDig2, tAmb, urAmb, (mq2.r0 > 0 && !isnan(mq2.rs)) ? mq2.rs / mq2.r0 : NAN, pressaoPa);
  return String(b);
}

static void publicarEvento(const String& txt);

// Persistência: sobe após 3 avaliações (15 s), limpa após 6 (30 s) — evita "piscar"
static void alarmesTick() {
  uint32_t cond = avaliarCondicoes();
  for (int a = 0; a < A_N; a++) {
    bool c = cond & (1u << a), at = alarmesAtivos & (1u << a);
    if (c != at) alCont[a]++; else alCont[a] = 0;
    uint8_t lim = c ? 3 : 6;
    if (alCont[a] >= lim) {
      alCont[a] = 0;
      if (c) alarmesAtivos |= (1u << a); else alarmesAtivos &= ~(1u << a);
      evento(c ? "alarme_on" : "alarme_off", ALARME_NOME[a]);
      if (a == A_MQ_NCAL) continue;       // informativo: não notifica
      String msg = String(c ? "ALARME: " : "Normalizado: ") + ALARME_TXT[a] + " | " + resumoLeituras();
      publicarEvento(msg);
      tgEnfileirar(msg);
    }
  }
  atuadoresTick();
}

// ============================================================================
//                               FILA EM FLASH
// ============================================================================
static uint32_t filaTamanho() {
  if (!fsOk || !LittleFS.exists(FILA_ARQ)) return 0;
  File f = LittleFS.open(FILA_ARQ, "r"); uint32_t s = f.size(); f.close(); return s;
}
static uint32_t filaLerOffset() {
  if (!LittleFS.exists(FILA_OFF)) return 0;
  File f = LittleFS.open(FILA_OFF, "r"); uint32_t o = f.parseInt(); f.close(); return o;
}
static void filaGravarOffset(uint32_t o) { File f = LittleFS.open(FILA_OFF, "w"); f.print(o); f.close(); }
static void filaEnfileirar(const String& json) {
  if (!fsOk) return;
  if (filaTamanho() + json.length() + 1 > FILA_MAX_BYTES) { Serial.println("[FILA] cheia — amostra descartada"); return; }
  File f = LittleFS.open(FILA_ARQ, "a"); if (f) { f.println(json); f.close(); }
}
static void filaDescarregar() {
  if (!fsOk || !mqtt.connected() || !LittleFS.exists(FILA_ARQ)) return;
  uint32_t off = filaLerOffset();
  File f = LittleFS.open(FILA_ARQ, "r"); if (!f) return;
  if (off >= f.size()) { f.close(); LittleFS.remove(FILA_ARQ); LittleFS.remove(FILA_OFF); Serial.println("[FILA] esvaziada"); return; }
  f.seek(off);
  for (int i = 0; i < FLUSH_POR_LOOP && f.available(); i++) {
    String l = f.readStringUntil('\n'); l.trim();
    if (l.length() && !mqtt.publish(tTelem.c_str(), l.c_str(), false)) break;
    off = f.position();
  }
  f.close(); filaGravarOffset(off);
}

// ============================================================================
//                               TELEMETRIA
// ============================================================================
static String montarJson() {
  JsonDocument doc;
  doc["id"] = DEVICE_ID; doc["fw"] = FW_VERSAO; doc["seq"] = ++seq;
  doc["ts"] = epochAgora(); doc["up"] = (millis() - bootMs) / 1000; doc["sim"] = simAtivo;

  jnum(doc["t_dig"], tDig1); jnum(doc["t_dig2"], tDig2);
  jnum(doc["t_med"], (!isnan(tDig1) && !isnan(tDig2)) ? (tDig1 + tDig2) / 2 : (!isnan(tDig1) ? tDig1 : tDig2));
  jnum(doc["dt_estrat"], (!isnan(tDig1) && !isnan(tDig2)) ? tDig1 - tDig2 : NAN);
  jnum(doc["taxa_t"], taxaTdig);
  jnum(doc["t_amb"], tAmb, 1); jnum(doc["ur_amb"], urAmb, 0);

  doc["gas_ok"] = aquecido();
  auto mqObj = [&](const char* k, SensorMQ& s, float rs) {
    JsonObject o = doc[k].to<JsonObject>();
    jnum(o["v"], s.vAo);
    if (isnan(rs)) o["rs"] = nullptr; else o["rs"] = roundf(rs);
    jnum(o["rel"], (s.r0 > 0 && !isnan(rs)) ? rs / s.r0 : NAN, 3);
    o["cond_ok"] = mqCondOk(s);
  };
  mqObj("mq2", mq2, mq2.rs);
#if MQ7_HEATER_CYCLE
  mqObj("mq7", mq7, mq7RsUltimoCiclo); jnum(doc["co_ppm"], mq7Ppm, 1);
#else
  mqObj("mq7", mq7, mq7.rs);
#endif

  if (simAtivo) {
    jnum(doc["p_pa"], simP, 0);
    doc["valv"] = simValv; doc["ciclos"] = simCiclos;
    jnum(doc["vol_nl"], simVol, 3); jnum(doc["vazao_nlh"], simVazao, 3); jnum(doc["vhead_l"], cfg.vHead, 2);
    simValv = false;
  } else {
#if PRESSAO_TIPO
    jnum(doc["p_pa"], pressaoPa, 0);
#endif
#if RELE_MODO == 2
    doc["valv"] = releLigado; doc["ciclos"] = valvCiclos;
    jnum(doc["vol_nl"], volNl, 3); jnum(doc["vazao_nlh"], vazaoNlh, 3); jnum(doc["vhead_l"], cfg.vHead, 2);
#endif
  }
#if RELE_MODO == 1
  doc["rele"] = releLigado;
#endif
#if ENABLE_MEDIDOR_GAS
  doc["pulsos"] = pulsosTotal; jnum(doc["vol_l"], pulsosTotal * LITROS_POR_PULSO, 3); jnum(doc["vazao_lh"], vazaoPulsosLh(), 3);
#endif
  doc["mudo"] = millis() < silenciadoAte;

  JsonArray al = doc["alarmes"].to<JsonArray>();
  for (int a = 0; a < A_N; a++) if (alarmesAtivos & (1u << a)) al.add(ALARME_NOME[a]);

  doc["rssi"] = WiFi.isConnected() ? WiFi.RSSI() : 0;
  doc["heap"] = ESP.getFreeHeap();
  doc["fila"] = filaTamanho();
  doc["sd"] = sdOk; doc["n_sd"] = nUltimo;
  String out; serializeJson(doc, out); return out;
}

// ---------------------------------------------------------------------------
// Modelo de simulação (valores plausíveis para biodigestor pequeno sem aquecimento):
//  - ambiente: ciclo diário (mín. ~6 h, máx. ~15 h), UR inversa à temperatura
//  - digestato: ciclo amortecido e atrasado; topo ~1 °C mais frio (estratificação leve)
//  - biogás: produção 0,7–2,3 NL/h (maior à tarde) pressurizando o headspace;
//    ao atingir P_ABRE_PA a "válvula" abre e o volume é contabilizado como no método real
//  - MQ: ar limpo com ruído; comando simular_vazamento força 3 min de vazamento
// ---------------------------------------------------------------------------
static float gauss() { return (random(-1000, 1000) + random(-1000, 1000)) / 2000.0f; }

static void simular() {
  uint32_t agora = millis();
  float dt = simUlt ? (agora - simUlt) / 1000.0f : 1.0f; simUlt = agora;
  time_t t = time(nullptr); struct tm lt; localtime_r(&t, &lt);
  float h = (t > 1700000000) ? lt.tm_hour + lt.tm_min / 60.0f + lt.tm_sec / 3600.0f : fmodf(agora / 3600000.0f, 24.0f);
  const float W = 2.0f * PI / 24.0f;
  tAmb  = 22.0f + 6.0f * sinf(W * (h - 9.0f)) + 0.15f * gauss();
  urAmb = constrain(70.0f - 2.8f * (tAmb - 22.0f) + 1.5f * gauss(), 30.0f, 95.0f);
  tDig1 = 28.0f + 1.5f * sinf(W * (h - 13.0f)) + 0.03f * gauss();
  tDig2 = tDig1 - (0.9f + 0.4f * sinf(W * (h - 16.0f))) + 0.03f * gauss();
  taxaTdig = 1.5f * W * cosf(W * (h - 13.0f));

  auto mqSim = [&](SensorMQ& m, float r0, float fator) {
    if (m.r0 <= 0) m.r0 = r0;
    m.rs = m.r0 * fator * (1.0f + 0.02f * gauss());
    const float rDiv = DIV_R_TOP + DIV_R_BOT, rlEf = m.rl * rDiv / (m.rl + rDiv);
    m.vAo = MQ_VC * rlEf / (m.rs + rlEf); m.valido = true;
  };
  bool vaz = agora < simVazamentoAte;
  mqSim(mq2, 9000.0f, vaz ? 0.35f : 1.0f);
  mqSim(mq7, 7300.0f, vaz ? 0.80f : 1.0f);

  simVazao = 1.5f + 0.8f * sinf(W * (h - 15.0f));                        // NL/h
  float Tk = tDig2 + 273.15f;
  simP += simVazao / 3600.0f * dt * 101325.0f / cfg.vHead * Tk / 273.15f;  // inverso de V_N = V·ΔP/P0·T0/T
  if (simP >= P_ABRE_PA) {
    float dP = simP - P_FECHA_PA;
    simVol += cfg.vHead * dP / 101325.0f * 273.15f / Tk;
    simCiclos++; simP = P_FECHA_PA; simValv = true; simUltCicloMs = agora;
  }
  simP += 3.0f * gauss();
}

static void simularSet(bool on) {
  simAtivo = on;
  prefs.begin("bio", false); prefs.putBool("sim", on); prefs.end();
  if (!on) {                     // volta ao real: descarta valores sintéticos e recarrega calibração
    tDig1 = tDig2 = tAmb = urAmb = taxaTdig = NAN; histN = 0;
    carregarCalibracao();
  } else { simP = P_FECHA_PA; simUlt = 0; }
}


// ============================================================================
//            HISTÓRICO LOCAL (médias de 5 min, 48 h, persistido em flash)
// ============================================================================
// Permite que o painel mostre o período anterior ao momento em que foi aberto,
// sem servidor. Pedido pelo painel com "historico:<horas>" (sem token, só leitura).
#define HIST_INT_S   300
#define HIST_N       576          // 576 x 5 min = 48 h
#define HIST_ARQ     "/hist.bin"
#define HNULL        INT16_MIN

struct __attribute__((packed)) HistPt {
  uint32_t ts; int16_t t1, t2, ta; uint8_t ur; uint16_t r2, r7; int16_t p; float vol; uint32_t al; uint8_t sim;
};
HistPt hist[HIST_N];
uint16_t histHead = 0, histCnt = 0, histDesdeSalvo = 0;
uint32_t histBalde = 0;                      // balde de 5 min atual (epoch / 300)
struct { double s[8]; uint16_t n[8]; uint32_t al; uint8_t sim; } acc;

static void histCarregar() {
  if (!fsOk || !LittleFS.exists(HIST_ARQ)) return;
  File f = LittleFS.open(HIST_ARQ, "r");
  if (f.size() == sizeof(hist) + 4) { f.read((uint8_t*)&histHead, 2); f.read((uint8_t*)&histCnt, 2); f.read((uint8_t*)hist, sizeof(hist)); }
  f.close();
  if (histHead >= HIST_N || histCnt > HIST_N) { histHead = histCnt = 0; }
  Serial.printf("[HIST] %u pontos carregados da flash\n", histCnt);
}

static void histSalvar() {
  if (!fsOk) return;
  File f = LittleFS.open(HIST_ARQ, "w");
  if (f) { f.write((uint8_t*)&histHead, 2); f.write((uint8_t*)&histCnt, 2); f.write((uint8_t*)hist, sizeof(hist)); f.close(); }
  histDesdeSalvo = 0;
}

static void accAdd(int i, float v) { if (!isnan(v)) { acc.s[i] += v; acc.n[i]++; } }
static float accMed(int i) { return acc.n[i] ? acc.s[i] / acc.n[i] : NAN; }
static int16_t q(float v, float esc) { return isnan(v) ? HNULL : (int16_t)constrain(lroundf(v * esc), -32767, 32767); }

// Chamado a cada publicação (30 s): acumula e fecha a média ao virar o balde de 5 min
static void histAmostrar() {
  uint32_t ts = epochAgora(); if (!ts) return;
  uint32_t balde = ts / HIST_INT_S;
  if (histBalde && balde != histBalde && acc.n[0] + acc.n[2] + acc.n[4] + acc.n[6] > 0) {
    HistPt& h = hist[histHead];
    h.ts = histBalde * HIST_INT_S + HIST_INT_S / 2;
    h.t1 = q(accMed(0), 100); h.t2 = q(accMed(1), 100); h.ta = q(accMed(2), 100);
    float ur = accMed(3); h.ur = isnan(ur) ? 255 : (uint8_t)lroundf(ur);
    float r2 = accMed(4), r7 = accMed(5);
    h.r2 = isnan(r2) ? 65535 : (uint16_t)constrain(lroundf(r2 * 1000), 0, 65534);
    h.r7 = isnan(r7) ? 65535 : (uint16_t)constrain(lroundf(r7 * 1000), 0, 65534);
    h.p = q(accMed(6), 1); h.vol = accMed(7); h.al = acc.al; h.sim = acc.sim;
    histHead = (histHead + 1) % HIST_N; if (histCnt < HIST_N) histCnt++;
    memset(&acc, 0, sizeof(acc));
    if (++histDesdeSalvo >= 6) histSalvar();          // grava a cada 30 min
  }
  histBalde = balde;
  accAdd(0, tDig1); accAdd(1, tDig2); accAdd(2, tAmb); accAdd(3, urAmb);
  accAdd(4, (mq2.r0 > 0 && !isnan(mq2.rs) && aquecido()) ? mq2.rs / mq2.r0 : NAN);
  accAdd(5, (mq7.r0 > 0 && !isnan(mq7.rs) && aquecido()) ? mq7.rs / mq7.r0 : NAN);
  accAdd(6, simAtivo ? simP : pressaoPa);
  accAdd(7, simAtivo ? simVol : ((RELE_MODO == 2) ? volNl : NAN));
  acc.al |= alarmesAtivos; acc.sim |= simAtivo;
}

// Envio em partes (não bloqueia o loop): 30 linhas por mensagem
int32_t histEnvIdx = -1; uint16_t histEnvFim = 0, histEnvParte = 0, histEnvTotal = 0;
static String histPedir(int horas) {
  horas = constrain(horas, 1, 48);
  uint16_t n = min<uint16_t>(histCnt, horas * 3600 / HIST_INT_S);
  if (!n) { histEnvIdx = -1; if (mqtt.connected()) mqtt.publish((cfg.topic + "/historico").c_str(), "{\"parte\":0,\"total\":0,\"d\":[]}"); return "histórico vazio"; }
  histEnvIdx = 0; histEnvFim = n; histEnvParte = 0; histEnvTotal = (n + 29) / 30;
  return "enviando " + String(n) + " pontos de 5 min (" + String(histEnvTotal) + " partes)";
}

static void histTick() {
  if (histEnvIdx < 0 || !mqtt.connected()) return;
  static uint32_t t = 0; if (millis() - t < 150) return; t = millis();
  JsonDocument doc;
  doc["parte"] = ++histEnvParte; doc["total"] = histEnvTotal; doc["int"] = HIST_INT_S;
  JsonArray c = doc["c"].to<JsonArray>();
  for (const char* k : {"ts", "t_dig", "t_dig2", "t_amb", "ur_amb", "mq2_rel", "mq7_rel", "p_pa", "vol_nl", "n_alarmes", "sim"}) c.add(k);
  JsonArray d = doc["d"].to<JsonArray>();
  uint16_t n = histEnvFim;
  for (int k = 0; k < 30 && histEnvIdx < n; k++, histEnvIdx++) {
    const HistPt& h = hist[(histHead + HIST_N - n + histEnvIdx) % HIST_N];
    JsonArray r = d.add<JsonArray>();
    r.add(h.ts);
    auto addq = [&](int16_t v, float esc) { if (v == HNULL) r.add(nullptr); else r.add(v / esc); };
    addq(h.t1, 100); addq(h.t2, 100); addq(h.ta, 100);
    if (h.ur == 255) r.add(nullptr); else r.add(h.ur);
    if (h.r2 == 65535) r.add(nullptr); else r.add(h.r2 / 1000.0f);
    if (h.r7 == 65535) r.add(nullptr); else r.add(h.r7 / 1000.0f);
    addq(h.p, 1);
    if (isnan(h.vol)) r.add(nullptr); else r.add(roundf(h.vol * 1000) / 1000);
    r.add(__builtin_popcount(h.al & ~(1u << A_MQ_NCAL))); r.add(h.sim);
  }
  String out; serializeJson(doc, out);
  mqtt.publish((cfg.topic + "/historico").c_str(), out.c_str(), false);
  if (histEnvIdx >= n) histEnvIdx = -1;
}

// ============================================================================
//                               REDE / MQTT / OTA
// ============================================================================
static void publicarEvento(const String& txt) {
  Serial.println("[EVT] " + txt);
  if (mqtt.connected()) mqtt.publish(tEvento.c_str(), txt.c_str(), false);
}

static void montarTopicos() {
  tTelem = cfg.topic + "/telemetria"; tStatus = cfg.topic + "/status";
  tCmd = cfg.topic + "/cmd"; tEvento = cfg.topic + "/evento";
  tDiario = cfg.topic + "/diario"; tDiarioHist = cfg.topic + "/diario/hist";
}

static void mqttAplicarCfg() {
  if (cfg.tls) { netTls.setInsecure(); mqtt.setClient(netTls); } else mqtt.setClient(netPlain);
  mqtt.setServer(cfg.host.c_str(), cfg.port);
  montarTopicos();
  if (mqtt.connected()) mqtt.disconnect();
}

extern uint32_t tUltHttp;
static void webParar(); static void webIniciar(); static void portalAbrir(const char* origem); static void ntpSincronizou(struct timeval* tv);
static String apNome() { char b[24]; snprintf(b, sizeof b, "Biodigestor-%04X", (uint16_t)(ESP.getEfuseMac() >> 32)); return String(b); }

// Parâmetros extras do portal (MQTT/Telegram) — gravados ao salvar
WiFiManagerParameter *pHost, *pPort, *pTopic, *pUser, *pPass, *pToken, *pTgTok, *pTgChat;

static void portalSalvarParams() {
  // campos vazios NÃO apagam a configuração (a página de Wi-Fi pode salvar sem enviar estes campos)
  if (strlen(pHost->getValue()))  cfgSet("host",  pHost->getValue());
  if (atoi(pPort->getValue()) > 0) cfgSet("port", pPort->getValue());
  if (strlen(pTopic->getValue())) cfgSet("topic", pTopic->getValue());
  if (strlen(pUser->getValue()))  cfgSet("user",  pUser->getValue());
  if (strlen(pPass->getValue()))  cfgSet("pass",  pPass->getValue());
  if (strlen(pToken->getValue())) cfgSet("token", pToken->getValue());
  if (strlen(pTgTok->getValue())) cfgSet("tgtok", pTgTok->getValue());
  if (strlen(pTgChat->getValue())) cfgSet("tgchat", pTgChat->getValue());
  mqttAplicarCfg();
  Serial.println("[PORTAL] configuração salva:\n" + cfgTexto());
}

static void portalPreparar() {
  static char sPort[8]; snprintf(sPort, sizeof sPort, "%u", cfg.port);
  pHost   = new WiFiManagerParameter("host", "Broker MQTT", cfg.host.c_str(), 64);
  pPort   = new WiFiManagerParameter("port", "Porta (1883 / 8883 TLS)", sPort, 6);
  pTopic  = new WiFiManagerParameter("topic", "Tópico base", cfg.topic.c_str(), 96);
  pUser   = new WiFiManagerParameter("user", "Usuário MQTT (opcional)", cfg.user.c_str(), 48);
  pPass   = new WiFiManagerParameter("pass", "Senha MQTT (vazio = manter)", "", 64);
  pToken  = new WiFiManagerParameter("token", "Token de comando (vazio = manter)", "", 48);
  pTgTok  = new WiFiManagerParameter("tgtok", "Telegram: token do bot (vazio = manter)", "", 64);
  pTgChat = new WiFiManagerParameter("tgchat", "Telegram: chat_id", cfg.tgChat.c_str(), 24);
  for (auto p : {pHost, pPort, pTopic, pUser, pPass, pToken, pTgTok, pTgChat}) wm.addParameter(p);
  wm.setSaveParamsCallback(portalSalvarParams);
  wm.setSaveConfigCallback(portalSalvarParams);
  wm.setTitle("Monitor de Biodigestor");
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  wm.setWebPortalClientCheck(false);              // o celular conectado (checagem de internet do Android) mantinha o portal aberto
  wm.setAPClientCheck(false);                     //   para sempre e a API de coleta nunca voltava: agora fecha em PORTAL_TIMEOUT_S
  wm.setConnectTimeout(12);                        // salvar no portal bloqueia ~2+12+1 s: abaixo do watchdog (30 s)
  wm.setPreSaveConfigCallback([]() { esp_task_wdt_reset(); });
  wm.setHostname(DEVICE_ID);
  wm.setShowInfoUpdate(false);
}

static void portalAbrir(const char* origem) {
  if (portalAtivo) return;
  Serial.printf("[PORTAL] aberto por: %s\n", origem);
  webParar();                         // o portal usa a porta 80
  Serial.printf("[PORTAL] AP \"%s\" senha \"%s\" -> http://192.168.4.1\n", apNome().c_str(), PORTAL_SENHA);
  wm.startConfigPortal(apNome().c_str(), cfg.apPass.c_str());
  portalAtivo = true;
  evento("portal", "portal de Wi-Fi aberto (" + String(origem) + ") por " + String(PORTAL_TIMEOUT_S / 60) + " min (coleta indisponível)");
}

static bool wifiTemRedeSalva() {
  wifi_config_t c; if (esp_wifi_get_config(WIFI_IF_STA, &c) != ESP_OK) return false;
  return c.sta.ssid[0] != 0;
}
static void apIniciar() {
  WiFi.softAP(apNome().c_str(), cfg.apPass.c_str());
  Serial.printf("[AP] \"%s\" ativo em http://%s (coleta offline)\n", apNome().c_str(), WiFi.softAPIP().toString().c_str());
}
static void wifiIniciar() {
  WiFi.persistent(true);
  WiFi.mode(WIFI_AP_STA);            // AP sempre ligado (coleta) + estação para a internet, quando houver
  WiFi.setAutoReconnect(false);      // reconexão controlada: tentar sem parar derruba quem está no AP
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {   // diagnóstico: por que a rede caiu
    uint8_t r = info.wifi_sta_disconnected.reason;
    Serial.printf("[WiFi] desconectado de \"%.32s\": motivo %u (%s), RSSI %d dBm\n",
                  (const char*)info.wifi_sta_disconnected.ssid, r, WiFi.disconnectReasonName((wifi_err_reason_t)r),
                  info.wifi_sta_disconnected.rssi);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    Serial.printf("[WiFi] associado ao AP, canal %u\n", info.wifi_sta_connected.channel);
  }, ARDUINO_EVENT_WIFI_STA_CONNECTED);
  apIniciar();
#if WIFI_ENTERPRISE
  WiFi.begin(WIFI_EAP_SSID, WPA2_AUTH_PEAP, WIFI_EAP_IDENTITY, WIFI_EAP_USERNAME, WIFI_EAP_PASSWORD);
  Serial.println("[WiFi] Enterprise: " WIFI_EAP_SSID);
#else
  portalPreparar();
  if (wifiTemRedeSalva()) { WiFi.begin(); Serial.println("[WiFi] tentando a rede salva…"); }
  else Serial.println("[WiFi] nenhuma rede salva — segure o BOOT 3 s (ou /portal) para configurar");
#endif
  tUltWifiTry = millis();
}

static void otaIniciar() {
  if (otaIniciado) return;
  ArduinoOTA.setHostname(DEVICE_ID);
  ArduinoOTA.setMdnsEnabled(false);   // o mDNS já foi iniciado como biodigestor.local (coleta.h)
  ArduinoOTA.setPassword(cfg.token.c_str());
  ArduinoOTA.onStart([]() { publicarEvento("OTA local iniciado"); });
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { esp_task_wdt_reset(); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] erro %u\n", e); });
  ArduinoOTA.begin();
  otaIniciado = true;
  Serial.println("[OTA] pronto (rede local, senha = token)");
}

static String otaPorUrl(const String& url) {
  publicarEvento("OTA remoto: baixando " + url);
  mqtt.loop(); delay(200);
  salvarVolume(true);
  histSalvar();
  WiFiClientSecure sc; sc.setInsecure();
  WiFiClient pc;
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(true);
  httpUpdate.onProgress([](int, int) { esp_task_wdt_reset(); });
  t_httpUpdate_return r = url.startsWith("https") ? httpUpdate.update(sc, url) : httpUpdate.update(pc, url);
  if (r == HTTP_UPDATE_FAILED) return "OTA falhou: " + httpUpdate.getLastErrorString();
  return "OTA: nenhuma atualização";
}

static String statusTexto() {
  char b[300];
  snprintf(b, sizeof b,
    "fw=%s up=%lus wifi=%s(%s) rssi=%d mqtt=%s fila=%luB | T1=%.2f T2=%.2f Tamb=%.1f UR=%.0f | MQ2 Rs=%.0f R0=%.0f | MQ7 Rs=%.0f R0=%.0f | P=%.0f Pa | rele=%d vol=%.3f NL",
    FW_VERSAO, (unsigned long)((millis() - bootMs) / 1000), WiFi.isConnected() ? "ok" : "off", WiFi.SSID().c_str(),
    WiFi.isConnected() ? WiFi.RSSI() : 0, mqtt.connected() ? "ok" : "off", (unsigned long)filaTamanho(),
    tDig1, tDig2, tAmb, urAmb, mq2.rs, mq2.r0, mq7.rs, mq7.r0, pressaoPa, releLigado, volNl);
  return String(b);
}

// Formato: "comando[:arg]:TOKEN" (token = último campo). ping/status dispensam token.
static String tratarComando(String c, bool origemSerial) {
  c.trim();
  if (c == "ping") return "pong";
  if (c == "status") return statusTexto();
  if (c.startsWith("historico")) { int i = c.indexOf(':'); return histPedir(i > 0 ? c.substring(i + 1).toInt() : 24); }
  int ult = c.lastIndexOf(':');
  String tok = ult > 0 ? c.substring(ult + 1) : "";
  String corpo = ult > 0 ? c.substring(0, ult) : c;
  if (!origemSerial && tok != cfg.token) return "comando inválido ou token errado";
  if (origemSerial && tok != cfg.token) corpo = c;           // no Serial o token é opcional
  int sep = corpo.indexOf(':');
  String cmd = sep > 0 ? corpo.substring(0, sep) : corpo;
  String arg = sep > 0 ? corpo.substring(sep + 1) : "";

  if (cmd == "calibrar")      return calibrarMQ(arg == "forcar");
  if (cmd == "zerar_pressao") return pressaoZerar();
  if (cmd == "cal_pressao")   return pressaoCal(arg.toFloat());
  if (cmd == "vhead")         { if (arg.toFloat() <= 0) return "erro: vhead:<litros>"; cfgSet("vhead", arg); return "V_head = " + String(cfg.vHead, 2) + " L"; }
  if (cmd == "zerar_volume")  {
    pulsosTotal = pulsosSalvos = 0; volNl = 0; valvCiclos = 0;
    prefs.begin("bio", false); prefs.putUInt("pulsos", 0); prefs.end(); salvarVolume(true);
    return "volume zerado";
  }
  if (cmd == "silenciar")     { silenciadoAte = millis() + SILENCIAR_MIN * 60000UL; return "alarme sonoro silenciado por " + String(SILENCIAR_MIN) + " min"; }
  if (cmd == "rele_teste")    {
#if RELE_MODO
    releSet(true); delay(1500); releSet(false); return "relé acionado por 1,5 s";
#else
    return "relé desativado (RELE_MODO 0)";
#endif
  }
  if (cmd == "telegram_teste") { tgEnfileirar("Teste de alerta — " + resumoLeituras()); return cfg.tgToken.length() ? "mensagem de teste enfileirada" : "Telegram não configurado"; }
  if (cmd == "set") {
    int eq = arg.indexOf('=');
    if (eq < 1) return "uso: set:<chave>=<valor>";
    String k = arg.substring(0, eq), v = arg.substring(eq + 1);
    if (!cfgSet(k, v)) return "chave desconhecida";
    if (k == "host" || k == "port" || k == "tls" || k == "user" || k == "pass" || k == "topic") mqttAplicarCfg();
    return "ok: " + k + " gravado";
  }
  if (cmd == "simular")       { simularSet(arg != "0"); return simAtivo ? "SIMULAÇÃO ATIVADA (dados sintéticos, campo sim=true)" : "simulação desativada — leituras reais"; }
  if (cmd == "simular_vazamento") { if (!simAtivo) return "ative a simulação antes (simular:1)"; simVazamentoAte = millis() + 180000UL; return "vazamento simulado por 3 min"; }
  if (cmd == "portal")        { portalAbrir(origemSerial ? "comando serial" : "comando MQTT"); return "portal aberto: " + apNome(); }
  if (cmd == "portal_fechar") { if (!portalAtivo) return "portal já está fechado"; wm.stopConfigPortal(); return "fechando o portal"; }
  if (cmd == "sd")            return sdStatus();
  if (cmd == "diario")        return diarioResumo();
  if (cmd == "ota")           return otaPorUrl(arg);
  if (cmd == "reiniciar")     { publicarEvento("reiniciando"); salvarVolume(true); histSalvar(); delay(300); ESP.restart(); return ""; }
  return "comando desconhecido";
}


// ============================================================================
//                     DIÁRIO DE OPERAÇÃO (alimentação, pH, CO2…)
// O painel publica em TOPIC/diario; o ESP32 é o dono do registro:
//   flash (/diario.jsonl, sempre) + microSD (/diario.csv) + evento "diario" (vai p/ nuvem pelo app)
//   e republica as últimas entradas, RETIDAS, em TOPIC/diario/hist (painel recarrega após F5).
// ============================================================================
#define DIARIO_ARQ       "/diario.jsonl"
#define DIARIO_MAX_B     48000UL     // ~250 entradas; acima disso guarda a metade mais recente
#define DIARIO_HIST_B    6000U       // tamanho máx. da mensagem retida (buffer MQTT = 8 kB)
static void sdDiarioAppend(const JsonDocument& e);   // coleta.h

static bool diarioExiste(uint32_t ts, const char* tipo) {
  File f = LittleFS.open(DIARIO_ARQ, "r"); if (!f) return false;
  char pref[48]; snprintf(pref, sizeof pref, "{\"ts\":%lu,\"tipo\":\"%s\"", (unsigned long)ts, tipo);
  bool achou = false;
  while (f.available() && !achou) achou = f.readStringUntil('\n').startsWith(pref);
  f.close(); return achou;
}

static void diarioCompactar() {
  File f = LittleFS.open(DIARIO_ARQ, "r"); if (!f) return;
  size_t tam = f.size(); if (tam <= DIARIO_MAX_B) { f.close(); return; }
  f.seek(tam - DIARIO_MAX_B / 2); f.readStringUntil('\n');            // alinha no início de uma linha
  File g = LittleFS.open("/diario.tmp", "w");
  if (g) { while (f.available()) { String l = f.readStringUntil('\n'); l.trim(); if (l.length()) g.println(l); } g.close(); }
  f.close();
  if (g) { LittleFS.remove(DIARIO_ARQ); LittleFS.rename("/diario.tmp", DIARIO_ARQ); }
}

// Entradas mais recentes primeiro, até maxB bytes, como array JSON.
static String diarioUltimos(size_t maxB, uint16_t* nTotal = nullptr) {
  const int N = 40; String ring[N]; int cnt = 0;
  File f = LittleFS.open(DIARIO_ARQ, "r");
  if (f) { while (f.available()) { String l = f.readStringUntil('\n'); l.trim(); if (l.length()) ring[cnt++ % N] = l; } f.close(); }
  if (nTotal) *nTotal = cnt;
  String out = "["; int k = cnt < N ? cnt : N;
  for (int i = cnt - 1; i >= cnt - k; i--) {
    const String& l = ring[i % N];
    if (out.length() + l.length() + 2 > maxB) break;
    if (out.length() > 1) out += ','; out += l;
  }
  return out + "]";
}

static void diarioPublicarHist() {
  if (!mqtt.connected()) return;
  String h = diarioUltimos(DIARIO_HIST_B);
  mqtt.publish(tDiarioHist.c_str(), (const uint8_t*)h.c_str(), h.length(), true);
}

static void diarioReceber(const byte* p, unsigned int len) {
  if (len > 1500) return;
  JsonDocument in;
  if (deserializeJson(in, p, len)) { Serial.println("[DIARIO] JSON inválido"); return; }
  uint32_t ts = in["ts"] | 0UL;
  const char* tipo = in["tipo"] | "";
  size_t nt = strlen(tipo);
  if (ts < 1700000000UL || ts > 4000000000UL || nt == 0 || nt > 16) { Serial.println("[DIARIO] entrada inválida"); return; }
  for (size_t i = 0; i < nt; i++) if (!isalnum((unsigned char)tipo[i]) && tipo[i] != '_') return;
  if (diarioExiste(ts, tipo)) { diarioPublicarHist(); return; }     // repetição (reenvio do painel): só confirma

  JsonDocument e;
  e["ts"] = ts; e["tipo"] = tipo;
  auto txt = [&](const char* k, size_t mx) {
    const char* v = in[k] | ""; if (!*v) return;
    String s(v); s.replace("\n", " "); s.replace("\r", " "); if (s.length() > mx) s = s.substring(0, mx); e[k] = s;
  };
  auto num = [&](const char* k) { if (in[k].is<double>()) e[k] = in[k].as<double>(); };
  if (in["valor"].is<double>()) num("valor"); else txt("valor", 40);
  txt("unid", 10); num("co2_pct"); num("vi_ml"); num("vf_ml"); txt("autor", 40); txt("obs", 200);
  e["rx"] = (uint32_t)time(nullptr);

  String l; serializeJson(e, l);
  File f = LittleFS.open(DIARIO_ARQ, "a"); if (f) { f.println(l); f.close(); }
  diarioCompactar();
  sdDiarioAppend(e);
  String v = e["valor"].isNull() ? String("") : (e["valor"].is<const char*>() ? String(e["valor"].as<const char*>()) : String(e["valor"].as<double>(), 2));
  evento("diario", String(tipo) + " " + v + " " + (const char*)(e["unid"] | "") + " | " + (const char*)(e["autor"] | "") + " | " + (const char*)(e["obs"] | ""));
  diarioPublicarHist();
}

static String diarioResumo() {
  uint16_t n = 0; String u = diarioUltimos(1500, &n);
  return "diário: " + String(n) + " entradas na flash. Mais recentes: " + u;
}

static void mqttCallback(char* topic, byte* payload, unsigned int len) {
  if (tDiario == topic) { diarioReceber(payload, len); return; }
  String s; s.reserve(len); for (unsigned int i = 0; i < len; i++) s += (char)payload[i];
  publicarEvento(tratarComando(s, false));
}

static void redeTick() {
#if !WIFI_ENTERPRISE
  if (portalAtivo) {
    wm.process();
    if (!wm.getConfigPortalActive()) {
      portalAtivo = false;
      WiFi.mode(WIFI_AP_STA); apIniciar(); webIniciar();   // volta ao modo de coleta
      evento("portal", "portal fechado; API de coleta ativa");
      if (wifiTemRedeSalva() && !WiFi.isConnected()) WiFi.begin();
    }
  }
#endif
  static bool estavaConectado = false;
  if (WiFi.isConnected() != estavaConectado) {
    estavaConectado = WiFi.isConnected();
    evento(estavaConectado ? "wifi_on" : "wifi_off", estavaConectado ? WiFi.SSID() : String(""));
  }
  if (!WiFi.isConnected()) {
    // só tenta de novo se ninguém estiver conectado ao AP (a busca de canal derruba o coletor)
    // com alguém no AP, só tenta a cada 30 min e se não houver coleta em andamento (2 min sem HTTP)
    bool apOcupado = WiFi.softAPgetStationNum() > 0;
    uint32_t intervalo = apOcupado ? 1800000UL : WIFI_RETRY_MS;
    if (!portalAtivo && wifiTemRedeSalva() && millis() - tUltWifiTry > intervalo && (!apOcupado || millis() - tUltHttp > 120000UL)) {
      tUltWifiTry = millis(); WiFi.begin();
    }
    return;
  }
  if (!ntpIniciado) {
    sntp_set_time_sync_notification_cb(ntpSincronizou);
    configTzTime(TZ_INFO, NTP_SERVER1, NTP_SERVER2); ntpIniciado = true;
    Serial.printf("[WiFi] \"%s\" IP %s RSSI %d dBm\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  }
  otaIniciar();
  ArduinoOTA.handle();
  tgTick();
  if (mqtt.connected()) { mqtt.loop(); return; }
  if (millis() - tUltMqttTry < 5000UL) return;
  tUltMqttTry = millis();
  String cid = String(DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  bool ok = cfg.user.length()
    ? mqtt.connect(cid.c_str(), cfg.user.c_str(), cfg.pass.c_str(), tStatus.c_str(), 1, true, "offline")
    : mqtt.connect(cid.c_str(), tStatus.c_str(), 1, true, "offline");
  if (ok) {
    mqtt.publish(tStatus.c_str(), "online", true);
    mqtt.subscribe(tCmd.c_str(), 1);
    mqtt.subscribe(tDiario.c_str(), 1);
    diarioPublicarHist();
    Serial.printf("[MQTT] conectado a %s:%u\n", cfg.host.c_str(), cfg.port);
    static bool primeira = true;
    if (primeira) {
      primeira = false;
      String m = "Sistema iniciado (fw " FW_VERSAO ", reset=" + String((int)esp_reset_reason()) + ")";
      publicarEvento(m); tgEnfileirar(m);
    }
  } else Serial.printf("[MQTT] falha rc=%d\n", mqtt.state());
}

// ============================================================================
//                               SERIAL / BOTÃO
// ============================================================================
static void serialTick() {
  if (!Serial.available()) return;
  String c = Serial.readStringUntil('\n'); c.trim();
  if (!c.length()) return;
  if (c == "ajuda" || c == "help") {
    Serial.println(
      "status | cfg | enderecos | fila | cal (MQ em ar limpo, >=30 min ligado) | cal_forcar | zerar_pressao | cal_pressao:<Pa>\n"
      "vhead:<litros> | zerar_volume | silenciar | rele_teste | telegram_teste | portal | reiniciar\n"
      "simular:1 | simular:0 | simular_vazamento | diario | sd\n"
      "wifi <ssid> <senha> | set:<chave>=<valor>  (chaves: host port tls user pass topic token tgtok tgchat vhead)\n"
      "ota:<url-do-.bin>");
  }
  else if (c == "cfg")       Serial.println(cfgTexto());
  else if (c == "enderecos") dsInit();
  else if (c == "fila")      Serial.printf("fila=%lu bytes, offset=%lu\n", (unsigned long)filaTamanho(), (unsigned long)filaLerOffset());
  else if (c == "cal")       Serial.println(calibrarMQ());
  else if (c == "cal_forcar") Serial.println(calibrarMQ(true));
  else if (c.startsWith("wifi ")) {
    String r = c.substring(5); int sp = r.indexOf(' ');
    String ssid = sp > 0 ? r.substring(0, sp) : r, senha = sp > 0 ? r.substring(sp + 1) : "";
    WiFi.persistent(true); WiFi.begin(ssid.c_str(), senha.c_str());
    Serial.println("conectando a \"" + ssid + "\" (credenciais salvas)");
  }
  else Serial.println(tratarComando(c, true));
}

static void botaoTick() {
  static uint32_t t0 = 0, tSolto = 0; static bool preso = false, longo = false, armado = false;
  bool p = digitalRead(PIN_BOTAO) == LOW;
  // GPIO0 (BOOT) também é ligado ao DTR/RTS do conversor USB-serial: com um console serial aberto ele pode
  // ficar em nível baixo o tempo todo e abria o portal 3 s após o boot. Só aceita o botão depois de vê-lo
  // solto por 1 s seguido, e qualquer "pressionado" que já vinha do boot é ignorado.
  if (!armado) {
    if (p) { tSolto = 0; return; }
    if (!tSolto) tSolto = millis();
    if (millis() - tSolto < 1000) return;
    armado = true; Serial.println("[BOTAO] BOOT liberado (curto = silenciar, 3 s = portal)");
  }
  if (p && !preso) { preso = true; longo = false; t0 = millis(); }
  if (p && preso && !longo && millis() - t0 > 3000) { longo = true; portalAbrir("botão BOOT 3 s"); }
  if (!p && preso) {
    preso = false;
    if (!longo && millis() - t0 > 50) { silenciadoAte = millis() + SILENCIAR_MIN * 60000UL; publicarEvento("alarme silenciado pelo botão"); }
  }
}

#include "coleta.h"

// ============================================================================
//                               SETUP / LOOP
// ============================================================================
void setup() {
  Serial.begin(115200); delay(300);
  bootMs = millis();
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BOTAO, INPUT_PULLUP);
#if RELE_MODO
  releSet(false);
#endif
#if BUZZER_ATIVO
  pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW);
#endif
  Serial.println("\n=== Monitor de Biodigestor " DEVICE_ID " — fw " FW_VERSAO " ===  (digite 'ajuda')");
  Serial.printf("Motivo do reset: %d\n", (int)esp_reset_reason());

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_MQ2_AO, ADC_11db);
  analogSetPinAttenuation(PIN_MQ7_AO, ADC_11db);
#if PRESSAO_TIPO == 1
  analogSetPinAttenuation(PIN_PRESSAO_ANALOG, ADC_11db);
#elif PRESSAO_TIPO == 2
  pinMode(PIN_HX_DOUT, INPUT); pinMode(PIN_HX_SCK, OUTPUT); digitalWrite(PIN_HX_SCK, LOW);
#endif

  setenv("TZ", TZ_INFO, 1); tzset();
  bootContar();
  fsOk = LittleFS.begin(true);
  histCarregar();
  Serial.printf("[FS] LittleFS %s, fila: %lu bytes\n", fsOk ? "ok" : "FALHOU", (unsigned long)filaTamanho());
  cfgCarregar();
  carregarCalibracao();
  Serial.println(cfgTexto());

  dsInit();
  dht.setup(PIN_DHT, DHT_MODELO == 22 ? DHTesp::DHT22 : DHTesp::DHT11);
#if MQ7_HEATER_CYCLE
  mq7HeaterInit();
#endif
#if ENABLE_MEDIDOR_GAS
  pinMode(PIN_PULSO_GAS, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_PULSO_GAS), isrPulso, FALLING);
#endif

  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(8192);
  mqtt.setKeepAlive(60);
  mqtt.setSocketTimeout(5);
  mqttAplicarCfg();
  sdIniciar();
  evento("boot", "fw " FW_VERSAO " reset=" + String((int)esp_reset_reason()));
  wifiIniciar();      // não bloqueia: AP de coleta sempre ativo
  webIniciar();
  wdtInit();          // watchdog só depois, para não disparar durante a conexão inicial
}

void loop() {
  esp_task_wdt_reset();
  uint32_t agora = millis();
  serialTick();
  botaoTick();
  redeTick();

  if (simAtivo) {
    if (agora - tUltGas >= GAS_SAMPLE_MS) { tUltGas = agora; simular(); }
  } else {
  dsTick();
  dhtTick();
  pressaoTick();
  if (agora - tUltGas >= GAS_SAMPLE_MS) {
    tUltGas = agora; lerMQ(mq2);
  #if !MQ7_HEATER_CYCLE
    lerMQ(mq7);
  #endif
  }
  #if MQ7_HEATER_CYCLE
  mq7HeaterTick();
  #endif
  #if ENABLE_MEDIDOR_GAS
  medidorTick();
  #endif
#if RELE_MODO == 2
  valvulaTick();
#endif
  }

  if (agora - tUltAlarme >= ALARME_EVAL_MS) { tUltAlarme = agora; alarmesTick(); }
#if BUZZER_ATIVO
  atuadoresTick();   // mantém o padrão de bipe
#endif

  // 1ª publicação espera o relógio (NTP) por até 20 s, para não gerar amostra com ts=0
  bool relogioOk = epochAgora() || (agora - bootMs > 20000UL);
  if ((agora - tUltPub >= PUBLISH_INTERVAL_MS || tUltPub == 0) && relogioOk) {
    tUltPub = agora;
    histAmostrar();
    String json = montarJson();
    Serial.println(json);
    bool enviado = false;
    if (mqtt.connected() && filaTamanho() == 0) enviado = mqtt.publish(tTelem.c_str(), json.c_str(), true);
    if (!enviado) filaEnfileirar(json);
  }

  histTick();
  coletaTick();
  static uint32_t tUltFlush = 0;
  if (agora - tUltFlush >= 200) { tUltFlush = agora; filaDescarregar(); }

  // LED: aceso = MQTT ok · pisca rápido = portal aberto · pisca lento = sem conexão
  digitalWrite(PIN_LED, mqtt.connected() ? HIGH : portalAtivo ? ((agora / 100) % 2) : ((agora / 500) % 2));
  delay(2);
}
