#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stdbool.h>
#include <stdint.h>
#include "misc.h"

#define SP_MAX_POINTS 240
#define WF_XN 120
#define WF_YN 45

typedef struct {
  int16_t min;
  int16_t max;
} DBmRange;

void SP_AddPoint(Loot *msm);
void SP_ResetHistory(void);
void SP_ResetRender(void);
void SP_Init(FRange *r, uint32_t stepSize, uint32_t bw);
void SP_Begin(void);
void SP_Next(void);
void SP_Render(FRange *p, uint8_t sy, uint8_t sh);
void WF_Render(uint8_t y, bool wfDown);
void SP_RenderArrow(FRange *p, uint32_t f, uint8_t sx, uint8_t sy, uint8_t sh);

DBmRange SP_GetGradientRange(void);
uint16_t SP_GetNoiseFloor(void);
uint16_t SP_GetNoiseMax(void);
const uint16_t *SP_GetHistory(void);

#endif
