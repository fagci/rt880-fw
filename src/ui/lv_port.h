#ifndef LV_PORT_H
#define LV_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void lv_port_init(void);
void lv_port_stats_begin(void);
void lv_port_stats(uint32_t *calls, uint32_t *px, uint32_t *flushMs,
                   uint32_t *pre);

#ifdef __cplusplus
}
#endif

#endif
