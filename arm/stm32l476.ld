ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM);

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 96K
}

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(128);
        KEEP(*(.isr_vector))
    } > FLASH

        .text :
    {
        . = ALIGN(4);
        __text_start__ = .;
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        . = ALIGN(4);
        __text_end__ = .;
    } > FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .ARM.extab :
    {
        *(.ARM.extab*)
    } > FLASH

    .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH

    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) :
    {
        . = ALIGN(8);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        _ebss = .;
    } > RAM

    /DISCARD/ :
    {
        *(.comment*)
        *(.note*)
    }
}

ASSERT((_ebss + 0x800) <= _estack,
       "RAM overflow: reserve at least 2 KiB for the main stack")
