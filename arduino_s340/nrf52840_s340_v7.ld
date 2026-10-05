/* Linker script for nRF52840 + ANT SoftDevice S340 v7.0.1 (Adafruit UF2 bootloader). */

SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
  /* MBR + S340 7.0.1 occupy 0x00000-0x31000 (SD_FLASH_SIZE 0x30000 + MBR 0x1000).
   * Bootloader starts at 0xF4000, 0xED000-0xF4000 is reserved for InternalFS. */
  FLASH (rx)     : ORIGIN = 0x31000, LENGTH = 0xED000 - 0x31000

  /* SRAM required by SoftDevice (BLE config of Bluefruit). Left with extra
   * headroom compared to S140 (0x20006000) since S340 also runs ANT. */
  RAM (rwx) :  ORIGIN = 0x20008000, LENGTH = 0x20040000 - 0x20008000
}

SECTIONS
{
  . = ALIGN(4);
  .svc_data :
  {
    PROVIDE(__start_svc_data = .);
    KEEP(*(.svc_data))
    PROVIDE(__stop_svc_data = .);
  } > RAM

  .fs_data :
  {
    PROVIDE(__start_fs_data = .);
    KEEP(*(.fs_data))
    PROVIDE(__stop_fs_data = .);
  } > RAM
} INSERT AFTER .data;

INCLUDE "nrf52_common.ld"
