#pragma once
#include "mode.h"

/* Экран списка находок (loot). Открывается из сканера (MENU длинный) и
 * из VFO (длинный 8). EXIT — назад в тот режим, откуда открыли. */
extern Mode &MODE_LOOTLIST;

void LOOTLIST_OpenFrom(Mode &from);
