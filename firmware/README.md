# Firmware do biodigestor · ESP32 (v3.0.8)

Firmware do ESP32 DevKit V1 (placa v3.1-FS) do projeto de monitoramento remoto do biodigestor
(UTFPR Campo Mourão · EEL6006).

| Pasta | Conteúdo |
|---|---|
| `biodigestor_monitor/` | código-fonte (`.ino`, `config.h`, `coleta.h`, `partitions.csv`) |
| `binarios/` | imagens prontas para gravar + `SHA256SUMS.txt` |

## Gravar (Flash Download Tool ou esptool)

| Situação | Arquivo | Endereço |
|---|---|---|
| Atualizar **mantendo** Wi-Fi, token e calibrações (OTA) | `biodigestor_v3.0.8_ota_0x10000.bin` | **0x10000** |
| Placa nova / do zero (**apaga** os ajustes) | `biodigestor_v3.0.8_completo_0x0.bin` | **0x0** |

```
esptool.py --chip esp32 -b 460800 write_flash 0x10000 biodigestor_v3.0.8_ota_0x10000.bin
```

A imagem de 0x10000 troca só o programa. NVS (Wi-Fi, token, R0 dos MQ, zero da pressão) e
LittleFS (fila offline e diário) ficam preservados.

## Compilar

- Arduino-ESP32 **3.1.3**, placa *ESP32 Dev Module*, esquema de partição **custom**
  (usa a `partitions.csv` da pasta do sketch: 2 apps OTA de 1,625 MB + 640 kB LittleFS).
- Bibliotecas:

| Biblioteca | Versão usada |
|---|---|
| ArduinoJson | 7.2.1 |
| DHTesp | 1.19 |
| DallasTemperature | 3.9.0 |
| OneWire | 2.3.8 |
| PubSubClient | 2.8 |
| WiFiManager | 2.0.17 |

## Pinagem (resumo)

DS18B20 ×2 → GPIO4 · DHT11 → GPIO16 · MQ-2 → GPIO36 e MQ-7 → GPIO39 (divisor 10k/10k) ·
HX710B OUT → GPIO13, SCK → GPIO14 · relé → GPIO26 · buzzer → GPIO27 (BC337) ·
SD (HSPI): MISO 34, CS 32, SCK 33, MOSI 25.

## Segurança: troque os valores padrão

O `config.h` traz só **valores padrão de fábrica** (senha do AP `biogas123` e token
`troque-este-token`). Como este repositório é público, **troque os dois no dispositivo**
pelo monitor Serial (115200) — ficam gravados na flash, sem recompilar:

```
set:ap_pass=<nova senha com 8+ caracteres>
set:token=<novo token>
```

Nenhuma credencial real (Wi-Fi, MQTT, Telegram) está no código: elas são configuradas em campo
pelo portal ou pelo Serial.
