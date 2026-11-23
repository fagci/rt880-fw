#pragma once
#include <stdint.h>

#include "../radio.h"

/* Минимальный снимок состояния VFO: частота, модуляция, шаг, полоса и
 * приёмник (включая ручной выбор). Никакого жизненного цикла:
 * capture/restore делают ровно то, что написано. */
struct VfoSnapshot {
  uint8_t vfo;
  uint32_t freq;
  uint8_t mod;
  uint8_t step;
  uint8_t bw;
  uint8_t radio;
  bool radioManual;
};

inline VfoSnapshot Vfo_Capture(uint8_t vfo = currentVfo) {
  return {vfo,           vfos[vfo].rxF,   (uint8_t)vfos[vfo].modulation,
          (uint8_t)vfos[vfo].step, (uint8_t)vfos[vfo].bw,
          (uint8_t)vfos[vfo].radio, vfos[vfo].radioManual};
}

inline void Vfo_Restore(const VfoSnapshot &s, bool switchToIt = true) {
  if (switchToIt)
    currentVfo = s.vfo;
  VFO *v = &vfos[s.vfo];
  v->rxF = s.freq;
  v->modulation = static_cast<ModulationType>(s.mod);
  v->step = static_cast<Step>(s.step);
  v->bw = s.bw;
  v->radio = static_cast<Radio>(s.radio);
  v->radioManual = s.radioManual;
  if (currentVfo == s.vfo)
    Radio_TuneTo(s.freq, true);
}
