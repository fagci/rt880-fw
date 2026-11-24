#pragma once
#include "mode.h"

extern Mode &MODE_VFO;
extern Mode &MODE_SCANNER;
extern Mode &MODE_ANALYSER;
extern Mode &MODE_FC;
extern Mode &MODE_BC_SCANNER;
extern Mode &MODE_RADIO;
extern Mode &MODE_LOOTLIST;
extern Mode &MODE_CHANNELS;

/* Экран списка находок (loot). Открывается коротким STAR во всех режимах
 * (в редакторе частоты STAR — десятичная точка). EXIT — назад в тот режим,
 * откуда открыли. */
void LOOTLIST_OpenFrom(Mode &from);

/* TEMP: тест помех от дисплея при сканировании 400-470. */
void SCANNER_DebugStart(uint32_t secs, bool mute, uint8_t flags);
