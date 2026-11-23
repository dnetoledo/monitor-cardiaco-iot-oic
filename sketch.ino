// =====================================================================
// Sistema Inteligente de Monitoramento Cardíaco em Tempo Real
// com Alerta Sonoro utilizando IoT
// Dayane Nascimento de Toledo – Objetos Inteligentes Conectados (UPM)
//
// Plataforma: ESP32-DevKitC (simulado no Wokwi)
// Sensor:     AD8232 (chip personalizado "AD8232 simulado")
// Atuador:    buzzer no GPIO25 (onda de 2.048 Hz)
// Internet:   Wi-Fi + MQTT 3.1.1 no broker test.mosquitto.org (porta 1883)
//
// O que o programa faz:
//  1. Lê o sinal do ECG 250 vezes por segundo (temporizador esp_timer).
//  2. Detecta os picos R e calcula o BPM (arquivo detector_bpm.h).
//  3. Se o BPM ficar fora da faixa segura por 5 s, liga o buzzer.
//  4. Publica BPM, alerta, eletrodos e status nos tópicos ecg/dayane/...
//  5. Recebe uma nova faixa segura em ecg/dayane/config/faixa.
//  6. Mede os tempos de resposta pedidos na entrega final (linhas [MEDICAO]).
// =====================================================================
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "esp_timer.h"
#include "detector_bpm.h"

// ---------------- Pinos (Tabela 2 do artigo) ----------------
const int PIN_ECG      = 34;   // OUTPUT do AD8232 -> ADC1_CH6
const int PIN_LO_MAIS  = 32;   // LO+
const int PIN_LO_MENOS = 33;   // LO-
const int PIN_BUZZER   = 25;   // buzzer (PWM)

// ---------------- Rede e MQTT ----------------
const char *WIFI_REDE  = "Wokwi-GUEST";   // rede virtual do Wokwi (sem senha)
const char *WIFI_SENHA = "";
const char *MQTT_BROKER = "test.mosquitto.org";
const int   MQTT_PORTA  = 1883;

const char *TOPICO_BPM       = "ecg/dayane/bpm";
const char *TOPICO_ALERTA    = "ecg/dayane/alerta";
const char *TOPICO_ELETRODOS = "ecg/dayane/eletrodos";
const char *TOPICO_STATUS    = "ecg/dayane/status";
const char *TOPICO_FAIXA     = "ecg/dayane/config/faixa";

// ---------------- Parâmetros do método (Seção 2.8) ----------------
const uint32_t PERIODO_AMOSTRA_US = 4000;  // 250 Hz
const uint32_t INTERVALO_BPM_MS   = 2000;  // publica o BPM a cada 2 s
const uint32_t TEMPO_ALARME_MS    = 5000;  // 5 s fora da faixa para alarmar
const int      FREQ_BUZZER_HZ     = 2048;  // frequência nominal do CEM-1203(42)

// Mude para 1 para ver o sinal bruto no Monitor Serial (1 a cada 10 amostras)
#define MOSTRAR_SINAL 0

// ---------------- Estado do sistema ----------------
WiFiClient wifi;
PubSubClient mqtt(wifi);
DetectorBPM detector;
String clientId;

volatile bool eletrodoSolto = false;   // atualizado na rotina de amostragem
bool eletrodoSoltoPublicado = false;   // último estado enviado ao broker
bool eletrodoJaPublicado = false;

int faixaMin = 60;                     // faixa segura padrão
int faixaMax = 100;

bool alarmeLigado = false;
uint32_t foraDaFaixaDesde = 0;         // 0 = BPM dentro da faixa
uint32_t ultimoEnvioBpm = 0;
uint32_t ultimaTentativaMqtt = 0;

// ---------------- Medições de tempo (item 7 da entrega) ----------------
// Sensor:  tempo entre publicar o BPM e o broker entregá-lo de volta
//          (o ESP32 assina o próprio tópico e mede a ida e volta).
// Atuador: tempo entre receber o comando de nova faixa e ligar o buzzer
//          (inclui os 5 s de persistência exigidos pelo método).
const int MAX_MEDICOES = 4;
bool aguardandoEco = false;
uint32_t instantePublicacao = 0;
char ultimoPayloadBpm[24] = "";
uint32_t medSensor[MAX_MEDICOES];
int nMedSensor = 0;

uint32_t instanteComando = 0;          // 0 = nenhum comando pendente
uint32_t medAtuador[MAX_MEDICOES];
int nMedAtuador = 0;

// Imprime uma medição e a média das últimas (até 4)
void registraMedicao(const char *nome, uint32_t *lista, int &n, uint32_t valor) {
  if (n < MAX_MEDICOES) {
    lista[n++] = valor;
  } else {                              // mantém só as 4 mais recentes
    for (int i = 1; i < MAX_MEDICOES; i++) lista[i - 1] = lista[i];
    lista[MAX_MEDICOES - 1] = valor;
  }
  float soma = 0;
  for (int i = 0; i < n; i++) soma += lista[i];
  Serial.printf("[MEDICAO %s] %lu ms | media das ultimas %d: %.1f ms\n",
                nome, (unsigned long)valor, n, soma / n);
}

// =====================================================================
// 1) Amostragem a 250 Hz (chamada pelo temporizador esp_timer)
// =====================================================================
void amostraEcg(void *) {
  bool solto = digitalRead(PIN_LO_MAIS) == HIGH || digitalRead(PIN_LO_MENOS) == HIGH;
  eletrodoSolto = solto;
  if (solto) {                 // leitura inválida: suspende o cálculo
    detector.reinicia();
    return;
  }
  int valor = analogRead(PIN_ECG);
  detector.processa(valor, millis());

#if MOSTRAR_SINAL
  static int conta = 0;
  if (++conta >= 10) { conta = 0; Serial.println(valor); }
#endif
}

// =====================================================================
// 2) Mensagens recebidas do broker
// =====================================================================
void aoReceberMensagem(char *topico, byte *payload, unsigned int tamanho) {
  char texto[128];
  unsigned int n = tamanho < sizeof(texto) - 1 ? tamanho : sizeof(texto) - 1;
  memcpy(texto, payload, n);
  texto[n] = '\0';

  // Nova faixa segura, ex.: {"min":60,"max":100}
  if (strcmp(topico, TOPICO_FAIXA) == 0) {
    JsonDocument doc;
    if (deserializeJson(doc, texto)) {
      Serial.printf("Faixa ignorada (JSON invalido): %s\n", texto);
      return;
    }
    int novoMin = doc["min"] | -1;
    int novoMax = doc["max"] | -1;
    if (novoMin < 30 || novoMax > 220 || novoMin >= novoMax) {
      Serial.printf("Faixa ignorada (valores invalidos): %s\n", texto);
      return;
    }
    faixaMin = novoMin;
    faixaMax = novoMax;
    foraDaFaixaDesde = 0;        // recomeça a contagem dos 5 s com a nova faixa
    instanteComando = millis();  // início da medição do atuador
    Serial.printf("Nova faixa segura: %d a %d BPM\n", faixaMin, faixaMax);
    return;
  }

  // Eco do próprio BPM: mede o tempo até o broker entregar a mensagem
  if (strcmp(topico, TOPICO_BPM) == 0 && aguardandoEco &&
      strcmp(texto, ultimoPayloadBpm) == 0) {
    aguardandoEco = false;
    registraMedicao("SENSOR -> broker MQTT", medSensor, nMedSensor,
                    millis() - instantePublicacao);
  }
}

// =====================================================================
// 3) Conexões
// =====================================================================
void conectaWifi() {
  Serial.print("Conectando ao Wi-Fi");
  WiFi.begin(WIFI_REDE, WIFI_SENHA, 6);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.printf("\nWi-Fi conectado. IP: %s\n", WiFi.localIP().toString().c_str());
}

void publicaEletrodos() {
  bool solto = eletrodoSolto;
  if (mqtt.publish(TOPICO_ELETRODOS, solto ? "SOLTO" : "OK")) {
    eletrodoSoltoPublicado = solto;
    eletrodoJaPublicado = true;
    Serial.printf("Eletrodos: %s\n", solto ? "SOLTO" : "OK");
  }
}

// Tenta conectar sem travar o programa (nova tentativa a cada 5 s)
void verificaMqtt() {
  if (mqtt.connected()) return;
  if (millis() - ultimaTentativaMqtt < 5000 && ultimaTentativaMqtt != 0) return;
  ultimaTentativaMqtt = millis();

  Serial.printf("Conectando ao broker %s como %s... ", MQTT_BROKER, clientId.c_str());
  // Última vontade (LWT): se a placa cair, o broker publica "offline" (retida)
  if (mqtt.connect(clientId.c_str(), nullptr, nullptr,
                   TOPICO_STATUS, 1, true, "offline")) {
    Serial.println("conectado!");
    mqtt.publish(TOPICO_STATUS, "online", true);   // mensagem retida
    mqtt.subscribe(TOPICO_FAIXA, 1);
    mqtt.subscribe(TOPICO_BPM, 0);                 // para medir o tempo do sensor
    publicaEletrodos();
    mqtt.publish(TOPICO_ALERTA, alarmeLigado ? "ON" : "OFF");
  } else {
    Serial.printf("falhou (codigo %d). Nova tentativa em 5 s.\n", mqtt.state());
  }
}

// =====================================================================
// 4) Alarme: liga o buzzer após 5 s fora da faixa
// =====================================================================
void ligaAlarme(bool ligar) {
  if (ligar == alarmeLigado) return;
  alarmeLigado = ligar;
  if (ligar) {
    tone(PIN_BUZZER, FREQ_BUZZER_HZ);   // onda quadrada de 2.048 Hz (LEDC)
    if (instanteComando != 0) {
      registraMedicao("ATUADOR comando -> buzzer", medAtuador, nMedAtuador,
                      millis() - instanteComando);
      instanteComando = 0;
    }
  } else {
    noTone(PIN_BUZZER);
  }
  mqtt.publish(TOPICO_ALERTA, ligar ? "ON" : "OFF");
  Serial.printf("ALERTA %s\n", ligar ? "LIGADO" : "DESLIGADO");
}

void verificaAlarme() {
  if (eletrodoSolto || !detector.valido()) {   // sem leitura confiável
    foraDaFaixaDesde = 0;
    ligaAlarme(false);
    return;
  }
  float bpm = detector.bpm();
  bool fora = bpm < faixaMin || bpm > faixaMax;
  if (!fora) {
    foraDaFaixaDesde = 0;
    ligaAlarme(false);
    return;
  }
  if (foraDaFaixaDesde == 0) foraDaFaixaDesde = millis();
  if (millis() - foraDaFaixaDesde >= TEMPO_ALARME_MS) ligaAlarme(true);
}

// =====================================================================
// 5) Publicação periódica do BPM
// =====================================================================
void publicaBpm() {
  if (millis() - ultimoEnvioBpm < INTERVALO_BPM_MS) return;
  ultimoEnvioBpm = millis();
  if (eletrodoSolto || !detector.valido() || !mqtt.connected()) return;

  int bpm = (int)lroundf(detector.bpm());
  snprintf(ultimoPayloadBpm, sizeof(ultimoPayloadBpm), "{\"bpm\":%d}", bpm);
  instantePublicacao = millis();
  if (mqtt.publish(TOPICO_BPM, ultimoPayloadBpm)) {
    aguardandoEco = true;
    Serial.printf("BPM: %d (faixa %d-%d)\n", bpm, faixaMin, faixaMax);
  }
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LO_MAIS, INPUT);
  pinMode(PIN_LO_MENOS, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_ECG, ADC_11db);   // leitura de 0 a ~3,3 V

  // Client ID único (parte do endereço MAC da placa)
  char id[32];
  snprintf(id, sizeof(id), "esp32-dayane-oic-%06lX",
           (unsigned long)(ESP.getEfuseMac() & 0xFFFFFF));
  clientId = id;

  conectaWifi();
  mqtt.setServer(MQTT_BROKER, MQTT_PORTA);
  mqtt.setCallback(aoReceberMensagem);

  // Temporizador de 4 ms (250 Hz) para a amostragem do ECG
  const esp_timer_create_args_t args = {
    .callback = &amostraEcg,
    .arg = nullptr,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "amostra_ecg",
    .skip_unhandled_events = true,
  };
  esp_timer_handle_t timer;
  esp_timer_create(&args, &timer);
  esp_timer_start_periodic(timer, PERIODO_AMOSTRA_US);

  Serial.println("Monitor cardiaco iniciado. Faixa padrao: 60-100 BPM.");
}

void loop() {
  verificaMqtt();
  mqtt.loop();

  // Publica os eletrodos só quando o estado muda
  if (mqtt.connected() &&
      (!eletrodoJaPublicado || eletrodoSolto != eletrodoSoltoPublicado)) {
    publicaEletrodos();
  }

  publicaBpm();
  verificaAlarme();
  delay(5);
}
