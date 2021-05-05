/*
Copyright 2021 Douglas Cook

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

// Based on PartA2_SD Host_Controller_Simplified_Specification_Ver4.20.pdf
// from https://www.sdcard.org/downloads/pls/pdf/

enum SdRegSpecVersion : UINT8
{
    SdRegSpecVersion1 = 0,
    SdRegSpecVersion2 = 1,
    SdRegSpecVersion3 = 2,
    SdRegSpecVersion4_0 = 3,
    SdRegSpecVersion4_1 = 4,
    SdRegSpecVersion4_2 = 5,
};

enum SdRegDma : UCHAR
{
    SdRegDmaSdma,
    SdRegDmaReserved,
    SdRegDma32BitAdma2,
    SdRegDma64BitAdma2,
};

enum SdRegVoltage : UCHAR
{
    SdRegVoltageNone = 0,
    SdRegVoltage1_2 = 4,
    SdRegVoltage1_8 = 5,
    SdRegVoltage3_0 = 6,
    SdRegVoltage3_3 = 7,
};

enum SdRegUhs : UCHAR
{
    SdRegUhsSDR12,
    SdRegUhsSDR25,
    SdRegUhsSDR50,
    SdRegUhsSDR104,
    SdRegUhsDDR50,
};

enum SdRegDriverStrength : UCHAR
{
    SdRegDriverStrengthB,
    SdRegDriverStrengthA,
    SdRegDriverStrengthC,
    SdRegDriverStrengthD,
};

enum SdRegAutoCmd : UCHAR
{
    SdRegAutoCmdDisabled,
    SdRegAutoCmd12Enable,
    SdRegAutoCmd32Enable,
    SdRegAutoCmdAutoSelect,
};

enum SdRegResponse : UCHAR
{
    SdRegResponseNone,
    SdRegResponse136,
    SdRegResponse48,
    SdRegResponse48CheckBusy,
};

enum SdRegCommandType : UCHAR
{
    SdRegCommandTypeNormal,
    SdRegCommandTypeSuspend,
    SdRegCommandTypeResume,
    SdRegCommandTypeAbort,
};

enum SdRegDmaAction : unsigned
{
    SdRegDmaActionAdma2Nop = 0,
    SdRegDmaActionAdma2Rsv = 2,
    SdRegDmaActionAdma2Tran = 4,
    SdRegDmaActionAdma2Link = 6,
    SdRegDmaActionAdma3CmdSD = 1,
    SdRegDmaActionAdma3CmdUHS2 = 3,
    SdRegDmaActionAdma3Reserved = 5,
    SdRegDmaActionAdma3IntegratedDescriptor = 7,
};

union SdRegPowerControl
{
    explicit constexpr
    SdRegPowerControl(UINT8 u8)
        : U8(u8) {}

    UINT8 U8;
    struct
    {
        UCHAR Vdd1Power : 1;
        SdRegVoltage Vdd1Voltage : 3;
        UCHAR Vdd2Power : 1;
        SdRegVoltage Vdd2Voltage : 3;
    };
};
static_assert(sizeof(SdRegPowerControl) == 1, "SdRegPowerControl");

union SdRegCapabilities
{
    explicit constexpr
    SdRegCapabilities(UINT32 low, UINT32 high)
        : U64(low | ((UINT64)high << 32)) {}

    UINT64 U64;
    ULONG U32s[2];
    struct
    {
        UCHAR TimeoutClockFrequency : 6;
        UCHAR Reserved6 : 1;
        UCHAR TimeoutClockUnit : 1;

        UCHAR BaseFrequencyForSdClock : 8;

        UCHAR MaxBlockLength : 2;
        UCHAR DeviceBus8Bit : 1;
        UCHAR Adma2 : 1;
        UCHAR Reserved20 : 1;
        UCHAR HighSpeed : 1;
        UCHAR Sdma : 1;
        UCHAR SuspendResume : 1;

        UCHAR Voltage3_3 : 1;
        UCHAR Voltage3_0 : 1;
        UCHAR Voltage1_8 : 1;
        UCHAR SystemAddress64BitV4 : 1;
        UCHAR SystemAddress64BitV3 : 1;
        UCHAR AsynchronousInterrupt : 1;
        UCHAR SlotType : 2;

        UCHAR SDR50 : 1;
        UCHAR SDR104 : 1;
        UCHAR DDR50 : 1;
        UCHAR UHS2 : 1;
        UCHAR DriverTypeA : 1;
        UCHAR DriverTypeC : 1;
        UCHAR DriverTypeD : 1;
        UCHAR Reserved39 : 1;

        UCHAR TimerCountForReTuning : 4;
        UCHAR Reserved44 : 1;
        UCHAR UseTuningForSDR50 : 1;
        UCHAR RetuningModes : 2;

        UCHAR ClockMultiplier : 8;

        UCHAR Reserved56 : 8;
    };
};
static_assert(sizeof(SdRegCapabilities) == 8, "SdRegCapabilities");

union SdRegHostControl1
{
    explicit constexpr
    SdRegHostControl1(UINT8 u8)
        : U8(u8) {}

    UINT8 U8;
    struct
    {
        UCHAR LedControl : 1;
        UCHAR DataTransferWidth4 : 1;
        UCHAR HighSpeedEnable : 1;
        SdRegDma DmaSelect : 2;
        UCHAR DataTransferWidth8 : 1;
        UCHAR CardDetectTestLevel : 1;
        UCHAR CardDetectTestSelect : 1;
    };
};
static_assert(sizeof(SdRegHostControl1) == 1, "SdRegHostControl1");

union SdRegHostControl2
{
    explicit constexpr
    SdRegHostControl2(UINT16 u16)
        : U16(u16) {}

    UINT16 U16;
    struct
    {
        SdRegUhs UhsModeSelect : 3;
        UCHAR Signaling1_8 : 1;
        SdRegDriverStrength DriverStrength : 2;
        UCHAR ExecuteTuning : 1;
        UCHAR SamplingClockSelect : 1;
        UCHAR Uhs2Enable : 1;
        UCHAR Reserved9 : 1;
        UCHAR Adma2Length26Bit : 1;
        UCHAR Cmd23Enable : 1;
        UCHAR HostVersion4Enable : 1;
        UCHAR Addressing64Bit : 1;
        UCHAR AsynchronousInterruptEnable : 1;
        UCHAR PresetValueEnable : 1;
    };
};
static_assert(sizeof(SdRegHostControl2) == 2, "SdRegHostControl2");

union SdRegBlockGapControl
{
    explicit constexpr
    SdRegBlockGapControl(UINT8 u8)
        : U8(u8) {}

    UINT8 U8;
    struct
    {
        UCHAR StopAtBlockGapRequest : 1;
        UCHAR ContinueRequest : 1;
        UCHAR ReadWaitControl : 1;
        UCHAR InterruptAtBlockGap : 1;
        UCHAR Reserved4 : 4;
    };
};
static_assert(sizeof(SdRegBlockGapControl) == 1, "SdRegBlockGapControl");

union SdRegClockControl
{
    explicit constexpr
    SdRegClockControl(UINT16 u16)
        : U16(u16) {}

    UINT16 U16;
    struct
    {
        UCHAR InternalClockEnable : 1;
        UCHAR InternalClockStable : 1;
        UCHAR SdClockEnable : 1;
        UCHAR PllEnable : 1;
        UCHAR Reserved4 : 1;
        UCHAR ClockGeneratorProgrammable : 1;
        UCHAR FrequencySelectUpper : 2;
        UCHAR FrequencySelect : 8;
    };
};
static_assert(sizeof(SdRegClockControl) == 2, "SdRegClockControl");

union SdRegSoftwareReset
{
    explicit constexpr
    SdRegSoftwareReset(UINT8 u8)
        : U8(u8) {}

    UINT8 U8;
    struct
    {
        UCHAR ResetForAll : 1;
        UCHAR ResetForCmdLine : 1;
        UCHAR ResetForDatLine : 1;
        UCHAR Reserved3 : 5;
    };
};
static_assert(sizeof(SdRegSoftwareReset) == 1, "SdRegSoftwareReset");

union SdRegTransferMode
{
    explicit constexpr
    SdRegTransferMode(UINT16 u16)
        : U16(u16) {}

    UINT16 U16;
    struct
    {
        UCHAR DmaEnable : 1;
        UCHAR BlockCountEnable : 1;
        SdRegAutoCmd AutoCmdEnable : 2;
        UCHAR DataTransferDirectionRead : 1;
        UCHAR MultipleBlock : 1;
        UCHAR ResponseTypeR5 : 1;
        UCHAR ResponseErrorCheckEnable : 1;
        UCHAR ResponseInterruptDisable : 1;
        UCHAR Reserved9 : 7;
    };
};
static_assert(sizeof(SdRegTransferMode) == 2, "SdRegTransferMode");

union SdRegCommand
{
    explicit constexpr
    SdRegCommand(UINT16 u16)
        : U16(u16) {}

    UINT16 U16;
    struct
    {
        SdRegResponse ResponseType : 2;
        UCHAR SubCommand : 1;
        UCHAR CommandCrcCheck : 1;
        UCHAR CommandIndexCheck : 1;
        UCHAR DataPresent : 1;
        SdRegCommandType CommandType : 2;
        UCHAR CommandIndex : 6;
        UCHAR Reserved14 : 2;
    };
};
static_assert(sizeof(SdRegCommand) == 2, "SdRegCommand");

union SdRegPresentState
{
    explicit constexpr
    SdRegPresentState(ULONG u32)
        : U32(u32) {}

    ULONG U32;
    struct
    {
        UCHAR CommandInhibitCmd : 1;
        UCHAR CommandInhibitDat : 1;
        UCHAR DatLineActive : 1;
        UCHAR ReTuningRequest : 1;
        UCHAR EmbeddedDatSignalLevel : 4;

        UCHAR WriteTransferActive : 1;
        UCHAR ReadTransferActive : 1;
        UCHAR BufferWriteEnable : 1;
        UCHAR BufferReadEnable : 1;
        UCHAR Reserved12 : 4;

        UCHAR CardInserted : 1;
        UCHAR CardStateStable : 1;
        UCHAR CardDetect : 1;
        UCHAR WriteEnabled : 1;
        UCHAR SdDatSignalLevel : 4;

        UCHAR SdCmdSignalLevel : 1;
        UCHAR HostRegulatorVoltageStable : 1;
        UCHAR Reserved26 : 1;
        UCHAR CommandNotIssuedByError : 1;
        UCHAR SubComandStatus : 1;
        UCHAR InDormantState : 1;
        UCHAR LaneSynchronization : 1;
        UCHAR Uhs2IfDetection : 1;
    };
};
static_assert(sizeof(SdRegPresentState) == 4, "SdRegPresentState");

union SdRegNormalInterrupts
{
    explicit constexpr
    SdRegNormalInterrupts(UINT16 u16)
        : U16(u16) {}

    UINT16 U16;
    struct
    {
        UCHAR CommandComplete : 1;
        UCHAR TransferComplete : 1;
        UCHAR BlockGapEvent : 1;
        UCHAR DmaInterrupt : 1;
        UCHAR BufferWriteReady : 1;
        UCHAR BufferReadReady : 1;
        UCHAR CardInsertion : 1;
        UCHAR CardRemoval : 1;
        UCHAR CardInterrupt : 1;
        UCHAR IntA : 1;
        UCHAR IntB : 1;
        UCHAR IntC : 1;
        UCHAR ReTuningEvent : 1;
        UCHAR FxEvent : 1;
        UCHAR Reserved14 : 1;
        UCHAR ErrorInterrupt : 1;
    };
};
static_assert(sizeof(SdRegNormalInterrupts) == 2, "SdRegNormalInterrupts");

union SdRegErrorInterrupts
{
    explicit constexpr
    SdRegErrorInterrupts(UINT16 u16)
        : U16(u16) {}

    UINT16 U16;
    struct
    {
        UCHAR CommandTimeout : 1;
        UCHAR CommandCRC : 1;
        UCHAR CommandEndBit : 1;
        UCHAR CommandIndex : 1;
        UCHAR DataTimeout : 1;
        UCHAR DataCRC : 1;
        UCHAR DataEndBit : 1;
        UCHAR CurrentLimit : 1;
        UCHAR AutoCmd : 1;
        UCHAR Adma : 1;
        UCHAR Tuning : 1;
        UCHAR Response : 1;
    };
};
static_assert(sizeof(SdRegErrorInterrupts) == 2, "SdRegErrorInterrupts");

struct SdRegisters
{
    ULONG SdmaSystemAddress;            // 00
    UINT16 BlockSize;                   // 04
    UINT16 BlockCount16;                // 06
    ULONG Argument;                     // 08
    SdRegTransferMode TransferMode;     // 0C
    SdRegCommand Command;               // 0E
    ULONG Response32s[4];               // 10
    ULONG BufferDataPort;               // 20
    SdRegPresentState PresentState;     // 24
    SdRegHostControl1 HostControl1;     // 28
    SdRegPowerControl PowerControl;     // 29
    SdRegBlockGapControl BlockGapControl;//2A
    UINT8 WakeupControl;                // 2B
    SdRegClockControl ClockControl;     // 2C
    UINT8 TimeoutControl;               // 2E
    SdRegSoftwareReset SoftwareReset;   // 2F
    SdRegNormalInterrupts NormalInterruptStatus;       // 30
    SdRegErrorInterrupts ErrorInterruptStatus;         // 32
    SdRegNormalInterrupts NormalInterruptStatusEnable; // 34
    SdRegErrorInterrupts ErrorInterruptStatusEnable;   // 36
    SdRegNormalInterrupts NormalInterruptSignalEnable; // 38
    SdRegErrorInterrupts ErrorInterruptSignalEnable;   // 3A
    UINT16 AutoCmdErrorStatus;          // 3C
    SdRegHostControl2 HostControl2;     // 3E
    SdRegCapabilities Capabilities;     // 40
    UINT8 MaximumCurrentVdd1[4];        // 48
    UINT8 MaximumCurrentVdd2[4];        // 4C
    UINT16 ForceEventAutoCmdError;      // 50
    UINT16 ForceEventInterruptError;    // 52
    UINT8 AdmaErrorStatus;              // 54
    UINT8 Reserved55;                   // 55
    UINT16 Reserved56;                  // 56
    ULONG AdmaSystemAddress;            // 58
    ULONG AdmaSystemAddressHigh;        // 5C
    UINT16 PresetValue16s[8];           // 60
    ULONG Reserved70[32];               // 70 - ADMA3, UHS-II, vendor-specific stuff.
    ULONG ReservedF0[3];                // F0
    UINT16 SlotInterruptStatus;         // FC
    SdRegSpecVersion SpecVersion;       // FE
    UINT8 VendorVersion;                // FF
};
static_assert(sizeof(SdRegisters) == 0x100, "SdRegisters");

// Descriptor for 32-bit addresses.
union SdRegDma32
{
    UINT64 U64;
    ULONG U32s[2];
    struct
    {
        unsigned Valid : 1;
        unsigned End : 1;
        unsigned Int : 1;
        SdRegDmaAction Action : 3;
        unsigned LengthHigh : 10; // ADMA3
        unsigned Length : 16;
        UINT32 Address;
    };
};
static_assert(sizeof(SdRegDma32) == 8, "SdRegDma32");
