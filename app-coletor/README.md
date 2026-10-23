# Coletor do Biodigestor — app Android

App de campo que **baixa os dados do ESP32 do biodigestor pelo Wi-Fi próprio dele (sem internet)**, guarda no celular e, quando o celular tiver internet, **envia para o GitHub** (`lribeiro770-debug/biodigestor-painel`, pasta `dados/`), de onde o painel no GitHub Pages lê os gráficos.

Segue o protocolo de `coleta/ESPEC_COLETA.md` (versão 1).

```
[ESP32 + microSD] --Wi-Fi "Biodigestor-XXXX"--> [celular: Coletor] --internet--> [GitHub: dados/*.csv] --> [painel]
```

## O que o app faz

- **Conectar à rede do biodigestor**: no Android 10 ou mais novo, pede ao sistema para entrar na rede `Biodigestor-…` com a senha configurada (você confirma numa janela do Android). Em versões antigas, ou se der errado, abre o painel de Wi-Fi para você escolher a rede.
- **Coletar agora**:
  1. lê `/api/info` do ESP;
  2. acerta o relógio do ESP pela hora do celular (se o ESP estiver sem hora ou com mais de 2 s de diferença);
  3. baixa **só as linhas novas** (pelo número `n`), em lotes, gravando cada lote no celular antes de pedir o próximo; se a coleta cair no meio, a próxima continua de onde parou;
  4. baixa os eventos novos e registra a coleta no ESP (`/api/coletado`).
- **Enviar para a nuvem**: manda tudo que ainda não foi enviado, mesclando com o que já existe no repositório (sem duplicar, pela chave `serie+n`), ordenado por hora, e atualiza `dados/index.json`. Se outro coletor gravou ao mesmo tempo, o app baixa de novo, mescla e tenta outra vez.
- **Envio automático** (opcional, ligado por padrão): assim que o celular tiver internet e houver dados pendentes, envia sozinho.
- Mostra: ESP alcançável ou não, série do cartão, último `n` no ESP e último coletado, se a hora foi ajustada, linhas pendentes, último envio, a última leitura (temperatura do digestato e do ar, umidade, pressão, alarmes) e um registro de tudo o que aconteceu.
- Funciona sem internet. Modo escuro automático.

### Arquivos no celular (área privada do app)

Mesmo formato da nuvem (§6/§7 da especificação):

| Arquivo | Conteúdo |
|---|---|
| `dados/AAAA-MM.csv` | linhas com hora conhecida, separadas por mês **UTC** |
| `dados/sem_hora.csv` | linhas gravadas antes de o ESP saber a hora (e sem âncora no mesmo boot) |
| `dados/eventos.csv` | eventos do ESP (`ts,serie,n,tipo,detalhe`) |
| `estado.json` | até onde já coletou (`esp`) e até onde já enviou (`nuvem`), por série |

Se o cartão do ESP for formatado/trocado, a **série** muda e a numeração recomeça: o app trata como série nova, sem misturar com a antiga.

## Instalar o APK

1. No celular, abra a página de Releases do repositório: `https://github.com/lribeiro770-debug/biodigestor-painel/releases` e procure **Coletor do Biodigestor vX.Y.Z** (tag `coletor-vX.Y.Z`).
2. Toque no arquivo `coletor-biodigestor-vX.Y.Z.apk` para baixar.
3. Abra o arquivo baixado. O Android vai pedir para **permitir instalar apps desconhecidos** a partir do navegador (ou do app Arquivos): toque em *Configurações*, ative *Permitir desta fonte* e volte.
4. Toque em *Instalar*. Se o Play Protect avisar que o app é desconhecido, toque em *Mais detalhes → Instalar mesmo assim* (o APK é gerado pelo GitHub Actions a partir do código deste repositório).

Requisitos: Android 5.1 ou mais novo (a conexão automática à rede precisa de Android 10+).

> **Atualizar:** instale a versão nova por cima. Se o Android disser que o pacote é incompatível/conflita, é porque o APK foi assinado com outra chave (veja "Assinatura" abaixo): **envie os dados pendentes para a nuvem antes**, desinstale e instale de novo — desinstalar apaga os dados locais e o token.

## Primeira configuração

Toque na engrenagem (canto superior direito):

| Campo | O que pôr |
|---|---|
| Nome do coletor | seu nome (vai nas mensagens de commit e no registro do ESP) |
| Endereço do ESP | `http://192.168.4.1` (padrão) |
| Token do ESP | o token de comando do firmware (o mesmo dos comandos MQTT) |
| Nome da rede (SSID) | `Biodigestor-XXXX` aceita qualquer rede que comece com `Biodigestor-`; ou o nome exato, ex. `Biodigestor-54F5` |
| Senha da rede | padrão `biogas123` |
| GitHub: dono / repositório / ramo | `lribeiro770-debug` / `biodigestor-painel` / `main` |
| Token do GitHub | token *fine-grained* (ver abaixo) |

Toque em **Salvar**, depois em **Testar GitHub** (com internet) e, perto do biodigestor, em **Testar ESP**.

### Criar o token do GitHub (fine-grained)

1. No navegador, entre em github.com com uma conta que tenha acesso ao repositório.
2. **Settings → Developer settings → Personal access tokens → Fine-grained tokens → Generate new token**.
3. Nome: `Coletor Android`; validade: a que o grupo preferir.
4. **Resource owner**: `lribeiro770-debug`.
5. **Repository access → Only select repositories → biodigestor-painel**.
6. **Permissions → Repository permissions → Contents: Read and write**. Nada mais.
7. Gere, copie (`github_pat_…`) e cole no app.

Se o dono do repositório for outra pessoa, ela precisa te adicionar como colaborador antes, ou gerar o token ela mesma.

## Procedimento no campo

1. **Antes de sair** (com internet): abra o app e veja se não há envio pendente com erro.
2. Perto do biodigestor, toque em **1 · Conectar à rede do biodigestor** e confirme a rede `Biodigestor-…` na janela do Android. (Ou conecte pelo Wi-Fi do Android e volte ao app.) Se o Android avisar "esta rede não tem acesso à internet", escolha **manter conectado**.
3. Toque em **2 · Coletar agora**. Acompanhe a barra e o registro. Ao final aparece "Coleta concluída: N linha(s) nova(s)". O cartão ESP mostra último `n` no ESP = último `n` coletado.
4. Confira a **Última leitura** e os alarmes.
5. Toque em **Conectar…** de novo (agora "Desconectar da rede do biodigestor") ou simplesmente saia de perto.
6. Com internet (dados móveis ou Wi-Fi), toque em **3 · Enviar para a nuvem** — ou deixe o envio automático fazer isso. O cartão Nuvem deve ficar "em dia".
7. Confira o painel no GitHub Pages (pode levar 1–2 minutos para o Pages atualizar).

## O problema dos dados móveis (e como o app resolve)

Quando o celular está num Wi-Fi **sem internet** (a rede do ESP) e os dados móveis estão ligados, o Android costuma mandar o tráfego dos apps pelos **dados móveis**. Aí `192.168.4.1` fica inalcançável e a coleta falha, mesmo com o Wi-Fi "conectado".

O app tem um plugin nativo próprio (`RedeLocal`, em `android/app/src/main/java/br/edu/utfpr/biodigestor/coletor/RedeLocalPlugin.java`) que:

- **antes de cada coleta/teste do ESP** encontra a rede Wi-Fi e chama `bindProcessToNetwork(wifi)`: todo o tráfego do app passa a sair pelo Wi-Fi do ESP, ignorando os dados móveis;
- **depois** chama `bindProcessToNetwork(null)`, devolvendo o app à rede padrão — assim o envio ao GitHub pode sair pelos dados móveis mesmo com o celular ainda no Wi-Fi do ESP;
- no Android 10+, o botão **Conectar** usa `WifiNetworkSpecifier`: a rede do ESP fica ligada só para o app e o resto do celular continua usando os dados móveis normalmente.

Você não precisa desligar os dados móveis. Se mesmo assim falhar, desligar os dados móveis durante a coleta é o plano B.

### Permissões e para que servem

Todas são "normais" (concedidas na instalação, sem pop-up). O app **não** pede localização, câmera, contatos nem arquivos.

| Permissão | Para quê |
|---|---|
| `INTERNET` | falar com o ESP (HTTP) e com a API do GitHub |
| `ACCESS_NETWORK_STATE` | saber quais redes existem e se o Wi-Fi tem internet (plugin RedeLocal e envio automático) |
| `CHANGE_NETWORK_STATE` | prender o tráfego ao Wi-Fi do ESP (`bindProcessToNetwork`) e pedir a rede do AP (`requestNetwork`) |
| `ACCESS_WIFI_STATE`, `CHANGE_WIFI_STATE` | conexão ao AP `Biodigestor-…` via `WifiNetworkSpecifier` (Android 10+) |

HTTP sem criptografia só é permitido para `192.168.4.1` e `biodigestor.local` (`res/xml/network_security_config.xml`); todo o resto exige HTTPS.

## Problemas comuns

| Sintoma | O que fazer |
|---|---|
| "ESP não alcançável em http://192.168.4.1" | Confirme que está na rede `Biodigestor-…` (faixa no topo: "Wi-Fi sem internet"). Chegue mais perto do ESP. Toque em **Conectar** de novo. Plano B: desligue os dados móveis. |
| A janela do Android para escolher a rede não aparece / "não foi possível conectar" | Confira SSID e senha em Configurações. O app então abre o Wi-Fi do Android: conecte manualmente e volte. |
| "Token do ESP incorreto (401)" | O token do app não bate com o do firmware. Copie o token de comando configurado no ESP. |
| "Versão de protocolo do ESP diferente" | Atualize o app (nova Release) ou o firmware para o protocolo 1. |
| "O ESP não informou a série" | O cartão microSD está ausente ou com defeito. |
| Muitas linhas em `sem_hora.csv` | Foram gravadas depois de uma queda de energia, antes de o ESP saber a hora, sem âncora no mesmo boot. Elas ficam guardadas na nuvem em `dados/sem_hora.csv`. Coletar com frequência (o app acerta a hora a cada coleta) evita isso. |
| "Token do GitHub inválido ou expirado (401)" | Gere um novo token e cole em Configurações. |
| "Sem permissão no GitHub (403)" | O token não tem acesso ao repositório ou falta **Contents: Read and write**. |
| "Repositório ou ramo não encontrado (404)" | Confira dono, nome e ramo; confirme que o token foi criado com acesso a esse repositório. |
| "Limite de requisições do GitHub" | Espere alguns minutos e envie de novo. |
| Envio caiu no meio | Toque em **Enviar** de novo: o envio é idempotente (não duplica linhas). |
| "Atenção: o Android só permite HTTP para 192.168.4.1…" | Você mudou o endereço do ESP para outro IP. Use `http://192.168.4.1` (AP) ou `http://biodigestor.local`. |

## Privacidade do token

- Os tokens (ESP e GitHub) ficam **só neste celular**, na área privada do app (Capacitor Preferences). O backup automático do Android está desligado (`allowBackup="false"`), então eles não vão para a nuvem do Google.
- O token do GitHub só é enviado para `api.github.com` (ou o endereço configurado em *Avançado*); o do ESP só para o ESP.
- Use um token *fine-grained* restrito ao repositório `biodigestor-painel` com **Contents: Read and write** e validade limitada. Se perder o celular, revogue o token em GitHub → Settings → Developer settings.

## Para desenvolvedores

```
app-coletor/
├── package.json, package-lock.json, capacitor.config.json
├── www/                         interface (HTML/CSS/JS puro, sem bundler)
│   ├── index.html, estilo.css
│   └── js/nucleo.js             lógica pura (coleta, CSV, mesclagem, GitHub) — sem Capacitor
│       js/app.js                interface + adaptadores Capacitor (HTTP nativo, Filesystem, Preferences)
├── scripts/copiar-vendor.mjs    copia os bundles do Capacitor para www/vendor e gera www/js/versao.js
├── scripts/gerar_icones.py      gera ícones/splash (Pillow)
├── test/                        node:test + ESP e GitHub falsos em memória
└── android/                     projeto Capacitor Android (com o plugin RedeLocal)
```

- Testes: `npm ci && npm test`. Se o ESP simulado (`coleta/esp_simulado.py`, porta 8088) ou o `github_falso.py` (porta 8089) estiverem no ar, os testes de integração também rodam contra eles; senão são pulados.
- Rodar no navegador (sem os recursos nativos): `npm run vendor && python3 -m http.server -d www 8095` e abra `http://127.0.0.1:8095` (aponte o ESP para `http://127.0.0.1:8088`).
- Gerar APK local: Node 20, JDK 17, Android SDK 34 → `npm ci && npm run sync && cd android && ./gradlew assembleDebug`.
- Versão: edite `version` no `package.json` (vale para o app, `versionName`/`versionCode` e a tag `coletor-vX.Y.Z`). Cada versão nova gera uma Release nova; um push sem mudar a versão reaproveita a mesma Release.
- CI: `.github/workflows/build-apk.yml` roda a cada push em `app-coletor/**` na `main` (ou manualmente) e publica o APK como artefato e na Release.

### Assinatura

O APK é de **depuração** (bom para instalar direto). Sem configuração, cada build do GitHub Actions usa uma chave temporária diferente, e o Android não deixa atualizar por cima de uma versão assinada com outra chave. Para manter sempre a mesma chave, crie uma vez:

```
keytool -genkeypair -v -keystore debug.keystore -storepass android -alias androiddebugkey \
  -keypass android -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=UTFPR,C=BR"
base64 -w0 debug.keystore
```

e cole o resultado como segredo `COLETOR_DEBUG_KEYSTORE_B64` em *Settings → Secrets and variables → Actions* do repositório.
