/*
 * STM32F405RG memory map: 1 MiB flash, 128 KiB SRAM (SRAM1 + SRAM2), 64 KiB CCM.
 *
 * CCM (core-coupled memory) sits on the D-bus with zero wait states and is
 * not reachable by DMA. That makes it the right home for data only the CPU
 * touches in the hot loop: filter state and coefficients (.ccmram).
 *
 * .noinit is not zeroed at reset, so the crash record written by the fault
 * handler survives the SYSRESETREQ and is reported on the next boot.
 */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH  (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
    RAM    (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
    CCMRAM (rw)  : ORIGIN = 0x10000000, LENGTH = 64K
}

_msp_stack_size = 2K; /* ISRs + boot code; tasks have their own static stacks */

SECTIONS
{
    .isr_vector : ALIGN(4)
    {
        KEEP(*(.isr_vector))
    } > FLASH

    .text : ALIGN(4)
    {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } > FLASH

    _sidata = LOADADDR(.data);
    .data : ALIGN(4)
    {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    .noinit (NOLOAD) : ALIGN(4)
    {
        *(.noinit*)
    } > RAM

    .msp_stack (NOLOAD) : ALIGN(8)
    {
        _smsp = .;
        . = . + _msp_stack_size;
        _estack = .;
    } > RAM

    .ccmram (NOLOAD) : ALIGN(4)
    {
        _sccm = .;
        *(.ccmram*)
        . = ALIGN(4);
        _eccm = .;
    } > CCMRAM

    /DISCARD/ : { *(.comment) }
}
