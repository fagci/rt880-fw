#ifndef LV_PORT_H
#define LV_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void lv_port_init(void);
void lv_port_stats(uint32_t *calls, uint32_t *px, uint32_t *flushMs); /* и сброс */

#ifdef __cplusplus
}
#endif

#endif
