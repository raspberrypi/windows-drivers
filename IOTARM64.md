# 64-bit IoT Core

This repo currently provides experimental ARM64 IoT Core support for Raspberry Pi 3 and 4.

The supported version is 1809 (Build 17763).

## 64-bit firmware

A 64-bit UEFI firmware for the corresponding platform is required to export the BSP.

### Raspberry Pi 4

For the Raspberry Pi 4, firmware from the [Pi Firmware Task Force](https://rpi4-uefi.dev) is used, which provides UEFI and ACPI support. It is available at https://github.com/pftf/RPi4/releases.

Place the firmware files in `bspfiles/RPi4.ARM64/Packages/RPi.BootFirmware`, the following files are required.

```text
config.txt
fixup4.dat
RPI_EFI.fd
start4.elf
bcm2711-rpi-4-b.dtb
bcm2711-rpi-400.dtb
bcm2711-rpi-cm4.dtb
```

### Raspberry Pi 3

For the Raspberry Pi 3, the Pi Firmware Task Force provides firmware at https://github.com/pftf/RPi3/releases, which provides UEFI and ACPI support.

Place the firmware files in `bspfiles/RPi3.ARM64/Packages/RPi.BootFirmware`, the following files are required.

```text
bootcode.bin
config.txt
fixup.dat
RPI_EFI.fd
start.elf
bcm2710-rpi-3-b.dtb
bcm2710-rpi-3-b-plus.dtb
bcm2710-rpi-cm3.dtb
```

### 64-bit EFI Customisations

[SMBIOS requirements of Windows 10 IoT Core OEM Licensing](https://docs.microsoft.com/en-us/windows-hardware/manufacture/iot/license-requirements#smbios-support) requires a custom version of RPI_EFI.fd with the proper SMBIOS values.

See [PlatformSmbiosDxe.c](https://github.com/tianocore/edk2-platforms/blob/master/Platform/RaspberryPi/Drivers/PlatformSmbiosDxe/PlatformSmbiosDxe.c) to update the SMBIOS data. Steps to build the RPI_EFI.fd is provided in the [edk2-platforms Github](https://github.com/tianocore/edk2-platforms).

## Export the 64-bit BSP

We provide a `binexport.ps1` script to scrape the BSP components together into a zip file for easy use with the IoT ADK AddonKit.
1. Open Powershell
2. Navigate to rpi-iotcore\tools
3. Run `binexport.ps1` with the appropriate arguments.
    ```powershell
    # Platform:
    #    RPi        - Raspberry Pi 2/3           32-bit BSP
    #    RPi3.ARM64 - Raspberry Pi 3             64-bit BSP (experimental)
    #    RPi4.ARM64 - Raspberry Pi 4             64-bit BSP (experimental)

    # OutputDir
    #    The output directory for the BSP zip file.

    # IsDebug
    #    Specify to package debug binaries. Default is Release binaries.
    .\binexport.ps1 [Platform] [OutputDir] [-IsDebug]

    # Samples
    # 64-bit BSP for Raspberry Pi 3, release binaries
    .\binexport.ps1 RPi3.ARM64 C:\Release

    # 64-bit BSP for Raspberry Pi 4, debug binaries
    .\binexport.ps1 RPi4.ARM64 C:\Debug -IsDebug
    ```
4. The script will generate a zip file **RPix.ARM64.BSP.xx.zip** that can be imported into the IoT-ADK-Addonkit shell using [Import-IoTBSP](https://github.com/ms-iot/iot-adk-addonkit/blob/master/Tools/IoTCoreImaging/Docs/Import-IoTBSP.md).
    ```powershell   
    Import-IoTBSP RPi3.ARM64 C:\Temp\RPi3.ARM64.BSP.xx.zip
    (or)
    Import-IoTBSP RPi4.ARM64 C:\Temp\RPi4.ARM64.BSP.xx.zip
    ```
## Additional Drivers

Some additional drivers are required for a usable image.

You'll need the INF-style driver packages. For each INF file, use the following command to create a IoT driver package:

```powershell
Add-IoTDriverPackage sample_driver.inf Drivers.Sample
```

To add driver packages to your IoT Core image, refer to [Add a driver to an image](https://docs.microsoft.com/en-us/windows-hardware/manufacture/iot/add-a-driver-to-an-image) of the IoT Core manufacturing guide.

**WARNING:** If you are going to commercialize your IoT device, please make sure you have valid licenses for the drivers you use.

### USB Driver for Raspberry Pi 3

The DWCUSBOTG USB controller driver for Raspberry Pi 2 and 3 is included in the ARM32 IoT Core packages, but not the ARM64 ones.

The original author of this driver, MCCI, released an ARM64 version of the same driver under a **non commercial** license. The driver is available at https://pi64.win/download-usb-drivers/.

Download the standalone driver EXE, run the EXE file to extract the driver files. You'll need to add both `mcci_dwchsotg_hub.inf` and `mcci_dwchsotg_hcd.inf`.

### Drivers for LAN adapters

The ARM64 IoT Core has drivers for the ASIX AX88772 USB 2.0 to Fast Ethernet Adapter builtin, for other adapters, you'll need to provide your own driver.

**WARNING:** The bcmgenet driver in this repo, for the GENET Gigabit LAN adapter on Raspberry Pi 4, is currently **NOT** supported on IoT Core.

- For the builtin adapter on Rasberry Pi 2/3, ARM64 drivers are available from Microchip. \
To download, go to https://www.microchip.com/en-us/software-library, search for LAN9500 (for Pi 2/3B) or LAN78xx (for Pi 3B+), and select the one listed as "LANxxxx Windows (OneCore) Device Drivers".\
Download the zip package, and extract the files at `ndis650/arm64`.

- For Realtek USB LAN adapters (RTL8152, RTL8153, etc), ARM64 drivers are available on Realtek website at https://www.realtek.com/component/zoo/category/network-interface-controllers-10-100-1000m-gigabit-ethernet-usb-3-0-software. \
Download the "Win10 Auto Installation Program", open the install EXE with 7-zip, and extract the files at `WIN10/arm64`.

- **For testing purposes,** you may also extract drivers from a Windows 10 ARM64 v1809 (Build 17763) Desktop image. \
You can get drivers for these adapters in the following locations: \
Realtek USB adapters: `Windows\System32\DriverStore\FileRepository\rtuarm64w10.inf_arm64_15e06ca8abe46b2a` \
ASIX AX88179: `Windows\System32\DriverStore\FileRepository\netax88179_178a.inf_arm64_c5dc3d41b0e4583b`

## User Interface and App Compatibility

ARM64 IoT Core does not contain the default graphical user interface. By default, the resulting image will boot to a blank screen and have to be managed via the Device Portal web interface or SSH.

The UI App, IoTCoreDefaultApp, is available in source at https://github.com/Microsoft/Windows-iotcore-samples/tree/master/Samples/IoTCoreDefaultApp. To add this app to your image, refer to [Add an app to your image](https://docs.microsoft.com/en-us/windows-hardware/manufacture/iot/deploy-your-app-with-a-standard-board) of the IoT Core manufacturing guide.

IoT Core does not support WOW (Windows on Windows), so only ARM64 apps run on ARM64 IoT Core.

## GPIO, I2C, SPI Access via WinRT API

GPIO should work out of the box via the `Windows.Device.Gpio` API.

I2C and SPI access will **not** work on PFTF / upstream TianoCore firmware, since upstream TianoCore has updated the pin muxing declaration from the Microsoft-proprietary `MsftFunctionConfig` to the new standardized `PinFunction`.

Windows support for `PinFunction` is added in v1903 (Build 18362), therefore I2C and SPI will not work on IoT Core unless this change is reverted.

For more information, check out https://edk2.groups.io/g/devel/message/56396, https://github.com/andreiw/RaspberryPiPkg/issues/132, and https://github.com/andreiw/RaspberryPiPkg/commit/87f3611f9da37f594421bbaf513b9aaccee72176.
