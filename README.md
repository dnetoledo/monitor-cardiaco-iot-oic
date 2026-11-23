# Monitor Cardíaco IoT com Alerta Sonoro (ESP32 + MQTT + Node-RED)

Projeto da disciplina **Objetos Inteligentes Conectados** – Análise e Desenvolvimento de Sistemas, Universidade Presbiteriana Mackenzie.
Autora: Dayane Nascimento de Toledo.

O sistema lê o sinal de ECG, calcula a frequência cardíaca (BPM) no próprio ESP32, liga um buzzer quando o BPM fica fora de uma faixa segura por mais de 5 s e publica os dados por **MQTT** (TCP/IP) para um painel no **Node-RED**. O projeto roda no simulador **Wokwi**; o módulo AD8232 é substituído por um chip personalizado que gera o sinal de ECG.

- **Simulação no Wokwi:** https://wokwi.com/projects/481504335028549633
- **Vídeo-demonstração:** _(link será adicionado)_

> Projeto acadêmico. Não é um equipamento médico e não serve para diagnóstico.

## Arquitetura

```
AD8232 simulado ──(GPIO34, LO+ GPIO32, LO– GPIO33)──> ESP32 ──(Wi-Fi, MQTT)──> test.mosquitto.org <──(MQTT)── Node-RED ──> painel
                                                    │                                ▲
                                        buzzer GPIO25 (2.048 Hz)                     └── faixa segura (ecg/dayane/config/faixa)
```

O ESP32 tem Wi-Fi integrado e publica **direto no broker**, sem cabo serial nem computador como gateway (diferente da arquitetura Arduino + Firmata). O Node-RED é apenas mais um cliente do broker: ele assina os tópicos, mostra os dados no painel e publica a faixa segura.

## Arquivos

| Arquivo | Função |
|---|---|
| `sketch.ino` | Firmware do ESP32: amostragem a 250 Hz, alarme, MQTT e medições de tempo |
| `detector_bpm.h` | Detecção de picos R e cálculo do BPM (limiar adaptativo de 60% do máximo dos últimos 2 s, refratário de 250 ms, média dos 5 últimos RR) |
| `ad8232.chip.c` / `ad8232.chip.json` | Chip personalizado do Wokwi que simula o AD8232 (ondas P, QRS e T; controles de BPM e eletrodo solto) |
| `diagram.json` | Ligações do circuito no Wokwi |
| `libraries.txt` | Bibliotecas: PubSubClient e ArduinoJson |
| `oic_monitor_cardiaco_nodered.json` | Fluxo do Node-RED (importar no editor) |

## Hardware

| Componente físico (projeto) | No Wokwi | Ligação |
|---|---|---|
| ESP32-DevKitC (ESP32-WROOM-32) | `board-esp32-devkit-c-v4` | — |
| Módulo AD8232 (SparkFun SEN-12650) + cabo e 3 eletrodos | chip `AD8232 simulado` | OUTPUT→GPIO34 (ADC1_CH6), LO+→GPIO32, LO–→GPIO33, 3.3V→3V3, GND→GND, SDN não conectado |
| Buzzer CEM-1203(42) + transistor BC547 + resistor 1 kΩ + diodo 1N4148 | buzzer piezo | GPIO25 (PWM 2.048 Hz) e GND; no circuito físico o buzzer é alimentado em 5 V pelo transistor |

## Tópicos MQTT (broker `test.mosquitto.org`, porta 1883, MQTT 3.1.1)

| Tópico | Sentido | QoS | Conteúdo |
|---|---|---|---|
| `ecg/dayane/bpm` | ESP32 → broker | 0 | `{"bpm":72}` a cada 2 s |
| `ecg/dayane/alerta` | ESP32 → broker | 0 | `ON` / `OFF` quando muda |
| `ecg/dayane/eletrodos` | ESP32 → broker | 0 | `OK` / `SOLTO` quando muda |
| `ecg/dayane/status` | ESP32 → broker | 0 (retida); LWT com QoS 1 | `online` ao conectar; LWT `offline` se a placa cair |
| `ecg/dayane/config/faixa` | Node-RED → ESP32 | 1 | `{"min":60,"max":100}` |

A biblioteca PubSubClient só publica com QoS 0; o QoS 1 é usado na assinatura da faixa segura e na mensagem de última vontade (LWT). O ESP32 usa um Client ID único (`esp32-dayane-oic-` + parte do MAC) e tenta reconectar a cada 5 s.

## Como reproduzir

**Simulação (ESP32)**
1. Abra o projeto https://wokwi.com/projects/481504335028549633 (ou crie um projeto ESP32 no Wokwi e copie os arquivos `sketch.ino`, `detector_bpm.h`, `diagram.json`, `libraries.txt`, `ad8232.chip.c` e `ad8232.chip.json`).
2. Clique em **▶ Start**. O Monitor Serial mostra a conexão ao Wi-Fi (`Wokwi-GUEST`), ao broker e o BPM a cada 2 s. Se o Wi-Fi ficar preso em "Conectando…", pare e inicie a simulação de novo.
3. Clique no chip **AD8232 simulado** e use os controles: **BPM** (30–200) e **Eletrodo solto** (0 = conectados, 1 = LO+, 2 = LO–).

**Painel (Node-RED)**
1. Instale o Node.js (LTS) e o Node-RED: `npm install -g node-red` (no macOS/Linux pode ser preciso `sudo`; se depois a instalação de módulos falhar por permissão, rode `sudo chown -R $(whoami) ~/.npm`).
2. Rode `node-red` e abra http://127.0.0.1:1880.
3. Em **Menu → Manage palette → Install**, instale `@flowfuse/node-red-dashboard`.
4. Em **Menu → Import**, importe `oic_monitor_cardiaco_nodered.json` e clique em **Deploy**.
5. Abra o painel em http://127.0.0.1:1880/dashboard.

O fluxo tem 5 blocos comentados: (1) BPM, (2) alerta/eletrodos/status, (3) faixa segura, (4) botões de teste que simulam o ESP32 e (5) medição do tempo do atuador.

## Resultados

- **Validação do BPM:** em 50, 75, 100 e 130 BPM configurados no chip, o ESP32 calculou exatamente 50, 75, 100 e 130 (erro absoluto médio de 0 BPM).
- **Alerta:** liga após 5 s fora da faixa (testado em 50 e 130 BPM) e desliga ao voltar; eletrodo solto publica `SOLTO` e suspende o cálculo.

| Nº | Sensor/atuador | Tempo de resposta |
|---|---|---|
| 1–4 | Sensor (publicação do BPM → entrega pelo broker) | 376, 391, 365, 366 ms — **média 374,5 ms** |
| 1–4 | Atuador (comando de faixa no Node-RED → buzzer → alerta ON no Node-RED) | 5.609, 5.620, 5.625, 5.611 ms — **média 5.616,3 ms** |

No atuador, cerca de 5.006 ms são a persistência proposital de 5 s (medida pelo ESP32); a comunicação de ida e volta leva ~610 ms.

**Como medir:** o Monitor Serial mostra `[MEDICAO SENSOR -> broker MQTT]` (o ESP32 assina o próprio tópico de BPM) e `[MEDICAO ATUADOR comando -> buzzer]`. No Node-RED, com o chip em 75 BPM, clique em *MEDIR ATUADOR* (envia a faixa 40–50), espere o alerta, clique em *Restaurar faixa 60–100* e repita.

## Limitações

O sinal simulado não tem ruídos de movimento nem interferência da rede elétrica; os testes avaliam a lógica do sistema, não a aquisição real do ECG. O broker público não tem autenticação nem criptografia, por isso não deve ser usado com dados reais de saúde.
