# Painel do Biodigestor · UTFPR Campo Mourão (EEL6006)

Painel web (PWA) para o monitoramento remoto de um biodigestor: temperaturas do digestato,
ambiente (DHT11), gases (MQ-2/MQ-7), pressão/produção de biogás e alarmes.
Os dados chegam do ESP32 via MQTT (WebSocket seguro, porta 8884).

**Acesse:** https://lribeiro770-debug.github.io/biodigestor-painel/

- No celular: abra o link e use "Adicionar à tela inicial" para instalar como app.
- Os comandos (calibrar, silenciar etc.) pedem o token, que é digitado em **Conexão**.
  Ele fica só no seu navegador e nunca vai para este repositório.
