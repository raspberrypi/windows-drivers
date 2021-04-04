# Broadcom SD Host (BRCME88C) for Raspberry Pi 4

Driver for the eMMC2 host controller (BRCME88C) for Raspberry Pi 4.

This is a miniport - it depends on the in-box sdport.sys to handle the
SD/SDIO/eMMC protocol and WDM interfaces.

Implemented using the
[Microsoft SDHC driver sample](https://github.com/microsoft/Windows-driver-samples/tree/master/sd/miniport/sdhc)
as a reference.

## TODO

- Regulator voltage should be set to 3.3v during ResetHw, but that leads to
  deadlock with current RPIQ driver. This means the driver will be incompatible
  with some SD cards that are UHS-capabile but not LV-compliant.
- DMA. (via a bounce buffer)
- Driver crashes if unloaded. Appears to be a bug in sdport.sys.
- Tuning? (required to support UHS-SDR50)
