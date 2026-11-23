// =============================================================================
//  coleta.h — registro no microSD + ponto de acesso com API de coleta offline
//  Protocolo v1 (ver coleta/ESPEC_COLETA.md). Incluído no fim do .ino, antes do
//  setup(), por isso enxerga todas as variáveis e funções do programa principal.
// =============================================================================
#pragma once
#include <SD.h>
#include <SPI.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <sys/time.h>
#include <lwip/sockets.h>
#include <esp_timer.h>

SPIClass spiSD(HSPI);
WebServer web(80);
bool webAtivo = false;
uint32_t nPrimeiro = 0, evUltimo = 0, bootNum = 0, tUltSd = 0, tUltSdTentativa = 0, tUltHttp = 0;
bool sdTeveFalha = false, sdPrimeiraVez = true;
String serieSD = "";
volatile bool ntpChegou = false;
bool horaConhecida = false;

static const char* COLS_SD = "n,ts,ts_ok,boot,up,t_dig,t_dig2,t_med,t_amb,ur_amb,mq2_rel,mq7_rel,p_pa,vol_nl,vazao_nlh,rele,alarmes,sim";
static const char* COLS_EV = "n,ts,ts_ok,boot,up,tipo,detalhe";
static const char* ARQ_EV = "/eventos.csv";
static const char* ARQ_ANC = "/ancoras.csv";

struct Ancora { uint32_t boot, up, epoch; };
Ancora ancoras[64]; uint8_t nAncoras = 0;   // 1 âncora por boot (a primeira): basta para as linhas sem hora

// ------------------------------------------------------------------ utilidades
static uint32_t upS() { return (uint32_t)(esp_timer_get_time() / 1000000LL); }   // 64 bits: não dá a volta aos 49,7 dias
static String fmtF(float v, int casas) { return isnan(v) ? String("") : String(v, casas); }
static String arqLog(uint32_t n) {
  char b[24]; snprintf(b, sizeof b, "/log/%06lu.csv", (unsigned long)((n - 1) / SD_LINHAS_POR_ARQ)); return String(b);
}
static uint32_t primeiroCampo(const String& l) { return (uint32_t)strtoul(l.c_str(), nullptr, 10); }

// reseta o watchdog só se esta tarefa já estiver inscrita (evita "task not found" no boot)
static inline void wdtOk() { if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset(); }

// leitor de linhas em blocos: nunca trava em erro de leitura (read()==0 encerra)
struct LeitorLinhas {
  File& f; char b[512]; int len = 0, pos = 0; bool fim = false;
  explicit LeitorLinhas(File& arq) : f(arq) {}
  bool proxima(String& l) {
    l = ""; if (fim && pos >= len) return false;
    for (;;) {
      if (pos >= len) {
        if (fim) return l.length() > 0;
        len = f.read((uint8_t*)b, sizeof b); pos = 0;
        if (len <= 0) { len = 0; fim = true; return l.length() > 0; }
      }
      char c = b[pos++];
      if (c == '\n') { l.trim(); return true; }
      if (l.length() < 400) l += c;               // linha absurda é truncada (e depois descartada)
    }
  }
};
static int contarVirgulas(const String& l) { int c = 0; for (size_t i = 0; i < l.length(); i++) if (l[i] == ',') c++; return c; }
static const int VIRG_LOG = 17, VIRG_EV = 6;
static bool linhaValida(const String& l, int virg) {
  if (!l.length() || l[0] < '1' || l[0] > '9') return false;
  return contarVirgulas(l) == virg;
}

// maior n válido do arquivo (dentro de [lo, hi]); olha a cauda e, se ela estiver danificada, varre tudo
static uint32_t ultimoN(const char* path, int virg, uint32_t lo, uint32_t hi) {
  File f = SD.open(path, FILE_READ); if (!f) return 0;
  f.setTimeout(0);
  uint32_t maior = 0; size_t sz = f.size();
  if (sz > 2048) f.seek(sz - 2048);
  for (int passada = 0; passada < 2 && !maior; passada++) {
    if (passada == 1) f.seek(0);
    LeitorLinhas r(f); String l; uint32_t cont = 0;
    while (r.proxima(l)) {
      if (!linhaValida(l, virg)) continue;
      uint32_t k = primeiroCampo(l); if (k >= lo && k <= hi && k > maior) maior = k;
      if (++cont % 500 == 0) wdtOk();
    }
    if (sz <= 2048) break;
  }
  f.close(); return maior;
}

static void bootContar() {
  prefs.begin("bio", false); bootNum = prefs.getUInt("boot", 0) + 1; prefs.putUInt("boot", bootNum); prefs.end();
}

// --------------------------------------------------------------------- âncoras
// Cada boot guarda só a PRIMEIRA âncora: linhas sem hora existem apenas antes dela;
// depois disso todas já saem com ts. Ajustes posteriores viram só evento.
static bool ancoraDoBoot(uint32_t b, Ancora& out) {
  for (int i = nAncoras - 1; i >= 0; i--) if (ancoras[i].boot == b) { out = ancoras[i]; return true; }
  return false;
}
static void ancoraMemoria(uint32_t b, uint32_t u, uint32_t e) {
  Ancora x; if (ancoraDoBoot(b, x)) return;
  if (nAncoras == sizeof(ancoras) / sizeof(ancoras[0])) { memmove(ancoras, ancoras + 1, sizeof(Ancora) * (nAncoras - 1)); nAncoras--; }
  ancoras[nAncoras++] = {b, u, e};
}
static void ancoraArquivo(const Ancora& a) {
  File f = SD.open(ARQ_ANC, FILE_APPEND); if (f) { f.printf("%lu,%lu,%lu\n", (unsigned long)a.boot, (unsigned long)a.up, (unsigned long)a.epoch); f.close(); }
}
static void ancorasCarregar() {
  Ancora minha; bool tenho = ancoraDoBoot(bootNum, minha);   // âncora deste boot que ainda não foi ao cartão
  nAncoras = 0;
  File f = SD.open(ARQ_ANC, FILE_READ);
  if (f) {
    f.setTimeout(0); LeitorLinhas r(f); String l;
    while (r.proxima(l)) {
      uint32_t b, u, e; if (sscanf(l.c_str(), "%lu,%lu,%lu", (unsigned long*)&b, (unsigned long*)&u, (unsigned long*)&e) == 3) ancoraMemoria(b, u, e);
    }
    f.close();
  }
  Ancora x; if (tenho && !ancoraDoBoot(bootNum, x)) { ancoraMemoria(minha.boot, minha.up, minha.epoch); ancoraArquivo(minha); }
}
// devolve true se gravou uma âncora nova (1ª do boot)
static bool ancoraGravar(uint32_t e) {
  Ancora x; if (ancoraDoBoot(bootNum, x)) return false;
  uint32_t u = upS(); ancoraMemoria(bootNum, u, e);
  if (sdOk) ancoraArquivo({bootNum, u, e});
  return true;
}

// --------------------------------------------------------------------- eventos
static void evento(const char* tipo, const String& detalhe) {
  Serial.printf("[EV] %s %s\n", tipo, detalhe.c_str());
  if (!sdOk) return;
  bool novo = !SD.exists(ARQ_EV);
  File f = SD.open(ARQ_EV, FILE_APPEND); if (!f) return;
  if (novo) f.println(COLS_EV);
  String d = detalhe; d.replace(",", ";"); d.replace("\n", " "); d.replace("\r", " "); d.replace("\"", "'");
  if (d.length() > 80) d = d.substring(0, 80);
  uint32_t e = epochAgora();
  f.printf("%lu,%s,%d,%lu,%lu,%s,%s\n", (unsigned long)(evUltimo + 1), e ? String(e).c_str() : "", e ? 1 : 0,
           (unsigned long)bootNum, (unsigned long)upS(), tipo, d.c_str());
  f.close(); evUltimo++;
}

// ------------------------------------------------------------------ hora/relógio
static uint32_t epochPrevisto() {      // hora que o relógio deveria marcar pela âncora deste boot
  Ancora a; if (!ancoraDoBoot(bootNum, a)) return 0;
  return a.epoch + (upS() - a.up);
}
static void horaDefinida(uint32_t epoch, const char* fonte, uint32_t previsto) {
  bool antes = horaConhecida; horaConhecida = true;
  ancoraGravar(epoch);
  if (!antes) { evento("hora", String(fonte) + " (relógio acertado)"); return; }
  long dif = previsto ? (long)epoch - (long)previsto : 0;
  if (labs(dif) > 2) evento("hora", String(fonte) + " (ajuste " + String(dif) + " s)");   // ressincronias normais do NTP (a cada 3 h) não viram evento
}
static void ntpSincronizou(struct timeval* tv) { ntpChegou = true; }   // contexto do lwIP: só marca

// ------------------------------------------------------------------------- SD
static String sdStatus() {
  if (!sdOk) return "microSD: AUSENTE ou com falha (confira o cartão e os jumpers do J15)";
  return "microSD ok · série " + serieSD + " · amostras " + String(nPrimeiro) + "–" + String(nUltimo) +
         " · eventos " + String(evUltimo) + " · livre " + String((uint32_t)((SD.totalBytes() - SD.usedBytes()) / 1048576ULL)) + " MB";
}

static void sdIniciar() {
  spiSD.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  sdOk = SD.begin(PIN_SD_CS, spiSD, SD_SPI_HZ);
  tUltSdTentativa = millis();
  if (!sdOk) { sdTeveFalha = true; Serial.println("[SD] cartão não encontrado — registro local DESATIVADO"); return; }
  if (!SD.exists("/log")) SD.mkdir("/log");
  // série: identifica este cartão; se mudar, a numeração recomeçou
  File s = SD.open("/serie.txt", FILE_READ);
  if (s) { serieSD = s.readStringUntil('\n'); serieSD.trim(); s.close(); }
  if (serieSD.length() != 8) {
    char b[9]; snprintf(b, sizeof b, "%08lx", (unsigned long)esp_random()); serieSD = b;
    File w = SD.open("/serie.txt", FILE_WRITE); if (w) { w.println(serieSD); w.close(); }
  }
  // número do boot também no cartão: se a flash for apagada, não repete números antigos
  if (sdPrimeiraVez) {
    sdPrimeiraVez = false;
    File bf = SD.open("/boot.txt", FILE_READ);
    if (bf) { uint32_t bsd = bf.readStringUntil('\n').toInt(); bf.close();
      if (bsd >= bootNum) { bootNum = bsd + 1; prefs.begin("bio", false); prefs.putUInt("boot", bootNum); prefs.end(); } }
    File bw = SD.open("/boot.txt", FILE_WRITE); if (bw) { bw.println(bootNum); bw.close(); }
  }
  // recupera o último n pelo arquivo de maior número
  int32_t maior = -1, menor = -1;
  File d = SD.open("/log");
  for (File e = d.openNextFile(); e; e = d.openNextFile()) {
    String nm = e.name(); e.close();
    if (!nm.endsWith(".csv")) continue;
    int32_t k = nm.substring(nm.lastIndexOf('/') + 1).toInt();
    if (k > maior) maior = k;
    if (menor < 0 || k < menor) menor = k;
  }
  d.close();
  nUltimo = 0; nPrimeiro = 0;
  if (maior >= 0) {
    char p[24]; snprintf(p, sizeof p, "/log/%06ld.csv", (long)maior);
    uint32_t lo = (uint32_t)maior * SD_LINHAS_POR_ARQ + 1, hi = lo + SD_LINHAS_POR_ARQ - 1;
    nUltimo = ultimoN(p, VIRG_LOG, lo, hi);
    if (nUltimo == 0) nUltimo = lo - 1;
    nPrimeiro = (uint32_t)menor * SD_LINHAS_POR_ARQ + 1;
    if (nPrimeiro > nUltimo) nPrimeiro = nUltimo ? nUltimo : 0;
  }
  // piso gravado na flash (a cada hora): nunca reutilizar um n já entregue
  prefs.begin("bio", true); String ns = prefs.getString("nser", ""); uint32_t nmin = prefs.getUInt("nmax", 0); prefs.end();
  if (ns == serieSD && nmin > nUltimo) { Serial.printf("[SD] cauda danificada: n retomado em %lu\n", (unsigned long)nmin); nUltimo = nmin; }
  evUltimo = ultimoN(ARQ_EV, VIRG_EV, 1, 0xFFFFFFFFUL);
  ancorasCarregar();
  Serial.println("[SD] " + sdStatus());
}

static void nPisoSalvar() {
  prefs.begin("bio", false); prefs.putString("nser", serieSD); prefs.putUInt("nmax", nUltimo); prefs.end();
}

static void sdGravarAmostra() {
  uint32_t n = nUltimo + 1;
  String nome = arqLog(n);
  bool novo = !SD.exists(nome);
  File f = SD.open(nome, FILE_APPEND);
  if (!f) { sdOk = false; Serial.println("[SD] falha ao gravar — cartão removido?"); return; }
  if (novo) f.println(COLS_SD);
  uint32_t e = epochAgora();
  float tMed = (!isnan(tDig1) && !isnan(tDig2)) ? (tDig1 + tDig2) / 2 : (!isnan(tDig1) ? tDig1 : tDig2);
  float rel2 = (mq2.r0 > 0 && !isnan(mq2.rs)) ? mq2.rs / mq2.r0 : NAN;
#if MQ7_HEATER_CYCLE
  float rs7 = mq7RsUltimoCiclo;
#else
  float rs7 = mq7.rs;
#endif
  float rel7 = (mq7.r0 > 0 && !isnan(rs7)) ? rs7 / mq7.r0 : NAN;
  float p = simAtivo ? simP : pressaoPa;
  // volume só existe no modo válvula (gasômetro rígido); com balão flexível fica em branco
  float vol = simAtivo ? simVol : (RELE_MODO == 2 ? volNl : NAN), vaz = simAtivo ? simVazao : (RELE_MODO == 2 ? vazaoNlh : NAN);
  String al;
  for (int a = 0; a < A_N; a++) if (alarmesAtivos & (1u << a)) { if (al.length()) al += '|'; al += ALARME_NOME[a]; }
  String l; l.reserve(160);
  l += n; l += ','; if (e) l += e; l += ','; l += (e ? 1 : 0); l += ','; l += bootNum; l += ','; l += upS(); l += ',';
  l += fmtF(tDig1, 2); l += ','; l += fmtF(tDig2, 2); l += ','; l += fmtF(tMed, 2); l += ',';
  l += fmtF(tAmb, 1); l += ','; l += fmtF(urAmb, 0); l += ','; l += fmtF(rel2, 3); l += ','; l += fmtF(rel7, 3); l += ',';
  l += fmtF(p, 0); l += ','; l += fmtF(vol, 3); l += ','; l += fmtF(vaz, 3); l += ',';
  l += (releLigado ? 1 : 0); l += ','; l += al; l += ','; l += (simAtivo ? 1 : 0);
  size_t w = f.println(l); f.close();
  if (w < l.length()) { sdOk = false; Serial.println("[SD] gravação incompleta"); return; }
  nUltimo = n; if (!nPrimeiro) nPrimeiro = n;
  if (n % 60 == 0) nPisoSalvar();
}

// completa ts de linha gravada sem hora, usando a âncora do mesmo boot
static String corrigirLinha(const String& l) {
  int c1 = l.indexOf(','); if (c1 < 0) return l;
  int c2 = l.indexOf(',', c1 + 1); if (c2 < 0 || c2 != c1 + 1) return l;      // ts já preenchido
  int c3 = l.indexOf(',', c2 + 1); int c4 = l.indexOf(',', c3 + 1); int c5 = l.indexOf(',', c4 + 1);
  if (c3 < 0 || c4 < 0 || c5 < 0) return l;
  uint32_t b = strtoul(l.substring(c3 + 1, c4).c_str(), nullptr, 10);
  uint32_t u = strtoul(l.substring(c4 + 1, c5).c_str(), nullptr, 10);
  Ancora a; if (!ancoraDoBoot(b, a)) return l;
  uint32_t ts = a.epoch - (a.up - u);          // aritmética módulo 2^32: vale antes e depois da âncora
  return l.substring(0, c1 + 1) + String(ts) + ",2" + l.substring(c3);
}

// ------------------------------------------------------------------- servidor
static void cors() {
  tUltHttp = millis();
  web.sendHeader("Access-Control-Allow-Origin", "*");
  web.sendHeader("Access-Control-Expose-Headers", "X-Ultimo-N, X-Mais, X-Serie");
}
static bool tokenOk() {
  String t = web.hasHeader("X-Token") ? web.header("X-Token") : web.arg("token");
  if (t == cfg.token) return true;
  cors(); web.send(401, "application/json", "{\"erro\":\"token\"}"); return false;
}

static void hInfo() {
  JsonDocument d;
  d["proto"] = 1; d["id"] = DEVICE_ID; d["fw"] = FW_VERSAO; d["serie"] = serieSD;
  d["agora"] = epochAgora(); d["hora_ok"] = epochAgora() != 0;
  d["boot"] = bootNum; d["up"] = upS();
  d["n_primeiro"] = nPrimeiro; d["n_ultimo"] = nUltimo; d["intervalo_s"] = SD_INTERVALO_S;
  d["sd_ok"] = sdOk; d["sd_livre_mb"] = sdOk ? (uint32_t)((SD.totalBytes() - SD.usedBytes()) / 1048576ULL) : 0;
  d["ev_ultimo"] = evUltimo;
  JsonArray c = d["colunas"].to<JsonArray>();
  String cols = COLS_SD; int i = 0;
  while (i < (int)cols.length()) { int j = cols.indexOf(',', i); if (j < 0) j = cols.length(); c.add(cols.substring(i, j)); i = j + 1; }
  d["wifi"] = WiFi.isConnected() ? WiFi.SSID() : String(""); d["mqtt"] = mqtt.connected();
  String out; serializeJson(d, out); cors(); web.send(200, "application/json", out);
}

static void hAgora() { cors(); web.send(200, "application/json", montarJson()); }

// ---- envio direto no socket: se o coletor sumir no meio, desiste em ~4 s (sem travar o loop nem o watchdog)
static bool envioOk = true;
static bool enviarBruto(const char* p, size_t n) {
  if (!envioOk) return false;
  int fd = web.client().fd(); if (fd < 0) { envioOk = false; return false; }
  uint32_t semProgresso = millis();
  while (n) {
    wdtOk();
    fd_set ws; FD_ZERO(&ws); FD_SET(fd, &ws); struct timeval tv = {0, 200000};
    if (select(fd + 1, nullptr, &ws, nullptr, &tv) < 0) { envioOk = false; return false; }
    if (FD_ISSET(fd, &ws)) {
      int r = send(fd, p, n, MSG_DONTWAIT);
      if (r > 0) { p += r; n -= r; semProgresso = millis(); continue; }
      if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { envioOk = false; return false; }
    }
    if (millis() - semProgresso > 4000UL) { envioOk = false; return false; }
  }
  return true;
}
static bool enviarBruto(const String& s) { return enviarBruto(s.c_str(), s.length()); }

// posiciona o arquivo perto da 1ª linha com n > desde (busca binária por bytes; o resto é linear)
static void posicionar(File& f, uint32_t desde) {
  size_t lo = 0, hi = f.size(); String l;
  while (hi - lo > 2048) {
    size_t meio = lo + (hi - lo) / 2; f.seek(meio);
    LeitorLinhas r(f); uint32_t k = 0;
    for (int i = 0; i < 3 && r.proxima(l); i++) { if (i == 0) continue; if (linhaValida(l, VIRG_LOG)) { k = primeiroCampo(l); break; } }
    if (k && k <= desde) lo = meio; else hi = meio;
  }
  f.seek(lo);
}

// envia linhas com n em (desde, ate] — corpo cru, conexão fechada no fim
static void transmitirCsv(const char* cabecalho, bool ehLog, uint32_t desde, uint32_t ate) {
  enviarBruto(String(cabecalho) + "\n");
  String buf; buf.reserve(2300);
  const int virg = ehLog ? VIRG_LOG : VIRG_EV;
  uint32_t n = desde + 1;
  while (n <= ate && envioOk) {
    String nome = ehLog ? arqLog(n) : String(ARQ_EV);
    uint32_t teto = ehLog ? ((n - 1) / SD_LINHAS_POR_ARQ + 1) * SD_LINHAS_POR_ARQ : 0xFFFFFFFFUL;
    File f = SD.open(nome, FILE_READ);
    if (!f) { if (!ehLog) break; n = teto + 1; continue; }
    f.setTimeout(0);
    if (ehLog && n == desde + 1 && (desde % SD_LINHAS_POR_ARQ) > 200) posicionar(f, desde);   // pula direto para perto de desde
    LeitorLinhas r(f); String l; uint32_t ultimoLido = 0, cont = 0;
    while (envioOk && r.proxima(l)) {
      if (++cont % 200 == 0) wdtOk();
      if (!linhaValida(l, virg)) continue;          // cabeçalho, linha incompleta ou lixo
      uint32_t k = primeiroCampo(l);
      if (k <= desde || k > teto) continue;         // fora da faixa deste arquivo: ignora (não encerra)
      if (k > ate) { ultimoLido = k; break; }
      buf += ehLog ? corrigirLinha(l) : l; buf += '\n';
      ultimoLido = k;
      if (buf.length() > 2000) { enviarBruto(buf); buf = ""; }
    }
    f.close();
    if (!ehLog || ultimoLido >= ate) break;
    n = teto + 1;                                   // próximo arquivo
  }
  if (buf.length()) enviarBruto(buf);
}


// ------------------------------------------------------------------ diário (microSD)
static void sdDiarioAppend(const JsonDocument& e) {
  if (!sdOk) return;
  bool novo = !SD.exists("/diario.csv");
  File f = SD.open("/diario.csv", FILE_APPEND); if (!f) return;
  if (novo) f.println("data_hora;ts;tipo;valor;unid;autor;obs;co2_pct;vi_ml;vf_ml");
  time_t t = (time_t)(e["ts"] | 0UL); struct tm tmv; localtime_r(&t, &tmv);
  char dh[24]; strftime(dh, sizeof dh, "%d/%m/%Y %H:%M", &tmv);
  auto cel = [&](const char* k) -> String {
    JsonVariantConst v = e[k]; if (v.isNull()) return "";
    String s; if (v.is<const char*>()) s = v.as<const char*>(); else serializeJson(v, s);
    s.replace(";", ","); return s;
  };
  f.printf("%s;%s;%s;%s;%s;%s;%s;%s;%s;%s\n", dh, cel("ts").c_str(), cel("tipo").c_str(), cel("valor").c_str(), cel("unid").c_str(),
           cel("autor").c_str(), cel("obs").c_str(), cel("co2_pct").c_str(), cel("vi_ml").c_str(), cel("vf_ml").c_str());
  f.close();
}

static void hDiario() {
  if (!tokenOk()) return;
  cors();
  File f = LittleFS.open(DIARIO_ARQ, "r");
  if (!f) { web.send(200, "application/x-ndjson", ""); return; }
  web.streamFile(f, "application/x-ndjson"); f.close();
}

static void hDados(bool ehLog) {
  if (!tokenOk()) return;
  if (!sdOk) { cors(); web.send(503, "application/json", "{\"erro\":\"sd\"}"); return; }
  uint32_t desde = strtoul(web.arg("desde").c_str(), nullptr, 10);
  uint32_t mx = web.hasArg("max") ? strtoul(web.arg("max").c_str(), nullptr, 10) : 2000;
  mx = constrain(mx, 1UL, 5000UL);
  uint32_t total = ehLog ? nUltimo : evUltimo;
  uint32_t ate = (desde > 0xFFFFFFFFUL - mx) ? total : min(total, desde + mx); if (ate < desde) ate = desde;
  // cabeçalhos escritos à mão: corpo cru até o fechamento (sem "chunked"), para controlar cada envio
  String h; h.reserve(320);
  h += "HTTP/1.1 200 OK\r\nContent-Type: text/csv; charset=utf-8\r\nConnection: close\r\nCache-Control: no-store\r\n";
  h += "Access-Control-Allow-Origin: *\r\nAccess-Control-Expose-Headers: X-Ultimo-N, X-Mais, X-Serie\r\n";
  h += "X-Ultimo-N: " + String(ate) + "\r\nX-Mais: " + String(total > ate ? "1" : "0") + "\r\nX-Serie: " + serieSD + "\r\n\r\n";
  envioOk = true; tUltHttp = millis();
  if (enviarBruto(h)) transmitirCsv(ehLog ? COLS_SD : COLS_EV, ehLog, desde, ate);
  if (!envioOk) Serial.println("[WEB] coletor desconectou no meio do envio — abortado");
  web.client().stop();
}

static void hHora() {
  if (!tokenOk()) return;
  JsonDocument d; if (deserializeJson(d, web.arg("plain")) || !d["agora"].is<uint32_t>()) { cors(); web.send(400, "application/json", "{\"erro\":\"json\"}"); return; }
  uint32_t ag = d["agora"].as<uint32_t>();
  if (ag < 1700000000UL) { cors(); web.send(400, "application/json", "{\"erro\":\"hora inválida\"}"); return; }
  uint32_t atual = epochAgora();
  long dif = atual ? (long)ag - (long)atual : 0;
  String resp;
  if (!atual || labs(dif) > 2) {
    uint32_t prev = epochPrevisto();
    struct timeval tv = {(time_t)ag, 0}; settimeofday(&tv, nullptr);
    horaDefinida(ag, "coletor", prev ? prev : atual);
    resp = atual ? "{\"ok\":true,\"ajuste_s\":" + String(dif) + "}" : String("{\"ok\":true,\"ajuste_s\":null}");
  } else resp = "{\"ok\":true,\"ajuste_s\":" + String(dif) + "}";
  cors(); web.send(200, "application/json", resp);
}

static void hColetado() {
  if (!tokenOk()) return;
  JsonDocument d; deserializeJson(d, web.arg("plain"));
  evento("coleta", String(d["coletor"] | "?") + " ate " + String(d["ate"] | 0));
  cors(); web.send(200, "application/json", "{\"ok\":true}");
}

static const char PAGINA[] PROGMEM = R"HTML(<!doctype html><html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Biodigestor</title><style>
body{font-family:system-ui,sans-serif;margin:0;background:#f2f3f5;color:#1d2329}main{max-width:560px;margin:auto;padding:16px}
h1{font-size:20px;margin:4px 0 12px}.c{background:#fff;border:1px solid #d6dbe0;border-radius:10px;padding:12px;margin-bottom:12px}
.g{display:grid;grid-template-columns:1fr 1fr;gap:8px}.v{font-size:22px;font-weight:600}.l{font-size:12px;color:#5d6873}
button,input{font:inherit;padding:10px;border-radius:8px;border:1px solid #c9ced3;width:100%;box-sizing:border-box}
button{background:#b8692f;color:#fff;border:0;margin-top:8px}.al{color:#d03b3b;font-weight:600}small{color:#5d6873}
@media(prefers-color-scheme:dark){body{background:#14181c;color:#e6eaee}.c{background:#1c2227;border-color:#2c343b}input{background:#14181c;color:#e6eaee;border-color:#2c343b}}
</style></head><body><main><h1>Biodigestor · coleta local</h1>
<div class="c g"><div><div class="l">Digestato (média)</div><div class="v" id="tm">—</div></div><div><div class="l">Ambiente</div><div class="v" id="ta">—</div></div>
<div><div class="l">Pressão</div><div class="v" id="p">—</div></div><div><div class="l">MQ-2 (Rs/R0)</div><div class="v" id="g">—</div></div></div>
<div class="c"><div id="al">Sem alarmes</div><small id="st"></small></div>
<div class="c"><div class="l">Token de comando</div><input id="tk" type="password" autocomplete="off" placeholder="token">
<button onclick="hora()">Acertar a hora pelo celular</button><button onclick="baixar()">Baixar todos os dados (CSV)</button>
<button onclick="location.href='/portal?token='+encodeURIComponent(tk.value)">Configurar Wi-Fi (abre o portal)</button>
<small id="msg"></small></div><small>Para enviar à nuvem, use o app Coletor ou importe o CSV no painel.</small></main>
<script>
const $=i=>document.getElementById(i),f=(v,d=1)=>v==null?'—':Number(v).toFixed(d);
async function at(){try{const j=await(await fetch('/api/agora')).json();$('tm').textContent=f(j.t_med)+' °C';$('ta').textContent=f(j.t_amb)+' °C · '+f(j.ur_amb,0)+'%';
$('p').textContent=j.p_pa==null?'—':Math.round(j.p_pa)+' Pa';$('g').textContent=j.mq2&&j.mq2.rel!=null?f(j.mq2.rel,2):'—';
$('al').innerHTML=j.alarmes&&j.alarmes.length?'<span class="al">'+j.alarmes.join(', ')+'</span>':'Sem alarmes';
const i=await(await fetch('/api/info')).json();$('st').textContent='Amostras no cartão: '+i.n_ultimo+(i.sd_ok?'':' (CARTÃO COM FALHA)')+' · hora '+(i.hora_ok?new Date(i.agora*1000).toLocaleString('pt-BR'):'NÃO acertada')+' · fw '+i.fw;}catch(e){$('st').textContent='sem resposta';}}
async function hora(){const r=await fetch('/api/hora',{method:'POST',headers:{'X-Token':tk.value},body:JSON.stringify({agora:Math.floor(Date.now()/1000)})});$('msg').textContent=r.ok?'Hora acertada.':'Falhou: confira o token.';at();}
async function baixar(){let d=0,out='',mais=true;$('msg').textContent='Baixando…';
try{while(mais){const r=await fetch('/api/dados?desde='+d+'&max=5000',{headers:{'X-Token':tk.value}});if(!r.ok){$('msg').textContent='Falhou: confira o token.';return;}
let t=await r.text();if(out)t=t.slice(t.indexOf('\n')+1);out+=t;const u=parseInt(r.headers.get('X-Ultimo-N'),10);mais=r.headers.get('X-Mais')==='1'&&u>d;d=u;$('msg').textContent='Baixando… até n '+d;}
const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([out],{type:'text/csv'}));a.download='biodigestor_'+new Date().toISOString().slice(0,10)+'.csv';a.click();$('msg').textContent='Pronto: até n '+d+'.';}
catch(e){$('msg').textContent='Falhou no meio: tente de novo.';}}
at();setInterval(at,5000);
</script></body></html>)HTML";

static void hRaiz() { web.send_P(200, "text/html; charset=utf-8", PAGINA); }
// GET só mostra um botão; abrir exige POST. Antes, uma aba do navegador com /portal aberta reabria o
// portal sozinha toda vez que o celular voltava para a rede do ESP (recarga automática), e a coleta dava 404.
static void hPortal() {
  if (web.method() != HTTP_POST) {
    web.send(200, "text/html; charset=utf-8",
      "<meta name=viewport content='width=device-width'><h3>Portal de Wi-Fi</h3>"
      "<p>Enquanto o portal estiver aberto (até 5 min), a coleta pelo app fica indisponível.</p>"
      "<form method=post><input name=token placeholder='token de comando'> <button>Abrir portal</button></form>");
    return;
  }
  if (!tokenOk()) return;
  web.send(200, "text/plain; charset=utf-8", "Abrindo o portal de configuração. Reconecte em " + apNome() + " em alguns segundos.");
  delay(300); portalAbrir("página /portal");
}

static bool rotasOk = false, mdnsOk = false;
static void webIniciar() {
  if (webAtivo) return;
  if (!rotasOk) {                                  // registra as rotas uma vez só (o portal para/reinicia o servidor)
    rotasOk = true;
    const char* hdr[] = {"X-Token"}; web.collectHeaders(hdr, 1);
    web.on("/", HTTP_GET, hRaiz);
    web.on("/api/info", HTTP_GET, hInfo);
    web.on("/api/agora", HTTP_GET, hAgora);
    web.on("/api/dados", HTTP_GET, []() { hDados(true); });
    web.on("/api/eventos", HTTP_GET, []() { hDados(false); });
    web.on("/api/diario", HTTP_GET, hDiario);
    web.on("/api/hora", HTTP_POST, hHora);
    web.on("/api/coletado", HTTP_POST, hColetado);
    web.on("/portal", HTTP_ANY, hPortal);
    web.onNotFound([]() {
      if (web.method() == HTTP_OPTIONS) {
        cors(); web.sendHeader("Access-Control-Allow-Headers", "X-Token, Content-Type"); web.sendHeader("Access-Control-Allow-Methods", "GET, POST");
        web.send(204); return;
      }
      cors(); web.send(404, "application/json", "{\"erro\":\"rota\"}");
    });
  }
  web.begin(); webAtivo = true;
  if (!mdnsOk && MDNS.begin("biodigestor")) {
    mdnsOk = true; MDNS.addService("http", "tcp", 80);
    MDNS.enableArduino(3232, true);                 // OTA local também por biodigestor.local
  }
  Serial.println("[WEB] API de coleta em http://192.168.4.1 (e http://biodigestor.local)");
}
static void webParar() { if (webAtivo) { web.stop(); webAtivo = false; } }

// ------------------------------------------------------------------- tick
static void coletaTick() {
  if (webAtivo) web.handleClient();
  if (ntpChegou) {
    ntpChegou = false;
    uint32_t e = epochAgora();
    if (e) horaDefinida(e, "ntp", epochPrevisto());
  }
  uint32_t agora = millis();
  if (!sdOk) {
    if (agora - tUltSdTentativa > 60000UL) {
      SD.end(); sdIniciar();
      if (sdOk && sdTeveFalha) { sdTeveFalha = false; evento("sd_erro", "cartão falhou/ausente e voltou (amostras do intervalo perdidas)"); }
    }
    return;
  }
  if (tUltSd == 0 || agora - tUltSd >= SD_INTERVALO_S * 1000UL) {
    // espera os sensores estabilizarem na primeira amostra (10 s após o boot)
    if (tUltSd == 0 && agora - bootMs < 10000UL) return;
    tUltSd = agora;
    sdGravarAmostra();
    if (!sdOk) sdTeveFalha = true;                  // registrado como evento quando o cartão voltar
  }
}
