# Broadcom SD Host (BRCME88C) for Raspberry Pi 4

Driver for the eMMC2 host controller (BRCME88C) for Raspberry Pi 4.

This is a miniport - it depends on the in-box sdport.sys to handle the
SD/SDIO/eMMC protocol and WDM interfaces.

Implemented using the
[Microsoft SDHC driver sample](https://github.com/microsoft/Windows-driver-samples/tree/master/sd/miniport/sdhc)
as a reference.

## TODO

- If both bcm2836sdhc and this driver are loaded and one is unloaded, the
  system will eventually crash. Appears to be a bug in sdport.sys's interrupt
  handling (the Arasan and eMMC2 controllers share an interrupt vector).
- Regulator voltage cannot be controlled while in crashdump mode.
- Tuning? (required to support UHS-SDR50, but I'm not sure if that's useful)
- Testing - make sure the DMA never corrupts data, test in crashdump mode.
- Figure out how to get this driver into SafeOS for Windows Update.
