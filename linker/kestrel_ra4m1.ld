/* Kestrel-RT linker script for the R7FA4M1AB on an Arduino UNO R4 WiFi.
 *
 * Flash 0x00000000-0x00003FFF belongs to the Arduino bootloader and the option-setting memory
 * (OFS0/OFS1/SECMPU at 0x400). Kestrel starts at 0x4000, where the bootloader's boot5() expects a
 * vector table (initial MSP in 0x20xxxxxx, reset handler next). scripts/check_image.py fails the
 * build if any loadable byte falls below 0x4000.
 *
 * SRAM layout (32 KB), chosen so MPU regions can cover whole power-of-two, size-aligned blocks:
 *   0x20000000 .data/.bss (kernel, privileged only)
 *   ...        .task_stacks (each stack power-of-two sized and aligned to its size)
 *   ...        .app_shared (one 2 KB aligned block, RW for unprivileged tasks)
 *   0x20007800 main stack (MSP, 2 KB) with a 32 B no-access MPU guard at its bottom
 * No heap exists. Any reference to malloc/_sbrk fails the link (-nostdlib, no libc).
 */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00004000, LENGTH = 0x0003C000
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 0x00008000
}

KESTREL_MAIN_STACK_SIZE = 0x800;
KESTREL_APP_SHARED_SIZE = 0x800;

ENTRY(kestrel_reset_handler)

SECTIONS
{
  .vectors ORIGIN(FLASH) :
  {
    __vectors_start = .;
    KEEP(*(.vectors))
    __vectors_end = .;
  } > FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text .text.*)
    *(.glue_7 .glue_7t .vfp11_veneer .v4_bx)
  } > FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata .rodata.*)
    . = ALIGN(4);
  } > FLASH

  .ARM.exidx :
  {
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
  } > FLASH

  __data_load = LOADADDR(.data);
  .data : ALIGN(4)
  {
    __data_start = .;
    *(.data .data.*)
    . = ALIGN(4);
    __data_end = .;
  } > RAM AT > FLASH

  .bss (NOLOAD) : ALIGN(4)
  {
    __bss_start = .;
    *(.bss .bss.* COMMON)
    . = ALIGN(4);
    __bss_end = .;
  } > RAM

  /* Task stacks: zeroed by startup, then filled with the watermark pattern by the kernel. */
  .task_stacks (NOLOAD) : ALIGN(32)
  {
    __task_stacks_start = .;
    *(.task_stacks .task_stacks.*)
    __task_stacks_end = .;
  } > RAM

  /* Shared application data is initialized like .data (copied from flash by startup); the rest of
   * its 2 KB MPU block is reserved by the NOLOAD pad section right after it. */
  .app_shared : ALIGN(KESTREL_APP_SHARED_SIZE)
  {
    __app_shared_start = .;
    *(.app_shared .app_shared.*)
    . = ALIGN(4);
    __app_shared_used_end = .;
  } > RAM AT > FLASH
  __app_shared_load = LOADADDR(.app_shared);

  .app_shared_pad (NOLOAD) :
  {
    . = __app_shared_start + KESTREL_APP_SHARED_SIZE;
    __app_shared_end = .;
  } > RAM

  .main_stack ORIGIN(RAM) + LENGTH(RAM) - KESTREL_MAIN_STACK_SIZE (NOLOAD) :
  {
    __main_stack_bottom = .;
    . = . + KESTREL_MAIN_STACK_SIZE;
    __main_stack_top = .;
  } > RAM

  /DISCARD/ : { *(.init) *(.fini) *(.eh_frame*) }
}

ASSERT(__app_shared_used_end <= __app_shared_end, "app_shared overflow: raise KESTREL_APP_SHARED_SIZE")
ASSERT(__app_shared_end <= __main_stack_bottom, "RAM overflow: task stacks + shared collide with the main stack")
ASSERT(__vectors_start == 0x4000, "vector table must sit at the bootloader's application origin 0x4000")
ASSERT((__main_stack_top & 0x7) == 0, "initial MSP must be 8-byte aligned")
