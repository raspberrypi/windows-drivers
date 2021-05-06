# Broadcom SD Host (BRCME88C) for Raspberry Pi 4

Driver for the eMMC2 host controller (BRCME88C) for Raspberry Pi 4.

This is a miniport - it depends on the in-box sdport.sys to handle the
SD/SDIO/eMMC protocol and WDM interfaces.

Implemented using the
[Microsoft SDHC driver sample](https://github.com/microsoft/Windows-driver-samples/tree/master/sd/miniport/sdhc)
as a reference.

## NOTES

- If both bcm2836sdhc and this driver are loaded and one is unloaded, the
  system will eventually crash. Appears to be a bug in sdport.sys's interrupt
  handling (the Arasan and eMMC2 controllers share an interrupt vector).
  - Possible workaround: Don't use both bcm2836sdhc and this driver.
  - Possible workaround: Don't unload drivers.

## TODO

- Regulator voltage cannot be controlled while in crashdump mode. Possible fix
  would be to have mailbox support via ACPI method instead of via RPIQ driver.
- Does not support UHS-SDR50 because we don't support tuning.
  - Would be easy to implement, but I'm not sure SDR50 is needed.
- Could use some performance testing to determine which transfers should use
  PIO and which transfers should use DMA.
- Needs testing in various scenarios, e.g. crashdump mode.
- Figure out how to get this driver into SafeOS for Windows Update.
