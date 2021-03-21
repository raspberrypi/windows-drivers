# Broadcom SD Host (BRCME88C) for Raspberry Pi 4

Driver for the eMMC2 host controller (BRCME88C) for Raspberry Pi 4.

Based on the
[Microsoft SDHC driver sample](https://github.com/microsoft/Windows-driver-samples/tree/master/sd/miniport/sdhc).

This is an sdport miniport - it depends on the in-box sdport.sys to handle the
SD/SDIO/eMMC protocol and WDM interfaces.

## TODO

- Support higher-speed modes.
- DMA.
