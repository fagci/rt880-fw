#ifndef RT880_W25Q32_H
#define RT880_W25Q32_H

#include <stdbool.h>
#include <stdint.h>

/* Внешняя SPI NOR W25Q32 (U509): 4 МБ, сектор 4 КБ, страница 256 Б.
 * PB12 — CS, PB13/14/15 — SPI2 (AF5). */
#define W25Q_SIZE_BYTES (4u * 1024u * 1024u)
#define W25Q_SECTOR_SIZE 4096u
#define W25Q_PAGE_SIZE 256u

#ifdef __cplusplus
extern "C" {
#endif

/* Инициализация SPI2 и проверка ID. false — чип не ответил. */
bool W25Q_Init(void);
bool W25Q_Present(void);
/* JEDEC ID (0x9F): производитель/тип/объём. 0 — нет связи. */
uint32_t W25Q_JedecId(void);

void W25Q_Read(uint32_t addr, void *dst, uint32_t len);
/* addr выровнен на сектор: стереть 4 КБ, дождаться готовности. */
bool W25Q_SectorErase(uint32_t addr);
/* Программирование страницы: addr и len внутри одной страницы 256 Б,
 * данные пишутся в предварительно стёртую область. */
bool W25Q_PageProgram(uint32_t addr, const void *src, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* RT880_W25Q32_H */
