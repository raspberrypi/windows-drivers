<#
.SYNOPSIS
Exports the driver binaries in a zip file format with all the bsp files in the right format.

.DESCRIPTION
Exports the driver binaries in a zip file format with all the bsp files in the right format the output zip file can be imported directly in the IoTCoreShell using the Import-IoTBSP command.

.PARAMETER Platform
Mandatory parameter, specifying the platform the BSP is for.
Valid options: 
    RPi         Raspberry Pi 2/3 32-Bit BSP
    RPi3.ARM64  Raspberry Pi 3   64-Bit BSP
    RPi4.ARM64  Raspberry Pi 4   64-Bit BSP

.PARAMETER OutputDir
Mandatory parameter, specifying the output directory for the zip file.

.PARAMETER IsDebug
Optional Switch parameter to package debug binaries. Default is Release binaries.

.EXAMPLE
binexport.ps1 RPi3.ARM64 "C:\Release" 

#>

param(
    [Parameter(Position = 0, Mandatory = $true)][ValidateSet('RPi', 'RPi3.ARM64', 'RPi4.ARM64')][String]$Platform,
    [Parameter(Position = 1, Mandatory = $true)][ValidateNotNullOrEmpty()][String]$OutputDir,
    [Parameter(Position = 2, Mandatory = $false)][Switch]$IsDebug
)

$RootDir = "$PSScriptRoot\..\"
#$RootDir = Resolve-Path -Path $RootDir
$buildconfig = "Release"
#Override if IsDebug switch defined.
if($IsDebug){
    $buildconfig = "Debug"
}
if($Platform -like "*ARM64") {
    $arch = "ARM64"
} else {
    $arch = "ARM"
}

# Check firmware for ARM64 platform.
$package = "$RootDir\bspfiles\$Platform\Packages"
if($arch -eq "ARM64") {
    $firmware = "$package\RPi.BootFirmware\RPI_EFI.fd"

    if (!(Test-Path $firmware -PathType Leaf)) {
        Write-Host "Error: UEFI Firmware for platform $Platform not found." -ForegroundColor Red
        Write-Host "Please refer to windows-drivers repo on how to get it." -ForegroundColor Red
        return
    }
}

$bindir = $RootDir + "build\bcm2836\$arch\$buildconfig\Output"
$OutputFile = "$Platform.BSP.$buildconfig.zip"

if (!(Test-Path $bindir -PathType Container)){
    Write-Host "Error: $bindir not found." -ForegroundColor Red
    return
}
$bindir = Resolve-Path -Path $bindir

if (!(Test-Path "$OutputDir\$Platform")) {
    New-Item "$OutputDir\$Platform" -ItemType Directory | Out-Null
}

#Copy items
Copy-Item -Path "$RootDir\bspfiles\$Platform\*" -Destination "$OutputDir\$Platform\" -Recurse -Force | Out-Null
Copy-Item -Path "$bindir\*" -Destination "$OutputDir\$Platform\Packages\RPi.Drivers\" -Include "*.sys","*.dll","*.inf" -Force | Out-Null

Write-Host "Output File: $OutputFile"
if (Test-Path "$OutputDir\$OutputFile" -PathType Leaf){
    Remove-Item "$OutputDir\$OutputFile" -Force | Out-Null
}
Compress-Archive -Path "$OutputDir\$Platform\" -CompressionLevel "Fastest" -DestinationPath "$OutputDir\$OutputFile"
Remove-Item "$OutputDir\$Platform\" -Recurse -Force | Out-Null

Write-Host "Done"
