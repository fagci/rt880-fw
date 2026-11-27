#ifndef RT880_BOOT_LAYOUT_H
#define RT880_BOOT_LAYOUT_H

/* Раскладка внутренней flash RT-880 (AT32F423, 256 КиБ).
 *
 *  0x08000000 .. 0x080027FF  стоковый загрузчик (10 КиБ) — не трогаем,
 *                            recovery по PTT, 115200, 246 кадров.
 *  0x08002800 .. 0x080047FF  stage-2 (8 КиБ): fast-прошивка 921600.
 *  0x08004800 .. 0x0803F7FF  приложение.
 *  0x0803F800 .. 0x0803FFFF  метаданные stage-2 (CRC/длина app).
 */

#define STOCK_BOOT_BASE   0x08000000u
#define STOCK_BOOT_LEN    0x2800u

#define BOOT2_BASE        0x08002800u
#define BOOT2_LEN         0x2000u

#define APP_BASE          (BOOT2_BASE + BOOT2_LEN)
#define APP_FLASH_END     0x08040000u

#define META_ADDR         0x0803F800u
#define META_LEN          0x800u
#define APP_MAX_LEN       (META_ADDR - APP_BASE)

#define META_MAGIC        0x42324D45u  /* "B2ME" */

/* SRAM выше стека приложения (0x2000B000) и стека стокового загрузчика
 * (0x20000A80); startup-ы её не обнуляют. */
#define BOOT2_MAGIC_ADDR  0x2000BFF0u
#define BOOT2_MAGIC       0xB0072002u

#define B2_BAUD           921600u
#define B2_BLOCK          2048u

#endif /* RT880_BOOT_LAYOUT_H */
