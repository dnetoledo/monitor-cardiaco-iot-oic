// =====================================================================
// AD8232 simulado – chip personalizado para o Wokwi (Custom Chips API)
// Projeto: Sistema Inteligente de Monitoramento Cardíaco em Tempo Real
//          com Alerta Sonoro utilizando IoT – Dayane N. de Toledo (OIC/UPM)
//
// O Wokwi não tem o módulo AD8232. Este chip faz o papel dele:
//  - OUTPUT: tensão analógica com o formato típico do ECG (ondas P, QRS e T),
//            em torno de 1,5 V (o AD8232 real usa meia tensão de alimentação
//            como referência).
//  - LO+ / LO-: ficam em nível ALTO quando um eletrodo "solta"
//            (igual ao módulo real, que avisa eletrodo sem contato).
//  - Controle "bpm": define a frequência cardíaca simulada (30 a 200 BPM).
//  - Controle "eletrodo": 0 = conectados, 1 = solta LO+, 2 = solta LO-.
// =====================================================================
#include "wokwi-api.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define PERIODO_US 1000      // a saída é atualizada a cada 1 ms (1 kHz)
#define V_BASE     1.50f     // linha de base do sinal (V)
#define V_SATURADO 3.30f     // saída "colada" no positivo quando o eletrodo solta

typedef struct {
  pin_t pin_out;
  pin_t pin_lo_mais;
  pin_t pin_lo_menos;
  uint32_t attr_bpm;
  uint32_t attr_eletrodo;
  double fase;               // posição dentro do batimento atual (0 a 1)
  double tempo_s;            // tempo de simulação (s), usado na oscilação lenta
  uint32_t ruido;            // estado do gerador de ruído
} chip_state_t;

// Uma "onda" do ECG é desenhada como uma curva em forma de sino (gaussiana)
static float gauss(float t, float centro, float largura, float amplitude) {
  float x = (t - centro) / largura;
  return amplitude * expf(-0.5f * x * x);
}

// Soma das ondas P, Q, R, S e T de um batimento.
// t = tempo em segundos em relação ao pico R; rr = duração do batimento (s)
static float batimento(float t, float rr) {
  float k = sqrtf(rr);       // P e T se aproximam do R quando o coração acelera
  float v = 0.0f;
  v += gauss(t, -0.20f * k, 0.025f,  0.10f);  // onda P
  v += gauss(t, -0.035f,    0.010f, -0.10f);  // onda Q
  v += gauss(t,  0.000f,    0.012f,  1.00f);  // onda R (pico usado pelo firmware)
  v += gauss(t,  0.035f,    0.010f, -0.20f);  // onda S
  v += gauss(t,  0.30f * k, 0.050f,  0.25f);  // onda T
  return v;
}

// Pequeno ruído pseudoaleatório (±10 mV) para o sinal não ficar "perfeito"
static float ruido(chip_state_t *s) {
  s->ruido = s->ruido * 1103515245u + 12345u;
  return (((s->ruido >> 16) & 0x7FFF) / 32767.0f - 0.5f) * 0.02f;
}

// Chamado a cada 1 ms pelo temporizador do chip
static void atualiza(void *user_data) {
  chip_state_t *s = (chip_state_t *)user_data;
  const double dt = PERIODO_US / 1e6;

  uint32_t bpm = attr_read(s->attr_bpm);
  if (bpm < 30) bpm = 30;
  if (bpm > 200) bpm = 200;
  uint32_t eletrodo = attr_read(s->attr_eletrodo);

  float rr = 60.0f / (float)bpm;           // duração de um batimento (s)
  s->fase += dt / rr;                       // avança a fase sem "pular" ao mudar o BPM
  if (s->fase >= 1.0) s->fase -= 1.0;
  s->tempo_s += dt;

  // Detecção de eletrodo solto (leads-off)
  pin_write(s->pin_lo_mais,  eletrodo == 1 ? HIGH : LOW);
  pin_write(s->pin_lo_menos, eletrodo == 2 ? HIGH : LOW);

  if (eletrodo != 0) {
    pin_dac_write(s->pin_out, V_SATURADO);  // sem contato, a saída satura
    return;
  }

  float t = (float)(s->fase * rr);          // tempo desde o último pico R
  float v = V_BASE
          + batimento(t, rr)                // batimento atual
          + batimento(t - rr, rr)           // começo do próximo (onda P)
          + 0.05f * sinf(2.0f * 3.14159265f * 0.25f * (float)s->tempo_s) // respiração
          + ruido(s);

  if (v < 0.0f) v = 0.0f;
  if (v > 3.3f) v = 3.3f;
  pin_dac_write(s->pin_out, v);
}

void chip_init(void) {
  chip_state_t *s = malloc(sizeof(chip_state_t));
  s->pin_out      = pin_init("OUTPUT", ANALOG);
  s->pin_lo_mais  = pin_init("LO+", OUTPUT_LOW);
  s->pin_lo_menos = pin_init("LO-", OUTPUT_LOW);
  pin_init("SDN", INPUT);                   // não conectado (módulo sempre ativo)
  s->attr_bpm      = attr_init("bpm", 75);
  s->attr_eletrodo = attr_init("eletrodo", 0);
  s->fase = 0.5;
  s->tempo_s = 0.0;
  s->ruido = 12345u;

  const timer_config_t cfg = { .callback = atualiza, .user_data = s };
  timer_t timer = timer_init(&cfg);
  timer_start(timer, PERIODO_US, true);

  printf("AD8232 simulado iniciado (BPM inicial: %u)\n", attr_read(s->attr_bpm));
}
