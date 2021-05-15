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
#include "SdRegisters.h"

class SlotExtension
{
public:

    _IRQL_requires_max_(PASSIVE_LEVEL)
    void CODE_SEG_PAGE
    SlotCleanup();

    _IRQL_requires_max_(PASSIVE_LEVEL)
    NTSTATUS CODE_SEG_PAGE
    SlotInitialize(
        _In_ PHYSICAL_ADDRESS physicalBase,
        _In_ void* virtualBase,
        _In_ ULONG length,
        _In_ BOOLEAN crashdumpMode);

    _IRQL_requires_max_(PASSIVE_LEVEL)
    void CODE_SEG_PAGE
    SlotGetSlotCapabilities(_Out_ SDPORT_CAPABILITIES* capabilities);

    _IRQL_requires_max_(APC_LEVEL)
    NTSTATUS CODE_SEG_PAGE
    SlotIssueBusOperation(_In_ SDPORT_BUS_OPERATION const& busOperation);

    _IRQL_requires_max_(PASSIVE_LEVEL)
    BOOLEAN CODE_SEG_PAGE
    SlotGetCardDetectState();

    _IRQL_requires_max_(DISPATCH_LEVEL)
    BOOLEAN CODE_SEG_TEXT
    SlotGetWriteProtectState();

    _IRQL_requires_max_(HIGH_LEVEL)
    BOOLEAN CODE_SEG_TEXT
    SlotInterrupt(
        _Out_ ULONG* events,
        _Out_ ULONG* errors,
        _Out_ BOOLEAN* notifyCardChange,
        _Out_ BOOLEAN* notifySdioInterrupt,
        _Out_ BOOLEAN* notifyTuning);

    _IRQL_requires_(DISPATCH_LEVEL)
    NTSTATUS CODE_SEG_TEXT
    SlotIssueRequest(_Inout_ SDPORT_REQUEST* request);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    void CODE_SEG_TEXT
    SlotGetResponse(
        _In_ SDPORT_COMMAND const& command,
        _Inout_bytecap_c_(16) void* responseBuffer);

    _IRQL_requires_max_(HIGH_LEVEL)
    void CODE_SEG_TEXT
    SlotToggleEvents(
        _In_ ULONG eventMask,
        _In_ BOOLEAN enable);

    _IRQL_requires_max_(HIGH_LEVEL)
    void CODE_SEG_TEXT
    SlotClearEvents(_In_ ULONG eventMask);

    _IRQL_requires_(DISPATCH_LEVEL)
    void CODE_SEG_TEXT
    SlotRequestDpc(
        _Inout_ SDPORT_REQUEST* request,
        _In_ ULONG events,
        _In_ ULONG errors);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    void CODE_SEG_TEXT
    SlotSaveContext();

    _IRQL_requires_max_(DISPATCH_LEVEL)
    void CODE_SEG_TEXT
    SlotRestoreContext();

private:

    _IRQL_requires_max_(DISPATCH_LEVEL)
    void CODE_SEG_TEXT
    FreeDmaBuffers();

    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS CODE_SEG_TEXT
    AllocateDmaBuffers(ULONG cbDmaData);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    bool CODE_SEG_TEXT
    CommandShouldUseDma(SDPORT_COMMAND const& command);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS CODE_SEG_PAGE
    SetRegulatorVoltage1_8(bool regulatorVoltage1_8);

    _IRQL_requires_max_(PASSIVE_LEVEL)
    _IRQL_requires_same_
    _Function_class_(WORKER_THREAD_ROUTINE)
    static void CODE_SEG_PAGE
    InvokeSetRegulatorVoltageWorker(void* parameter);

    _IRQL_requires_max_(APC_LEVEL)
    NTSTATUS CODE_SEG_PAGE
    InvokeSetRegulatorVoltage1_8(bool regulatorVoltage1_8);

    _IRQL_requires_max_(HIGH_LEVEL)
    void CODE_SEG_TEXT
    AcknowledgeInterrupts(SdRegNormalInterrupts interruptMask);

    _IRQL_requires_(DISPATCH_LEVEL)
    NTSTATUS CODE_SEG_TEXT
    SendCommand(_Inout_ SDPORT_REQUEST* request);

    _IRQL_requires_(DISPATCH_LEVEL)
    NTSTATUS CODE_SEG_TEXT
    StartPioTransfer(_Inout_ SDPORT_REQUEST* request);

    _IRQL_requires_(DISPATCH_LEVEL)
    NTSTATUS CODE_SEG_TEXT
    StartDmaTransfer(_Inout_ SDPORT_REQUEST* request);

    _IRQL_requires_(DISPATCH_LEVEL)
    NTSTATUS CODE_SEG_TEXT
    StartSgDmaTransfer(_In_ SDPORT_REQUEST* request);

    _IRQL_requires_max_(APC_LEVEL)
    NTSTATUS CODE_SEG_PAGE
    ResetHw();

    _IRQL_requires_max_(APC_LEVEL)
    NTSTATUS CODE_SEG_PAGE
    ResetHost(SDPORT_RESET_TYPE resetType);

    _IRQL_requires_max_(APC_LEVEL)
    NTSTATUS CODE_SEG_PAGE
    SetClock(ULONG frequencyKhz);

    _IRQL_requires_max_(APC_LEVEL)
    NTSTATUS CODE_SEG_PAGE
    SetVoltage(SDPORT_BUS_VOLTAGE voltage);

    _IRQL_requires_max_(APC_LEVEL)
    NTSTATUS CODE_SEG_PAGE
    SetBusWidth(SDPORT_BUS_WIDTH busWidth);

    _IRQL_requires_max_(APC_LEVEL)
    NTSTATUS CODE_SEG_PAGE
    SetBusSpeed(SDPORT_BUS_SPEED busSpeed);

    _IRQL_requires_max_(APC_LEVEL)
    NTSTATUS CODE_SEG_PAGE
    SetSignalingVoltage(SDPORT_SIGNALING_VOLTAGE signalingVoltage);

    _IRQL_requires_max_(APC_LEVEL)
    NTSTATUS CODE_SEG_PAGE
    SetDriverType(SDPORT_DRIVER_TYPE driverType);

    _IRQL_requires_max_(APC_LEVEL)
    NTSTATUS CODE_SEG_PAGE
    SetPresetValue(bool enabled);

    _IRQL_requires_max_(APC_LEVEL)
    NTSTATUS CODE_SEG_PAGE
    SetBlockGapInterrupt(bool enabled);

    _IRQL_requires_max_(APC_LEVEL)
    NTSTATUS CODE_SEG_PAGE
    ExecuteTuning();

private:

    enum Magic : UINT16
    {
        MagicPreInitialize = 0,
        MagicValid = 0xE88C,
        MagicPostCleanup = 0xDEAD,
    };

    volatile SdRegisters* m_registers; // Should only be accessed via READ_REGISTER/WRITE_REGISTER.
    MDL* m_pDmaDataMdl;
    MDL* m_pDmaDescriptorMdl;
    UINT32 m_iDmaEndDescriptor; // The index of the descriptor where End == true.
    SDPORT_TRANSFER_DIRECTION m_dmaInProgress;
    UINT32 m_dmaTranslation; // 0xC0000000 on old chips, 0x0 on new chips.
    Magic m_magic;
    bool m_crashDumpMode;
    bool m_regulatorVoltage1_8;
    WORK_QUEUE_ITEM m_rpiqWorkItem;
    SDPORT_CAPABILITIES m_capabilities;
};
