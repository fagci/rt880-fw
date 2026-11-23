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

/* Экран списка находок (loot). Открывается из сканера (MENU длинный) и
 * из VFO (длинный 8). EXIT — назад в тот режим, откуда открыли. */
void LOOTLIST_OpenFrom(Mode &from);
