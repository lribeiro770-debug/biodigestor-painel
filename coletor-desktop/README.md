# Coletor do Biodigestor (notebook)

Programa para o notebook de quem vai até o biodigestor. Ele:

1. conecta na rede Wi-Fi do próprio ESP32 (`Biodigestor-XXXX`), que funciona sem internet;
2. baixa **só as leituras novas** gravadas no cartão microSD (uma por minuto);
3. acerta o relógio do ESP pelo relógio do notebook;
4. guarda tudo no notebook (nada se perde se a coleta for interrompida);
5. quando o notebook tiver internet, envia os dados para o repositório
   `lribeiro770-debug/biodigestor-painel` (pasta `dados/`), de onde o painel web lê os gráficos.

O protocolo está descrito em `../ESPEC_COLETA.md`.

---

## 1. Instalação (Windows)

- Copie `ColetorBiodigestor.exe` para o notebook (por exemplo, na Área de Trabalho) e dê dois cliques.
  Não precisa instalar Python nem mais nada.
- Na primeira vez o Windows pode mostrar "O Windows protegeu o computador" (o programa não é assinado):
  clique em **Mais informações → Executar assim mesmo**.

Para gerar o `.exe` a partir do código (num PC Windows com Python 3.9 ou mais novo, instalado pelo
python.org com a opção *tcl/tk and IDLE*): dê dois cliques em `build_windows.bat`. O resultado sai em
`dist\ColetorBiodigestor.exe`.

## 2. Configuração na primeira vez

Abra o programa e clique em **Configurações**:

| Campo | O que colocar |
|---|---|
| Endereço do ESP | deixe `http://192.168.4.1` |
| Token do ESP | o token de comando do ESP (o mesmo usado nos comandos MQTT); peça ao responsável |
| Rede Wi-Fi do ESP | deixe vazio para achar qualquer `Biodigestor-XXXX`, ou escreva o nome exato (ex.: `Biodigestor-54F5`) |
| Senha da rede Wi-Fi do ESP | `biogas123` (padrão), a menos que tenha sido trocada |
| Dono / Repositório / Ramo | `lribeiro770-debug` / `biodigestor-painel` / `main` |
| Token do GitHub | veja a seção 3 |
| Seu nome | aparece no registro de coletas do ESP e nas mensagens de envio |

Use **Testar ESP** (perto da caixa) e **Testar GitHub** (com internet) para conferir. Depois clique em **Salvar**.

## 3. Como criar o token do GitHub

O token é uma "senha" que só permite gravar arquivos no repositório do painel.

1. Entre em <https://github.com> com a conta **dona** do repositório (`lribeiro770-debug`).
   Tokens *fine-grained* só enxergam repositórios do próprio dono da conta.
2. Clique na sua foto (canto superior direito) → **Settings**.
3. No fim do menu à esquerda: **Developer settings → Personal access tokens → Fine-grained tokens → Generate new token**.
4. *Token name*: `Coletor biodigestor`. *Expiration*: escolha uma data (por exemplo, 1 ano) e anote.
   Quando vencer, é só criar outro e colar no programa.
5. *Repository access*: **Only select repositories** → `biodigestor-painel`.
6. *Permissions → Repository permissions → Contents*: **Read and write**
   (*Metadata: Read-only* é marcado sozinho).
7. **Generate token**, copie o token (começa com `github_pat_`) e cole em **Configurações → Token do GitHub**.
   O GitHub não mostra o token de novo.

O token fica salvo apenas neste notebook (veja a seção 7). Não o envie por mensagem nem o coloque em arquivos do repositório.

## 4. Uso em campo, passo a passo

1. Antes de sair, confira se o relógio do notebook está certo (Configurações do Windows → Hora e idioma →
   *Definir horário automaticamente*). É ele que acerta o relógio do ESP.
2. Chegue a até ~30 m da caixa do biodigestor (mais perto se houver paredes ou metal no caminho).
3. Abra o programa. Em poucos segundos o quadro **Biodigestor (ESP)** deve mostrar *alcançável*:
   - se o notebook já estiver na rede `Biodigestor-XXXX`, ele mostra na hora;
   - se não estiver, clique em **Coletar agora**: o programa procura a rede, conecta sozinho, coleta e
     **volta para a rede anterior** no fim;
   - se a troca automática não funcionar, conecte manualmente pelo ícone de Wi-Fi do Windows na rede
     `Biodigestor-XXXX` (senha `biogas123`) e clique em **Coletar agora** de novo.
4. Acompanhe no **Registro**: "Dados: 2000 linhas recebidas…" até "Coleta concluída". Um mês de dados (~4–5 MB)
   leva de um a poucos minutos, conforme o sinal. Se você precisar sair antes, clique em **Interromper** (ou feche o programa): o que já
   veio fica guardado e a próxima coleta continua de onde parou.
5. A tabela **Últimas leituras guardadas** mostra as leituras mais recentes (temperatura média do
   biodigestor, temperatura e umidade ambiente, pressão do gás e alarmes, em vermelho).
6. Quando o notebook tiver internet (de volta ao laboratório, em casa ou pelo celular), clique em
   **Enviar para a nuvem**. O quadro **Nuvem (GitHub)** passa a mostrar *nada pendente*.
   O painel web atualiza em alguns minutos.

O quadro **Hora do ESP** mostra *ajustada* quando o relógio do ESP foi acertado. Leituras feitas enquanto o
ESP estava sem hora (depois de uma queda de energia, por exemplo) recebem a hora certa automaticamente
quando possível; as que não puderem ser datadas vão para `sem_hora.csv`, sem se perder.

## 5. Modo automático

Marque **Modo automático** e deixe o programa aberto (por exemplo, num notebook que fica perto do
biodigestor parte do dia). A cada 30 segundos ele:

- verifica se a rede do ESP está por perto; se a última coleta tiver mais de **N minutos**
  (Configurações → *Minutos entre coletas*, padrão 60), conecta, coleta e volta para a rede anterior;
- se houver internet e dados pendentes, envia para o GitHub.

Para desligar, desmarque a caixa. A troca de rede pode ser desligada em Configurações
(*Trocar de rede Wi-Fi automaticamente*); aí o modo automático só coleta quando o notebook já estiver na
rede do ESP.

## 6. Problemas comuns

| Mensagem / sintoma | O que fazer |
|---|---|
| **Rede do biodigestor não encontrada** | Aproxime-se da caixa. Confira se o ESP está ligado (LED). Em alguns Windows 11 a lista de redes exige *Serviços de localização* ligados (Configurações → Privacidade → Localização); outra saída é escrever o nome exato da rede em Configurações, que o programa tenta conectar mesmo sem conseguir listar. Ou conecte manualmente. |
| **ESP não alcançável em http://192.168.4.1** | Confira se o Wi-Fi do notebook está mesmo na rede `Biodigestor-XXXX`. Desligue VPN. Alguns antivírus/firewalls bloqueiam programas novos: permita o *ColetorBiodigestor* nas redes privadas e públicas. |
| **O ESP recusou o token (401)** | O token do ESP em Configurações está errado. |
| **Token do GitHub inválido ou expirado (401)** | Crie um token novo (seção 3) e cole em Configurações. |
| **O token não tem permissão (403)** | O token precisa de *Contents: Read and write* no repositório `biodigestor-painel`. |
| **Repositório não encontrado (404)** | Confira dono/repositório em Configurações. Se estiverem certos, o token não inclui esse repositório (ou foi criado em outra conta). |
| **Sem internet** | Os dados continuam guardados no notebook. Envie depois. |
| **Não consegui voltar para a rede …** | Reconecte manualmente à sua rede pelo ícone de Wi-Fi. |
| A hora das leituras parece errada | O relógio do notebook estava errado na coleta. Acerte o relógio e colete de novo: o ESP é reajustado quando a diferença passa de 2 s. |
| Trocaram o cartão microSD | Normal: o programa detecta a série nova do cartão e começa outra numeração, sem misturar com a antiga. |

O registro completo fica em `coletor.log` (seção 7); mande esse arquivo junto ao pedir ajuda (ele não contém os tokens).

## 7. Onde ficam os dados

- **Windows:** `%APPDATA%\ColetorBiodigestor` (cole isso na barra do Explorador de Arquivos), ou clique em
  **Abrir pasta de dados**.
- **Linux/macOS:** `~/.coletor_biodigestor`

Conteúdo:

| Arquivo | O que é |
|---|---|
| `dados/AAAA-MM.csv` | leituras do mês (UTC), no mesmo formato da nuvem |
| `dados/sem_hora.csv` | leituras sem hora conhecida |
| `dados/eventos.csv` | eventos do ESP (partidas, acertos de hora, alarmes, coletas) |
| `estado.json` | até onde já foi coletado e enviado, por série do cartão |
| `config.json` | configurações, **incluindo os tokens** (protegido apenas pela conta do usuário do computador) |
| `coletor.log` | registro das operações |

Os CSV abrem no Excel/LibreOffice (separador vírgula, decimal com ponto). Apagar a pasta não apaga nada do
cartão nem da nuvem: na próxima coleta tudo é baixado de novo, sem duplicar.

## 8. Linux e macOS

Precisa de Python 3.9 ou mais novo com tkinter (no Ubuntu/Debian: `sudo apt install python3-tk`).
Nenhuma outra biblioteca é necessária.

```bash
cd coleta/desktop
python3 coletor.py                 # abre a janela
python3 coletor.py config --definir esp_token=SEU_TOKEN gh_token=github_pat_...
python3 coletor.py coletar         # coleta pela linha de comando
python3 coletor.py enviar          # envia o que estiver pendente
python3 coletor.py status          # situação local, ESP e internet
python3 coletor.py auto            # modo automático no terminal (Ctrl+C para sair)
python3 coletor.py coletar --esp http://192.168.4.1 --sem-wifi --sem-acertar-hora
```

A troca automática de Wi-Fi usa `nmcli` (NetworkManager) no Linux e `networksetup` no macOS (melhor
esforço; no macOS a lista de redes nem sempre está disponível, então vale configurar o nome exato da rede).

## 9. Para desenvolvedores

- `coletor_core.py`: toda a lógica (sem interface), `coletor_gui.py`: janela tkinter, `coletor.py`: entrada e linha de comando.
  Só biblioteca padrão do Python.
- Testes (usam o ESP simulado `../esp_simulado.py` e o GitHub falso `../github_falso.py`, sem internet):

  ```bash
  cd coleta/desktop
  python3 -m unittest -v
  ```
- Para testar a janela contra o simulado: `python3 ../esp_simulado.py --porta 8088` e configure o endereço do
  ESP como `http://127.0.0.1:8088`, token `teste123`. O GitHub falso (`python3 ../github_falso.py`, porta 8089,
  token `teste-gh`) pode ser usado colocando `http://127.0.0.1:8089` em *Endereço da API*.
