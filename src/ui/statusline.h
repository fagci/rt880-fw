#ifndef STATUSLINE_H
#define STATUSLINE_H

void STATUSLINE_ApplyTheme(void); /* цвета под gInverted */
void STATUSLINE_render(void);
void STATUSLINE_SetText(const char *pattern, ...);

#endif
