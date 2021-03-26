Raspberry Pi Board Support Package for Windows 10
==============

## Welcome to the Raspberry Pi Board Support Package (BSP) for Windows 10

This repository contains BSP components for the Raspberry Pi 2, 3 and Compute Modules running 32-bit Windows 10 IoT Core (version 1809).
It also contains BSP components for the Raspberry Pi 2 (v1.2), 3, 4 and corresponding Compute Modules running 64-bit Windows 10.

This BSP repository is under community support; it is not actively maintained by Microsoft. 
For Raspberry Pi 4 onwards, it's designed for use with Windows 10 version 2004 and later, because of use of newer driver frameworks which are not available on earlier releases.

## 64-bit firmware

For the Raspberry Pi 4, firmware from the [Pi Firmware Task Force](https://rpi4-uefi.dev) is used, which provides UEFI and ACPI support. It is available at https://github.com/pftf/RPi4/releases.

For the Raspberry Pi 2 (v1.2) and Raspberry Pi 3, the Pi Firmware Task Force provides firmware at https://github.com/pftf/RPi3/releases, which provides UEFI and ACPI support.

## 32-bit firmware

Sample binaries of the firmware is included in [RPi.BootFirmware](bspfiles/Packages/RPi.BootFirmware) to enable quick prototyping. The sources for these binaries are listed below.

1. Firmware binaries : [RaspberryPi/Firmware](https://github.com/raspberrypi/firmware)
2. UEFI Sources : [RPi/UEFI](https://github.com/ms-iot/RPi-UEFI)

### 32-bit EFI Customisations

Note: this section is only applicable to 32-bit Windows 10 IoT Core on Raspberry Pi 3 and earlier.

[SMBIOS requirements of Windows 10 IoT Core OEM Licensing](https://docs.microsoft.com/en-us/windows-hardware/manufacture/iot/license-requirements#smbios-support) requires a custom version of kernel.img file with the proper SMBIOS values.

See [PlatformSmbiosDxe.c](https://github.com/ms-iot/RPi-UEFI/blob/ms-iot/Pi3BoardPkg/Drivers/PlatformSmbiosDxe/PlatformSmbiosDxe.c) to update the SMBIOS data. Steps to build the kernel.img is provided in the [RPi/UEFI Github](https://github.com/ms-iot/RPi-UEFI).

## Build the drivers

1. Clone https://github.com/raspberrypi/windows-drivers
1. Open Visual Studio with Administrator privileges
1. In Visual Studio: File -> Open -> Project/Solution -> Select `windows-drivers\build\bcm2836\buildbcm2836.sln`
1. Set your build configuration (Release or Debug)
1. Build -> Build Solution

The resulting driver binaries will be located in the `windows-drivers\build\bcm2836\ARM\Output` folder, or `windows-drivers\build\bcm2836\ARM64\Output` if you picked an Arm 64-bit configuration.

## Export the bsp

Note: this section is only applicable to Windows 10 IoT Core.

We provide a `binexport.ps1` script to scrape the BSP components together into a zip file for easy use with the IoT ADK AddonKit.
1. Open Powershell
2. Navigate to rpi-iotcore\tools
3. Run `binexport.ps1` with the appropriate arguments.
    ```powershell
    .\binexport.ps1 C:\Release
    (or)
    .\binexport.ps1 C:\Release -IsDebug # for debug binaries
    ```
4. The script will generate a zip file **RPi_BSP_xx.zip** that can be imported into the IoT-ADK-Addonkit shell using [Import-IoTBSP](https://github.com/ms-iot/iot-adk-addonkit/blob/master/Tools/IoTCoreImaging/Docs/Import-IoTBSP.md).
    ```powershell
    Import-IoTBSP RPi C:\Temp\RPi_BSP_xx.zip
    ```
