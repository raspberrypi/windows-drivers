# SD Host drivers for Raspberry Pi 4

The Raspberry Pi 4 contains 3 SD Host controllers. The "BCM2711 ARM Peripherals"
datasheet calls them **SDHOST**, **EMMC**, and **EMMC2**.

- **SDHOST** - old, unusual design, uses `bcm2836\rpisdhc` driver (an sdport.sys
  miniport driver). The RPi4 UEFI does not currently expose this device. In
  theory it could be enabled and connected to GPIO pins to run an external SD
  slot.
- **EMMC** - newer controller, supports SD memory and SDIO, uses
  `bcm2836\bcm2836sdhc` driver (an sdport.sys miniport driver). UEFI currently
  exposes this as `ACPI\ARASAN` and connects it to the WiFi SDIO card by
  default.
- **EMMC2** - new for RPi4, supports only SD memory. UEFI currently exposes this
  as `ACPI\BRCME88C` and connects it to the Micro-SD slot by default.

There are two drivers available for the **EMMC2** controller.

- `bcm2711\bcmemmc2` contains configuration files that load the Windows in-box
  `sdbus.sys` driver to run the EMMC2 controller.
  - Advantage: `sdbus.sys` is stable and reliable.
  - Disadvantage: `sdbus.sys` uses more CPU for slower data transfers because it
    can only use PIO (cannot use EMMC2's nonstandard DMA) and can only use 25
    MB/s transfer speeds (cannot switch the Raspberry Pi's voltage regulator to
    the 1.8v needed for UHS signaling).
- `bcm2711\brcme88c` contains the `SDHostBRCME88C.sys` miniport driver that
  works with the Windows in-box `sdport.sys` port driver to run the EMMC2
  controller.
  - Advantage: `SDHostBRCME88C.sys` uses less CPU for faster data transfers
    because it can use DMA and can use 50 MB/s transfer speeds.
  - Disadvantage: `SDHostBRCME88C.sys` is probably less reliable than
    `sdbus.sys`. `SDHostBRCME88C.sys` is new and not yet fully tested, plus
    `sdport.sys` is not as well-maintained by Microsoft and has some known
    issues.

For now, most users will probably want to use the `bcm2711\bcmemmc2` driver
while work continues on testing/improving the `bcm2711\brcme88c` driver.
