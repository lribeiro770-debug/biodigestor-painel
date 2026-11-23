// =============================================================================
//  config.h — Monitoramento remoto de biodigestor (UTFPR-CM · EEL6006)
//  Valores PADRÃO de compilação. Rede, MQTT, Telegram e token também podem ser
//  alterados em campo (portal WiFi pelo celular, Serial "set" ou MQTT) e ficam
//  gravados na flash — não é preciso recompilar para trocar de rede.
// =============================================================================
#pragma once

// ------------------------------- MODO ---------------------------------------
#define MODO_SIMULADO       0         // 1 = dados sintéticos (teste fim a fim sem sensores)

// ------------------------------- REDE ---------------------------------------
// WiFi normal: configurado pelo PORTAL (celular) ou Serial "wifi <ssid> <senha>".
// Se não houver rede salva, ou ela não conectar em WIFI_TIMEOUT_S, o ESP32 abre
// o ponto de acesso "Biodigestor-XXXX" (senha abaixo) com a página de configuração.
#define PORTAL_SENHA        "biogas123"   // senha padrão do AP (mín. 8); troque com set:ap_pass=<senha>
#define PORTAL_TIMEOUT_S    300           // portal fecha e o sistema segue medindo offline
#define WIFI_TIMEOUT_S      20

// Rede WPA2-Enterprise (eduroam). Não é suportada pelo portal: só por compilação.
#define WIFI_ENTERPRISE     0
#define WIFI_EAP_SSID       "eduroam"
#define WIFI_EAP_IDENTITY   "usuario@utfpr.edu.br"
#define WIFI_EAP_USERNAME   "usuario@utfpr.edu.br"
#define WIFI_EAP_PASSWORD   "senha"

// ------------------------------- MQTT (padrões) -----------------------------
#define DEF_MQTT_HOST       "broker.hivemq.com"
#define DEF_MQTT_PORT       1883
#define DEF_MQTT_TLS        0         // 1 = TLS (porta 8883, ex.: HiveMQ Cloud)
#define DEF_MQTT_USER       ""
#define DEF_MQTT_PASS       ""
#define DEVICE_ID           "biodig-utfpr-cm-01"
#define DEF_TOPIC_BASE      "utfpr-cm/biodigestor/k7q2/" DEVICE_ID   // troque o "k7q2"!
#define DEF_CMD_TOKEN       "troque-este-token"   // exigido em comandos críticos e senha do OTA

// ------------------------------- TELEGRAM -----------------------------------
// Crie um bot com o @BotFather, mande uma mensagem para ele e descubra o chat_id
// (ver README). Deixe vazio para desativar. Também configurável em campo.
#define DEF_TG_TOKEN        ""
#define DEF_TG_CHAT         ""
#define TG_MIN_INTERVALO_MS 3000UL    // espaço mínimo entre mensagens

// ------------------------------- TEMPOS -------------------------------------
#define PUBLISH_INTERVAL_MS 30000UL
#define TEMP_INTERVAL_MS    5000UL
#define GAS_SAMPLE_MS       1000UL
#define ALARME_EVAL_MS      5000UL    // avaliação de alarmes / relé / buzzer
#define MQ_WARMUP_S         900       // 15 min: antes disso o Rs dos MQ ainda está subindo (falso alarme)
#define MQ_CAL_MIN_S        1800      // 30 min ligado antes de aceitar "cal" (use cal_forcar só em bancada)
#define WDT_TIMEOUT_S       30

// ------------------------------- PINOS --------------------------------------
// Analógicos SOMENTE em ADC1 (GPIO32–39): ADC2 não funciona com WiFi ativo.
#define PIN_ONEWIRE         4         // 2x DS18B20 (pull-up 4k7 p/ 3V3)
#define PIN_MQ2_AO          36        // VP · via divisor 10k/10k (placa v3.1)
#define PIN_MQ7_AO          39        // VN · via divisor 10k/10k (placa v3.1)
#define PIN_DHT             16        // DHT11 (pull-up 10k p/ 3V3; módulos já têm)
#define PIN_HX_DOUT         13        // módulo de pressão MPS20N0040D + HX710B (OUT)
#define PIN_HX_SCK          14        // idem (SCK)
#define PIN_RELE            26        // módulo relé (IN) — placa v3.1
#define PIN_BUZZER          27        // buzzer ativo 5V via BC337 — placa v3.1
#define PIN_PULSO_GAS       35        // (opcional) reed do medidor por pulsos — sem borne na placa v3.1
#define PIN_PRESSAO_ANALOG  35        // (alternativa) sensor de pressão analógico 0,5–4,5 V
#define PIN_MQ7_HEATER      12        // (opcional, não usado na placa v3.1) MOSFET do aquecedor do MQ-7
// microSD (placa v3.1, conector J15) — SPI remapeado para pinos livres
#define PIN_SD_MISO         34        // entrada (só leitura)
#define PIN_SD_CS           32
#define PIN_SD_SCK          33
#define PIN_SD_MOSI         25
#define SD_SPI_HZ           4000000
#define SD_INTERVALO_S      60        // uma linha no cartão a cada 60 s (com ou sem internet)
#define SD_LINHAS_POR_ARQ   10000     // ~1 semana por arquivo /log/NNNNNN.csv
#define WIFI_RETRY_MS       300000UL  // sem rede: tenta reconectar a cada 5 min (e só se ninguém estiver no AP)
#define PIN_BOTAO           0         // botão BOOT da placa: curto = silenciar; 3 s = portal
#define PIN_LED             2

// ------------------------------- DS18B20 (ambas no digestato) ---------------
// Sonda 1 = meio/fundo do digestato; Sonda 2 = parte superior (perto do headspace).
// A diferença entre elas indica ESTRATIFICAÇÃO (falta de mistura).
// Endereços impressos no Serial no boot ("enderecos"). Zerados = ordem de descoberta.
#define ADDR_DIG1           {0,0,0,0,0,0,0,0}
#define ADDR_DIG2           {0,0,0,0,0,0,0,0}
#define DS_RESOLUCAO_BITS   12

// ------------------------------- DHT ----------------------------------------
#define DHT_MODELO          11        // 11 = DHT11 (azul) · 22 = DHT22/AM2302 (branco)
#define DHT_INTERVAL_MS     5000UL    // DHT11: no máximo 1 leitura/s

// ------------------------- CONDICIONAMENTO ANALÓGICO (MQ) -------------------
#define DIV_R_TOP           10000.0f
#define DIV_R_BOT           10000.0f
#define ADC_V_MAX           2.60f
#define MQ_VC               5.00f     // MEÇA o 5 V real
#define MQ2_RL              1000.0f   // MEÇA RL do módulo (AO–GND, desligado)
#define MQ7_RL              1000.0f

// ------------------------------- MQ-7 (opcional) ----------------------------
#define MQ7_HEATER_CYCLE    0
#define MQ7_ALTO_MS         60000UL
#define MQ7_BAIXO_MS        90000UL
#define MQ7_JANELA_LEIT_MS  5000UL
#define MQ7_CO_A            99.042f
#define MQ7_CO_B            -1.518f
#define MQ7_RAZAO_AR_LIMPO  27.5f

// ------------------------------- PRESSÃO ------------------------------------
// 0 = sem sensor · 1 = analógico 0,5–4,5 V (XGZP6847A/MPX5010DP) · 2 = HX710B (MPS20N0040D)
#define PRESSAO_TIPO        2         // placa v3.1: módulo HX710B encaixado no J9
// HX710B: ALIMENTE O MÓDULO EM 3V3 (a saída OUT segue a alimentação).
// Com 3,3 V e ganho 128 a faixa útil é ~0–12 kPa — sobra para biodigestor pequeno.
// Escala padrão aproximada; CALIBRE com coluna d'água (comando cal_pressao).
#define HX_ESCALA_PADRAO    650.0f    // contagens por Pa (estimativa; o sinal é ajustado na calibração)
#define HX_MEDIA_N          10        // média móvel (10 amostras @10 Hz = 1 s)
// Analógico (PRESSAO_TIPO 1)
#define P_FUNDO_ESCALA_KPA  10.0f
#define P_V_ZERO            0.5f
#define P_V_SPAN            4.0f

// ------------------------------- RELÉ ---------------------------------------
// RELE_MODO 0 = desativado
//           1 = ALARME LOCAL (sirene/luz liga em alarme de segurança)
//           2 = VÁLVULA do método manométrico (pressão -> volume de biogás)
#define RELE_MODO           1
#define RELE_ATIVO_BAIXO    1         // a maioria dos módulos de relé liga com IN = LOW
#define RELE_DRENO_ABERTO   1         // módulo de 5 V comandado por 3,3 V: para DESLIGAR o pino fica solto (alta impedância),
                                      // porque 3,3 V no IN não basta para apagar o LED do optoacoplador
#define BUZZER_ATIVO        1         // 1 = há buzzer em PIN_BUZZER
#define SILENCIAR_MIN       30        // botão/comando silencia o alarme sonoro por N min

// ------------------------- MÉTODO MANOMÉTRICO (RELE_MODO 2) -----------------
// O headspace fica fechado; o gás acumula e a pressão sobe. Ao atingir P_ABRE a
// válvula abre (para o gasômetro/bolsa) até P_FECHA. Cada ciclo libera:
//     V_N = V_head · ΔP / P0 · T0 / T_head       (gás ideal, isotérmico)
// onde V_N está em litros normais (0 °C, 101,325 kPa).
// V_head = volume de gás acima do líquido. MEÇA (enchendo com água) e atualize
// sempre que o nível mudar (comando vhead:<litros>:TOKEN).
#define DEF_V_HEAD_L        20.0f
#define P_ABRE_PA           1000.0f   // ~10 cmH2O
#define P_FECHA_PA          200.0f
#define P_MAX_PA            2500.0f   // sobrepressão: válvula abre e alarme dispara
#define VALV_MAX_ABERTA_MS  20000UL
#define VALV_ESTAB_MS       3000UL    // espera após fechar para ler a pressão final
// ATENÇÃO: o software NÃO substitui um SELO HIDRÁULICO (alívio passivo) — ver README.

// --------------------------- MEDIDOR POR PULSOS (opcional) ------------------
#define ENABLE_MEDIDOR_GAS  0
#define LITROS_POR_PULSO    0.10f     // ex.: contador de báscula de 100 mL — CALIBRE
#define PULSO_DEBOUNCE_MS   200UL
#define VAZAO_ZERO_APOS_MS  3600000UL

// ------------------------------- ALARMES ------------------------------------
#define T_DIG_MIN_C         20.0f     // sem aquecimento: ajuste à realidade do biodigestor
#define T_DIG_MAX_C         40.0f
#define T_TAXA_MAX_CH       3.0f      // variação máxima aceitável em °C/h (janela de 1 h)
#define DT_ESTRAT_MAX_C     3.0f      // |T1 − T2| acima disso = estratificação
#define UR_MAX              90.0f     // risco de condensação na eletrônica
#define MQ2_ALARME_REL      0.50f     // Rs/R0 abaixo disso = vazamento de combustível
#define MQ7_ALARME_REL      0.50f
#define CO_ALARME_PPM       50.0f
#define MQ_COND_DT_C        10.0f     // fora de ±10 °C / ±25 %UR da calibração: leitura "fora de condição"
#define MQ_COND_DUR         25.0f

// ------------------------------- FILA / HORA --------------------------------
#define FILA_MAX_BYTES      560000UL  // partição de 640 kB (partitions.csv) ≈ 10 h sem rede
#define FLUSH_POR_LOOP      5
#define NTP_SERVER1         "a.st1.ntp.br"
#define NTP_SERVER2         "pool.ntp.org"
#define TZ_INFO             "<-03>3"
