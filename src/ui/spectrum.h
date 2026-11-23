#ifndef SPECTRUM_H
#define SPECTRUM_H

#include "misc.h"
#include <stdbool.h>
#include <stdint.h>

#define SP_MAX_POINTS 240

typedef struct {
  int16_t min;
  int16_t max;
} DBmRange;

void SP_AddPoint(Measurement *msm);
void SP_ResetHistory(void);
void SP_ResetLevel(void); /* забыть опору уровня (вход в режим/другой тракт) */
void SP_ResetPeaks(void); /* сбросить peak hold (оставить текущий кадр) */
void SP_Invalidate(uint8_t sh); /* следующий SP_Render перерисует всю область */
void SP_Init(FRange *r, uint32_t stepSize, uint32_t bw);
void SP_Begin(void);
void SP_Render(FRange *p, uint8_t sy, uint8_t sh);
void WF_Render(uint8_t y, bool wfDown);
void WF_Reset(void);
void WF_Clear(uint8_t y); /* залить зону водопада цветом пола, не чёрным */
void SP_RenderArrow(FRange *p, uint32_t f, uint8_t sx, uint8_t sy, uint8_t sh);

#endif
