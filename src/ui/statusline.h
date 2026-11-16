#ifndef STATUSLINE_H
#define STATUSLINE_H

void STATUSLINE_ApplyTheme(void); /* пересобрать виджеты в цветах темы */
void STATUSLINE_SetTitle(const char *t); /* левый заголовок (статическая строка) */
void STATUSLINE_render(void);
void STATUSLINE_SetText(const char *pattern, ...);

#endif
