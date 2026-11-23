// =====================================================================
// detector_bpm.h – detecção de picos R e cálculo da frequência cardíaca
//
// Código "puro" em C++ (sem funções do Arduino), para poder ser testado
// também no computador. O firmware chama processa() 250 vezes por segundo.
//
// Método (Seção 2.8 do artigo):
//  1. Remove a linha de base (variações lentas, como a respiração).
//  2. Limiar adaptativo = 60% do maior valor visto nos últimos 2 segundos.
//  3. Quando o sinal passa do limiar, procura o ponto mais alto: é o pico R.
//  4. Período refratário de 250 ms: picos mais próximos que isso são ignorados
//     (evita contar a mesma onda duas vezes).
//  5. BPM = 60.000 / média dos 5 últimos intervalos RR (em ms).
// =====================================================================
#pragma once
#include <stdint.h>

class DetectorBPM {
public:
  static const uint32_t REFRATARIO_MS = 250;   // período refratário
  static const uint32_t SEM_BATIMENTO_MS = 3000; // sem pico R por 3 s -> BPM 0
  static const int N_RR = 5;                   // intervalos usados na média
  static const int N_BLOCOS = 4;               // 4 blocos de 0,5 s = janela de 2 s
  static const uint32_t BLOCO_MS = 500;
  static constexpr float FRACAO_LIMIAR = 0.6f;
  static constexpr float AMPLITUDE_MINIMA = 100.0f; // em unidades do ADC (~80 mV)

  DetectorBPM() { reinicia(); }

  // Zera tudo (usado quando um eletrodo solta)
  void reinicia() {
    iniciado_ = false;
    linhaBase_ = 0;
    for (int i = 0; i < N_BLOCOS; i++) maxBloco_[i] = 0;
    blocoAtual_ = 0;
    inicioBloco_ = 0;
    acimaLimiar_ = false;
    valorPico_ = 0;
    tempoPico_ = 0;
    ultimoPico_ = 0;
    temPico_ = false;
    nRR_ = 0;
    posRR_ = 0;
    bpm_ = -1;          // -1 = ainda sem valor
  }

  // Recebe uma amostra do ADC (0 a 4095) e o instante em ms.
  // Retorna true quando um novo pico R foi confirmado.
  bool processa(int amostra, uint32_t agoraMs) {
    if (!iniciado_) {
      linhaBase_ = (float)amostra;
      inicioBloco_ = agoraMs;
      ultimoPico_ = agoraMs;
      iniciado_ = true;
    }

    // 1) Linha de base por média móvel exponencial (constante de ~0,4 s)
    linhaBase_ += 0.01f * ((float)amostra - linhaBase_);
    float y = (float)amostra - linhaBase_;

    // 2) Maior valor dos últimos 2 s (4 blocos de 0,5 s)
    if (agoraMs - inicioBloco_ >= BLOCO_MS) {
      blocoAtual_ = (blocoAtual_ + 1) % N_BLOCOS;
      maxBloco_[blocoAtual_] = 0;
      inicioBloco_ = agoraMs;
    }
    if (y > maxBloco_[blocoAtual_]) maxBloco_[blocoAtual_] = y;
    float maxJanela = 0;
    for (int i = 0; i < N_BLOCOS; i++)
      if (maxBloco_[i] > maxJanela) maxJanela = maxBloco_[i];
    float limiar = FRACAO_LIMIAR * maxJanela;

    bool novoPico = false;
    if (maxJanela >= AMPLITUDE_MINIMA) {
      // 3) Procura o ponto mais alto enquanto o sinal está acima do limiar
      if (y > limiar) {
        if (!acimaLimiar_ || y > valorPico_) {
          valorPico_ = y;
          tempoPico_ = agoraMs;
        }
        acimaLimiar_ = true;
      } else if (acimaLimiar_) {
        acimaLimiar_ = false;
        // 4) Período refratário
        if (!temPico_ || tempoPico_ - ultimoPico_ >= REFRATARIO_MS) {
          registraPico(tempoPico_);
          novoPico = true;
        }
      }
    }

    // Sem nenhum pico R por 3 s: considera frequência 0 (situação de alerta)
    if (agoraMs - ultimoPico_ > SEM_BATIMENTO_MS) {
      nRR_ = 0;
      posRR_ = 0;
      temPico_ = false;
      bpm_ = 0;
      ultimoPico_ = agoraMs;   // evita repetir a cada amostra
    }
    return novoPico;
  }

  // BPM atual (-1 enquanto não há intervalos suficientes)
  float bpm() const { return bpm_; }
  bool valido() const { return bpm_ >= 0; }

private:
  void registraPico(uint32_t t) {
    if (temPico_) {
      uint32_t rr = t - ultimoPico_;
      if (rr > 2500) {            // intervalo longo demais: recomeça a média
        nRR_ = 0;
        posRR_ = 0;
      } else {
        rr_[posRR_] = rr;
        posRR_ = (posRR_ + 1) % N_RR;
        if (nRR_ < N_RR) nRR_++;
        // 5) BPM = 60.000 / média dos últimos intervalos RR
        if (nRR_ >= 2) {
          uint32_t soma = 0;
          for (int i = 0; i < nRR_; i++) soma += rr_[i];
          bpm_ = 60000.0f / ((float)soma / nRR_);
        }
      }
    }
    ultimoPico_ = t;
    temPico_ = true;
  }

  bool iniciado_;
  float linhaBase_;
  float maxBloco_[N_BLOCOS];
  int blocoAtual_;
  uint32_t inicioBloco_;
  bool acimaLimiar_;
  float valorPico_;
  uint32_t tempoPico_;
  uint32_t ultimoPico_;
  bool temPico_;
  uint32_t rr_[N_RR];
  int nRR_;
  int posRR_;
  volatile float bpm_;
};
