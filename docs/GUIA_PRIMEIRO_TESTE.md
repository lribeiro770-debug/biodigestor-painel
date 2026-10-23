# Primeiro teste na bancada — coleta offline (firmware 3.0)

Faça este roteiro **antes** de levar o sistema ao biodigestor. Leva uns 30 minutos.

## 0. O que você precisa
- ESP32 com o firmware 3.0 gravado (`biodigestor_v3.0.0_ota.bin` ou o `_completo_0x0.bin`)
- Módulo microSD ligado ao J15 por 6 jumpers (siga os **nomes** impressos no módulo) e o cartão inserido
- Celular Android com o app Coletor instalado (APK do GitHub Releases) **ou** notebook com o ColetorBiodigestor.exe
- O token de comando do ESP (o mesmo do painel) e um token do GitHub (ver passo 6)

## 1. Gravar o firmware
- **Por cima do atual, mantendo configurações:** grave `biodigestor_v3.0.0_ota.bin` em **0x10000** (Flash Download Tool, como antes).
- **Placa nova ou do zero:** grave `biodigestor_v3.0.0_completo_0x0.bin` em **0x0**.
- Depois aperte **EN**. No monitor serial (115200) devem aparecer:
  - `[SD] microSD ok · série xxxxxxxx · amostras …` (se aparecer "cartão não encontrado", confira os jumpers e o cartão em FAT32)
  - `[AP] "Biodigestor-XXXX" ativo em http://192.168.4.1`
  - `[WEB] API de coleta em http://192.168.4.1`

## 2. Ver a página local
1. No celular, conecte na rede **Biodigestor-XXXX** (senha `biogas123`, se você não trocou).
2. Se o Android avisar "rede sem internet", escolha **manter conectado**.
3. Abra `http://192.168.4.1` no navegador: devem aparecer as leituras ao vivo e "Amostras no cartão".

## 3. Testar sem internet (o caso real)
1. Desligue o roteador de casa (ou deixe o ESP sem rede salva) e **reinicie o ESP** (EN).
2. Espere **5 minutos**: o ESP grava 1 linha por minuto. Na página local, "Amostras no cartão" deve subir.
3. A hora vai aparecer como "NÃO acertada". É esperado: sem internet, quem acerta é o coletor.

## 4. Coletar com o app
- **Android:** abra o app → Configurações (URL `http://192.168.4.1`, token do ESP) → **Conectar à rede do biodigestor** → **Coletar agora**.
- **Windows:** abra o ColetorBiodigestor.exe → Configurações → conecte o notebook na rede Biodigestor-XXXX → **Coletar agora**.
- Confira no app: linhas novas baixadas, "hora ajustada", e nenhuma linha perdida (n contínuo).
- Na página local, a hora agora aparece certa. As linhas gravadas antes (sem hora) passam a vir com a hora reconstruída (`ts_ok = 2`).

## 5. Testar queda de energia
1. Tire o ESP da energia por 1 minuto e ligue de novo (sem internet).
2. Espere 3 minutos e colete de novo. As linhas novas chegam sem hora até a coleta acertar o relógio; depois disso, vêm com hora.
3. Linhas de um período em que **ninguém acertou a hora antes de outra queda** ficam em `sem_hora.csv` (limitação conhecida, sem relógio DS3231).

## 6. Enviar para a nuvem
1. Crie o token do GitHub **na conta lribeiro770-debug**: Settings → Developer settings → Personal access tokens → **Fine-grained tokens** → Generate new token → Repository access: **Only select repositories → biodigestor-painel** → Permissions → **Contents: Read and write** → Generate. Copie o token (ele só aparece uma vez).
2. Cole o token nas Configurações do app (fica salvo só naquele aparelho).
3. Com internet de novo, toque em **Enviar para a nuvem**.
4. Abra o painel (https://lribeiro770-debug.github.io/biodigestor-painel/) e escolha **Tudo**: a série coletada aparece, e o aviso diz "mostrando dados coletados".

## 7. Se algo der errado
| Sintoma | O que fazer |
|---|---|
| "cartão não encontrado" no serial | Confira os 6 jumpers pelos nomes; cartão formatado em FAT32; 5V no VCC do módulo |
| Celular não abre 192.168.4.1 | Mantenha a rede sem internet; o app usa "vincular ao Wi-Fi", mas no navegador desligue os dados móveis |
| App diz "token" | O token de comando do ESP está diferente do digitado no app |
| Envio diz 403/404 | O token do GitHub não tem acesso ao repositório biodigestor-painel ou falta "Contents: Read and write" |
| Página local abre mas "Amostras" não sobe | Veja no serial se há "[SD] falha ao gravar"; reinsira o cartão (o ESP tenta de novo a cada minuto) |
| Portal de Wi-Fi | Segure o botão BOOT por 3 s (ou use o botão na página local). Enquanto o portal estiver aberto, a coleta pausa |
