/* Radtel RT-880 — bare metal linker script
 * Flash: 0x08000000, 246K (251904 bytes)
 * RAM:   0x20000000, 37K  (0x9400 bytes)
 * Bootloader занимает первые 0x2AC0 байт Flash
 * Наш код начинается с 0x08002AC0
 *
 * ЕСЛИ пишем с нуля (без стокового bootloader) — стартуем с 0x08000000
 * ЕСЛИ хотим жить рядом со стоковым BL — менять ORIGIN на 0x08002AC0
 */

_estack = 0x20009A68;   /* из стоковой прошивки */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08002800, LENGTH = 246K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 48K
}

ENTRY(Reset_Handler)

SECTIONS
{
    /* Таблица векторов — самое начало Flash */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } >FLASH

    /* Код и константы */
    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } >FLASH

    /* Инициализированные глобальные переменные (хранятся во Flash, копируются в RAM) */
    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } >RAM AT >FLASH

    /* Неинициализированные переменные (обнуляются при старте) */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } >RAM

    /* Куча (heap) */
    .heap (NOLOAD) :
    {
        . = ALIGN(8);
        _sheap = .;
        . = . + 0x400;   /* 1K heap для начала */
        _eheap = .;
    } >RAM

    /* Удалить ненужные секции */
    /DISCARD/ :
    {
        libc.a ( * )
        libm.a ( * )
        libgcc.a ( * )
    }

    .ARM.attributes 0 : { *(.ARM.attributes) }
}