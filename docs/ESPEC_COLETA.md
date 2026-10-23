# Especificação — coleta offline (firmware 3.0, apps Coletor, nuvem GitHub)

Versão do protocolo: **1**. Todos os componentes (firmware, app desktop, app Android, painel) seguem este documento.

## 1. Visão geral
```
[ESP32 + microSD] --(Wi-Fi próprio, HTTP)--> [App Coletor: notebook ou celular] --(internet, API do GitHub)--> [repositório biodigestor-painel/dados] --> [painel GitHub Pages]
```
- O ESP grava **uma linha por amostra** (intervalo padrão 60 s) no microSD, com ou sem internet.
- O ESP mantém **sempre** um ponto de acesso Wi-Fi próprio (AP) junto com a conexão normal (STA), quando houver.
- O Coletor baixa **só o que é novo** (pelo número de sequência `n`), guarda localmente e, quando tiver internet, envia ao GitHub.
- Sem relógio de hardware: a hora vem do NTP (quando há internet) ou do Coletor (`POST /api/hora`). Linhas gravadas antes de a hora ser conhecida recebem `ts` vazio e são corrigidas pelo ESP no momento da leitura, usando "âncoras" (ver §4).

## 2. Ponto de acesso do ESP
| Item | Valor |
|---|---|
| SSID | `Biodigestor-XXXX` (XXXX = 4 dígitos hex derivados do MAC; aparecem no monitor serial ao ligar) |
| Senha (WPA2) | configurável; padrão `biogas123` (troque no `set:ap_pass=`) |
| IP do ESP | `192.168.4.1` (porta 80, HTTP) |
| Descoberta | `GET http://192.168.4.1/api/info` responde JSON; também via mDNS `biodigestor.local` quando na mesma rede |

## 3. Autenticação
- Endpoints `/api/*`, exceto `/api/info` e `/api/agora`, exigem o **token de comando** (o mesmo dos comandos MQTT).
- Enviar como cabeçalho `X-Token: <token>` **ou** parâmetro de URL `token=<token>` (para links de download no navegador).
- Token errado: `401` com JSON `{"erro":"token"}`.

## 4. Hora e âncoras
- Cada amostra tem `boot` (contador de partidas, persistente) e `up` (segundos desde a partida).
- Quando a hora é acertada (NTP ou `/api/hora`), o ESP grava uma âncora `boot,up,epoch` em `/ancoras.csv`.
- Ao servir dados, para uma linha com `ts` vazio, se houver âncora do **mesmo boot**: `ts = epoch_ancora - (up_ancora - up_linha)`. Sem âncora no mesmo boot, `ts` continua vazio e o campo `ts_ok` vale `0`.
- `ts_ok`: `1` = medido com hora conhecida; `2` = reconstruído por âncora; `0` = desconhecido.

## 5. Endpoints HTTP do ESP
### `GET /api/info` (sem token)
```json
{"proto":1,"id":"biodig-utfpr-cm-01","fw":"3.0.0","serie":"a1b2c3d4","agora":1793000000,"hora_ok":true,
 "boot":17,"up":86400,"n_primeiro":1,"n_ultimo":43210,"intervalo_s":60,"sd_ok":true,"sd_livre_mb":14800,
 "ev_ultimo":512,"colunas":["n","ts","ts_ok","boot","up","t_dig","t_dig2","t_med","t_amb","ur_amb","mq2_rel","mq7_rel","p_pa","vol_nl","vazao_nlh","rele","alarmes","sim"]}
```
- `serie`: identificador aleatório (8 hex) criado quando o cartão é formatado ou usado pela primeira vez (`/serie.txt`). Se mudar, a numeração `n` recomeçou: o Coletor trata como série nova.

### `GET /api/agora` (sem token)
Última leitura, JSON no mesmo formato da telemetria MQTT (para a página local).

### `GET /api/dados?desde=<n>&max=<k>` (token)
- Devolve `text/csv; charset=utf-8`, **com cabeçalho**, colunas exatamente como `colunas` em `/api/info`, só linhas com `n > desde`, em ordem crescente, no máximo `k` linhas (padrão 2000, limite 5000).
- Cabeçalhos HTTP: `X-Ultimo-N` (maior n coberto pela janela pedida: `min(n_ultimo, desde+max)`; pode chegar sem linhas se houver buraco no cartão — o Coletor avança mesmo assim), `X-Mais: 1|0` (se ainda há linhas depois), `X-Serie`.
- Decimal com ponto; número ausente = campo vazio; `alarmes` = códigos separados por `|` (ex.: `GAS_COMBUSTIVEL|T_DIG_BAIXA`); `rele` e `sim` = `0|1`.
- O Coletor repete com `desde=X-Ultimo-N` enquanto `X-Mais: 1`.

### `GET /api/eventos?desde=<n>&max=<k>` (token)
CSV `n,ts,ts_ok,boot,up,tipo,detalhe`. Tipos: `boot`, `hora`, `alarme_on`, `alarme_off`, `coleta`, `sd_erro`, `wifi_on`, `wifi_off` (`sd_erro` é gravado quando o cartão volta, descrevendo a falha). `detalhe` vem sem vírgula, aspas ou quebra de linha e com no máximo 80 caracteres.

### `POST /api/hora` (token)
Corpo JSON `{"agora": <epoch_s>}`. Se a diferença for > 2 s, acerta o relógio e grava âncora. Resposta `{"ok":true,"ajuste_s":<diferença>}`.

### `POST /api/coletado` (token)
Corpo `{"ate":<n>,"coletor":"<nome>"}`. Só registra evento `coleta` (auditoria). Resposta `{"ok":true}`.

### `GET /` (sem token)
Página HTML local: leitura ao vivo, estado do cartão, botão "Acertar hora pelo celular", link para baixar CSV (pede o token).

## 6. Formato na nuvem (repositório `lribeiro770-debug/biodigestor-painel`, ramo `main`)
| Arquivo | Conteúdo |
|---|---|
| `dados/AAAA-MM.csv` | Linhas com `ts` conhecido daquele mês (UTC). Colunas: `ts,t_dig,t_dig2,t_med,t_amb,ur_amb,mq2_rel,mq7_rel,p_pa,vol_nl,vazao_nlh,rele,alarmes,sim,ts_ok,serie,n`. Ordenado por `ts`; sem duplicatas pela chave `serie+n`. |
| `dados/sem_hora.csv` | Linhas com `ts` desconhecido (mesmas colunas). |
| `dados/eventos.csv` | `ts,serie,n,tipo,detalhe` (ts pode ser vazio). |
| `dados/index.json` | `{"proto":1,"dispositivo":"biodig-utfpr-cm-01","meses":["2026-10"],"series":{"a1b2c3d4":{"ultimo_n":43210,"ultimo_ev":512}},"ultimo_ts":1793000000,"atualizado":1793000500,"colunas":[...]}` |

- Envio pela API REST de conteúdo do GitHub (`GET`/`PUT /repos/{owner}/{repo}/contents/{path}`), mesclando com o que já existe (baixa o arquivo, une pela chave `serie+n`, ordena, envia com o `sha`).
- Token: **fine-grained personal access token** do GitHub, com acesso só ao repositório `biodigestor-painel` e permissão **Contents: Read and write**. Fica salvo apenas no aparelho do Coletor.
- Mensagem do commit: `dados: +<linhas> linhas (<coletor>, n <de>–<até>)`.

## 7. Estado local do Coletor
- Pasta de dados do app: `dados/AAAA-MM.csv` locais (mesmo formato da nuvem) + `estado.json`:
  `{"esp":{"<serie>":{"ultimo_n":X,"ultimo_ev":Y}},"nuvem":{"<serie>":{"ultimo_n":A,"ultimo_ev":B}},"pendente":true}`.
- Ordem de uma coleta: `info` → `hora` (se `hora_ok` falso ou diferença > 2 s) → `dados` em lotes → `eventos` → `coletado` → salvar local.
- Envio: quando houver internet, enviar tudo com `n > nuvem.ultimo_n` por série; atualizar `index.json`.

## 8. ESP simulado para testes
`coleta/esp_simulado.py` implementa todos os endpoints acima em `http://127.0.0.1:8088` com dados sintéticos (incluindo um trecho sem hora para testar as âncoras). Token padrão: `teste123`.
