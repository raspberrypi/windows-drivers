//
// Copyright (C) Microsoft. All rights reserved.
//
// Abstract:
//
//  This module contains common enums, types, constants and class definition
//  for Sdhost miniport driver controlling a Broadcom BCM283X proprietary
//  implementation of an SD Host Controller that interfaces to SD memory cards
//  compliant to SD Memory Card Specifications version 2.0 dated May 2006,
//  while this SD Host Controller itself is not SD standard compliant
//
//  Workarounds had to be implemented to fit this non-standard SDHC into Sdhost
//  port/miniport framework, while following the standard SDHC implementation
//  as a guidance
//
// Environment:
//
//  Kernel mode only
//

#ifndef _SDHC_HPP_
#define _SDHC_HPP_ 1

#define SDHC_NONPAGED_SEGMENT_BEGIN \
    __pragma(code_seg(push)) \
    //__pragma(code_seg(.text))

#define SDHC_NONPAGED_SEGMENT_END \
    __pragma(code_seg(pop))

#define SDHC_PAGED_SEGMENT_BEGIN \
    __pragma(code_seg(push)) \
    __pragma(code_seg("PAGE"))

#define SDHC_PAGED_SEGMENT_END \
    __pragma(code_seg(pop))

#define SDHC_INIT_SEGMENT_BEGIN \
    __pragma(code_seg(push)) \
    __pragma(code_seg("INIT"))

#define SDHC_INIT_SEGMENT_END \
    __pragma(code_seg(pop))

//
// Support placement semantics for object construction
//

__forceinline void* __cdecl operator new ( size_t, void* Ptr ) throw ()
{
    return Ptr;
} // operator new ( size_t, void* )

__forceinline void __cdecl operator delete ( void* Ptr ) throw ()
{
    NT_ASSERT(!"Unexpected call!");
    if (Ptr) ::ExFreePool(Ptr);
} // operator delete ( void* )

__forceinline void __cdecl operator delete ( void* Ptr, size_t ) throw ()
{
    NT_ASSERT(!"Unexpected call!");
    if (Ptr) ::ExFreePool(Ptr);
} // operator delete ( void*, size_t )

//
// Standard SD Commands Index
//

#define SDCMD_STOP_TRANSMISSION     12
#define SDCMD_SELECT_CARD           7

#if DBG
//
// When enabled, spawns a system thread that keeps updating a shadowed copy of
// SDHC registers in a tight loop
// Status sampling floods the IO bus and will degrade performance use only
// for debugging purposes
//
#define ENABLE_STATUS_SAMPLING      0
#endif

//
// When enabled, logs on request completion of each data transfer request useful
// measurements that aid in SDHC performance assessment
//
#define ENABLE_PERFORMANCE_LOGGING  1

extern "C" DRIVER_INITIALIZE DriverEntry;

//
// class SDHC
//

class SDHC {
    friend DRIVER_INITIALIZE DriverEntry;

private: // non-paged

    //
    // Empirically chosen timeout numbers that seem to work with Sdport and SDHC
    // Numbers are chosen such that any waitFor* method will timeout after
    // a lower bound of 1 second of call blocking, either because of polling
    // timeout or due to HW Read/Write/Erase timeout on the SDHC
    //
    enum : ULONG {
        //
        // Number of register maximum polls
        //
        _POLL_RETRY_COUNT = 100000,

        //
        // Waits between each registry poll
        //
        _POLL_WAIT_US = 10,

        //
        // Threshold used to catch very long waits on SDHC FSM state transitions
        // This is very helpful in catching poorly behaving SDCards with high latency
        // of finishing block writes
        //
        _LONG_FSM_WAIT_TIME_THRESHOLD_US = 100000,

        //
        // Divider used to determine SDHC HW timeout for Read/Write/Erase
        // The time-out value set in TOUT register is SDCLK / _RWE_TIMEOUT_CLOCK_DIV
        // A value of 1 means 1s HW timeout, a value of 4 means 1/4 of a second timeout
        //
        _RWE_TIMEOUT_CLOCK_DIV = 1,
    }; // enum

    enum class _REGISTER : ULONG {
        CMD  = 0x00,
        ARG  = 0x04,
        TOUT = 0x08,
        CDIV = 0x0C,
        RSP0 = 0x10,
        RSP1 = 0x14,
        RSP2 = 0x18,
        RSP3 = 0x1C,
        HSTS = 0x20,
        VDD  = 0x30,
        EDM  = 0x34,
        HCFG = 0x38,
        HBCT = 0x3C,
        DATA = 0x40,
        HBLC = 0x50
    }; // enum class _REGISTER

    union _CMD {
        enum : ULONG { OFFSET = ULONG(_REGISTER::CMD) };
        UINT32 AsUint32;
        struct {
            unsigned Command     : 6;  // 0:5
            unsigned ReadCmd     : 1;  // 6
            unsigned WriteCmd    : 2;  // 7:8
            unsigned ResponseCmd : 2;  // 9:10
            unsigned BusyCmd     : 1;  // 11
            unsigned _reserved0  : 2;  // 12:13
            unsigned FailFlag    : 1;  // 14
            unsigned NewFlag     : 1;  // 15
            unsigned _reserved1  : 16; // 16:31
        } Fields;
    }; // union _CMD

    union _ARG {
        enum : ULONG { OFFSET = ULONG(_REGISTER::ARG) };

        UINT32 AsUint32;
        struct {
            unsigned Argument : 32; // 0:31
        } Fields;
    }; // union _ARG

    union _TOUT {
        enum : ULONG { OFFSET = ULONG(_REGISTER::TOUT) };

        UINT32 AsUint32;
        struct {
            unsigned Timeout : 32; // 0:31
        } Fields;
    }; // union _TOUT

    union _CDIV {
        enum : ULONG { OFFSET = ULONG(_REGISTER::CDIV) };

        UINT32 AsUint32;
        struct {
            unsigned Clockdiv   : 11; // 0:10
            unsigned _reserved0 : 21; // 11:31
        } Fields;
    }; // union _CDIV

    union _RSP0 {
        enum : ULONG { OFFSET = ULONG(_REGISTER::RSP0) };

        UINT32 AsUint32;
        struct {
            unsigned CardStatus : 32; // 0:31
        } Fields;
    }; // union _RSP0

    union _RSP1 {
        enum : ULONG { OFFSET = ULONG(_REGISTER::RSP1) };

        UINT32 AsUint32;
        struct {
            unsigned CidCsd : 32; // 0:31
        } Fields;
    }; // union _RSP1

    union _RSP2 {
        enum : ULONG { OFFSET = ULONG(_REGISTER::RSP2) };

        UINT32 AsUint32;
        struct {
            unsigned CidCsd : 32; // 0:31
        } Fields;
    }; // union _RSP2

    union _RSP3 {
        enum : ULONG { OFFSET = ULONG(_REGISTER::RSP3) };

        UINT32 AsUint32;
        struct {
            unsigned CidCsd : 32; // 0:31
        } Fields;
    }; // union _RSP3

    union _HSTS {
        enum : ULONG { OFFSET = ULONG(_REGISTER::HSTS) };

        enum : UINT32 {
            UINT32_DATA_MASK = 0x1,
            UINT32_ERROR_MASK = 0x00F8,
            UINT32_IRPT_MASK  = 0x0700,
            UINT32_EVENTS_MASK = UINT32_IRPT_MASK | UINT32_DATA_MASK,
            UINT32_EVENTS_AND_ERRORS_MASK = UINT32_EVENTS_MASK | UINT32_ERROR_MASK
        };

        UINT32 AsUint32;
        struct {
            unsigned DataFlag   : 1;  // 0
            unsigned _reserved0 : 2;  // 1:2
            unsigned FifoError  : 1;  // 3
            unsigned Crc7Error  : 1;  // 4
            unsigned Crc16Error : 1;  // 5
            unsigned CmdTimeOut : 1;  // 6
            unsigned RewTimeOut : 1;  // 7
            unsigned SdioIrpt   : 1;  // 8
            unsigned BlockIrpt  : 1;  // 9
            unsigned BusyIrpt   : 1;  // 10
            unsigned _reserved1 : 21; // 11:31
        } Fields;
    }; // union _HSTS

    union _VDD {
        enum : ULONG { OFFSET = ULONG(_REGISTER::VDD) };

        UINT32 AsUint32;
        struct {
            unsigned PowerOn    : 1;  // 0
            unsigned ClockOff   : 1;  // 1
            unsigned _reserved0 : 30; // 2:31
        } Fields;
    }; // union _VDD

    union _EDM {
        enum : ULONG { OFFSET = ULONG(_REGISTER::EDM) };

        enum : UINT32 {
            UINT32_FSM_IDENTMODE = 0x0,
            UINT32_FSM_DATAMODE = 0x1,
            UINT32_FSM_WRITESTART1 = 0xa
        };

        UINT32 AsUint32;
        struct {
            unsigned StateMachine   : 4;  // 0:3
            unsigned FifoCount      : 5;  // 4:8
            unsigned WriteThreshold : 5;  // 9:13
            unsigned ReadThreshold  : 5;  // 14:18
            unsigned Force          : 1;  // 19
            unsigned Clock          : 1;  // 20
            unsigned ClearFifo      : 1;  // 21
            unsigned _reserved0     : 10; // 22:31
        } Fields;
    }; // union _EDM

    union _HCFG {
        enum : ULONG { OFFSET = ULONG(_REGISTER::HCFG) };

        enum : UINT32 {
            UINT32_IRPT_EN_MASK  = 0x0530
        };

        UINT32 AsUint32;
        struct {
            unsigned RelCmdLine  : 1;  // 0
            unsigned WideIntBus  : 1;  // 1
            unsigned WideExtBus  : 1;  // 2
            unsigned SlowCard    : 1;  // 3
            unsigned DataIrptEn  : 1;  // 4
            unsigned SdioIrptEn  : 1;  // 5
            unsigned _reserved0  : 2;  // 6:7
            unsigned BlockIrptEn : 1;  // 8
            unsigned _reserved1  : 1;  // 9
            unsigned BusyIrptEn  : 1;  // 10
            unsigned _reserved2  : 21; // 11:31
        } Fields;
    }; // union _HCFG

    union _HBCT {
        enum : ULONG { OFFSET = ULONG(_REGISTER::HBCT) };

        UINT32 AsUint32;
        struct {
            unsigned ByteCount : 32; // 0:31
        } Fields;
    }; // union _HBCT

    union _DATA {
        enum : ULONG { OFFSET = ULONG(_REGISTER::DATA) };

        UINT32 AsUint32;
        struct {
            unsigned Data : 32; // 0:31
        } Fields;
    }; // union _DATA

    union _HBLC {
        enum : ULONG { OFFSET = ULONG(_REGISTER::HBLC) };

        UINT32 AsUint32;
        struct {
            unsigned BlockCount : 9;  // 0:8
            unsigned _reserved0 : 23; // 9:31
        } Fields;
    }; // union _HBLC

    union _SDPORT_EVENTS {
        UINT32 AsUint32;
        struct {
            unsigned CardResponse  : 1;  // 0
            unsigned CardRwEnd     : 1;  // 1
            unsigned BlockGap      : 1;  // 2
            unsigned DmaComplete   : 1;  // 3
            unsigned BufferEmpty   : 1;  // 4
            unsigned BufferFull    : 1;  // 5
            unsigned CardChange    : 2;  // 6:7
            unsigned CardInterrupt : 1;  // 8
            unsigned _reserved0    : 3;  // 9:11
            unsigned Tuning        : 1;  // 12
            unsigned _reserved1    : 2;  // 13:14
            unsigned Error         : 1;  // 15
            unsigned _reserved2    : 16; // 16:31
        } Fields;
    }; // _SDPORT_EVENTS

    union _SDPORT_ERRORS {
        UINT32 AsUint32;
        struct {
            unsigned CmdTimeout      : 1;  // 0
            unsigned CmdCrcError     : 1;  // 1
            unsigned CmdEndBitError  : 1;  // 2
            unsigned CmdIndexError   : 1;  // 3
            unsigned DataTimeout     : 1;  // 4
            unsigned DataCrcError    : 1;  // 5
            unsigned DataEndBitError : 1;  // 6
            unsigned BusPowerError   : 1;  // 7
            unsigned _reserved0      : 21; // 8:29
            unsigned GenericIoError  : 1;  // 30
            unsigned _reserved1      : 1;  // 31
        } Fields;
    }; // _SDPORT_ERRORS

    template < typename _T_REG_UNION > __forceinline void readRegister (
        _Out_ _T_REG_UNION* RegUnionPtr
        ) const throw ()
    {
        C_ASSERT(sizeof(UINT32) == sizeof(ULONG));
        ULONG* const regPtr = reinterpret_cast<ULONG*>(
            ULONG_PTR(this->basePtr) + ULONG(RegUnionPtr->OFFSET));
        RegUnionPtr->AsUint32 = ::READ_REGISTER_ULONG(regPtr);
    } // readRegister<...> ( _T_REG_UNION* )

    template < typename _T_REG_UNION > __forceinline void readRegisterNoFence (
        _Out_ _T_REG_UNION* RegUnionPtr
        ) const throw ()
    {
        C_ASSERT(sizeof(UINT32) == sizeof(ULONG));
        ULONG* const regPtr = reinterpret_cast<ULONG*>(
            ULONG_PTR(this->basePtr) + ULONG(RegUnionPtr->OFFSET));
        RegUnionPtr->AsUint32 = ::READ_REGISTER_NOFENCE_ULONG(regPtr);
     } // readRegisterNoFence<...> ( _T_REG_UNION* )

    template < typename _T_REG_UNION > __forceinline _T_REG_UNION readRegister () const throw ()
    {
        _T_REG_UNION regUnion; this->readRegister(&regUnion);
        return regUnion;
    } // readRegister<...> ()

    template < typename _T_REG_UNION > __forceinline _T_REG_UNION readRegisterNoFence () const throw ()
    {
        _T_REG_UNION regUnion; this->readRegisterNoFence(&regUnion);
        return regUnion;
    } // readRegisterNoFence<...> ()

    template < typename _T_REG_UNION > __forceinline void writeRegister (
        _T_REG_UNION RegUnion
        ) const throw ()
    {
        C_ASSERT(sizeof(UINT32) == sizeof(ULONG));
        ULONG* const regPtr = reinterpret_cast<ULONG*>(
            ULONG_PTR(this->basePtr) + ULONG(RegUnion.OFFSET));
        ::WRITE_REGISTER_ULONG(regPtr, RegUnion.AsUint32);
    } // writeRegister<...> ( _T_REG_UNION )

    template < typename _T_REG_UNION > __forceinline void writeRegisterNoFence (
        _T_REG_UNION RegUnion
        ) const throw ()
    {
        C_ASSERT(sizeof(UINT32) == sizeof(ULONG));
        ULONG* const regPtr = reinterpret_cast<ULONG*>(
            ULONG_PTR(this->basePtr) + ULONG(RegUnion.OFFSET));
        ::WRITE_REGISTER_NOFENCE_ULONG(regPtr, RegUnion.AsUint32);
    } // writeRegisterNoFence<...> ( _T_REG_UNION )

    NTSTATUS readFromFifo (
        _Out_writes_bytes_(Size) void* BufferPtr,
        ULONG Size
        ) throw ();

    NTSTATUS writeToFifo (
        _In_reads_bytes_(Size) const void* BufferPtr,
        ULONG Size
        ) throw ();

    enum class _COMMAND_RESPONSE : UCHAR {
        SHORT_48BIT = 0,
        LONG_136BIT = 1,
        NO          = 2
    }; // enum class _COMMAND_RESPONSE

    static SDPORT_GET_SLOT_COUNT sdhcGetSlotCount;
    static SDPORT_GET_SLOT_CAPABILITIES sdhcGetSlotCapabilities;
    static SDPORT_INTERRUPT sdhcInterrupt;
    static SDPORT_ISSUE_REQUEST sdhcIssueRequest;
    static SDPORT_GET_RESPONSE sdhcGetResponse;
    static SDPORT_REQUEST_DPC sdhcRequestDpc;
    static SDPORT_TOGGLE_EVENTS sdhcToggleEvents;
    static SDPORT_CLEAR_EVENTS sdhcClearEvents;
    static SDPORT_SAVE_CONTEXT sdhcSaveContext;
    static SDPORT_RESTORE_CONTEXT sdhcRestoreContext;
    static SDPORT_INITIALIZE sdhcInitialize;
    static SDPORT_ISSUE_BUS_OPERATION sdhcIssueBusOperation;
    static SDPORT_GET_CARD_DETECT_STATE sdhcGetCardDetectState;
    static SDPORT_GET_WRITE_PROTECT_STATE sdhcGetWriteProtectState;
    static SDPORT_PO_FX_POWER_CONTROL_CALLBACK sdhcPowerControlCallback;
    static SDPORT_CLEANUP sdhcCleanup;

    _IRQL_requires_max_(APC_LEVEL)
    NTSTATUS resetHost ( SDPORT_RESET_TYPE ResetType ) throw ();

    _IRQL_requires_max_(APC_LEVEL)
    NTSTATUS setClock ( ULONG FrequencyKhz ) throw ();

    _IRQL_requires_min_(DISPATCH_LEVEL)
    _HCFG unmaskInterrupts ( _HCFG Unmask ) throw ();

    _IRQL_requires_min_(DISPATCH_LEVEL)
    _HCFG maskInterrupts ( _HCFG Mask ) throw ();

    _IRQL_requires_(DISPATCH_LEVEL)
    NTSTATUS sendRequestCommand ( _Inout_ SDPORT_REQUEST* RequestPtr ) throw ();

    _IRQL_requires_(DISPATCH_LEVEL)
    NTSTATUS sendCommandInternal (
        _CMD Cmd,
        _ARG Arg,
        bool WaitCompletion
        ) throw ();

    _IRQL_requires_(DISPATCH_LEVEL)
    NTSTATUS startTransfer ( _Inout_ SDPORT_REQUEST* RequestPtr ) throw ();

    _IRQL_requires_(DISPATCH_LEVEL)
    NTSTATUS startTransferPio ( _Inout_ SDPORT_REQUEST* RequestPtr ) throw ();

    NTSTATUS transferSingleBlockPio ( _Inout_ SDPORT_REQUEST* RequestPtr ) throw ();

    NTSTATUS transferMultiBlockPio ( _Inout_ SDPORT_REQUEST* RequestPtr ) throw ();

    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS sendNoTransferCommand (
        UCHAR Cmd,
        ULONG Arg,
        SDPORT_TRANSFER_DIRECTION TransferDirection,
        SDPORT_RESPONSE_TYPE ResponseType,
        bool WaitCompletion
        ) throw ();

    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS stopTransmission ( bool WaitCompletion ) throw ()
    {
        return this->sendNoTransferCommand(
            SDCMD_STOP_TRANSMISSION,
            0,
            SdTransferDirectionUndefined,
            SdResponseTypeR1B,
            WaitCompletion);
    }

    _IRQL_requires_max_(DISPATCH_LEVEL)
    void completeRequest (
        _Inout_ SDPORT_REQUEST* RequestPtr,
        NTSTATUS Status
        ) throw ();

    _COMMAND_RESPONSE getCommandResponseFromType ( SDPORT_RESPONSE_TYPE ResponseType ) throw ();

    _CMD buildCommand (
        UCHAR Command,
        SDPORT_TRANSFER_DIRECTION TransferDirection,
        SDPORT_RESPONSE_TYPE ResponseType
        ) throw ();

    NTSTATUS prepareTransferPio ( _Inout_ SDPORT_REQUEST* RequestPtr) throw ();

    NTSTATUS waitForLastCommandCompletion () throw ();

    NTSTATUS waitForDataFlag ( _Out_opt_ ULONG* WaitTimeUsPtr ) throw ();

    NTSTATUS waitForFsmState( ULONG state ) throw ();

    NTSTATUS drainReadFifo() throw ();

    NTSTATUS getErrorStatus ( _HSTS Hsts ) throw ();

    NTSTATUS getLastCommandCompletionStatus () throw ();

    _HCFG getInterruptSourcesFromEvents ( _HSTS hsts ) throw ()
    {
        _HCFG hcfg{ 0 };
        hcfg.Fields.DataIrptEn = hsts.Fields.DataFlag;
        hcfg.Fields.SdioIrptEn = hsts.Fields.SdioIrpt;
        hcfg.Fields.BlockIrptEn = hsts.Fields.BlockIrpt;
        hcfg.Fields.BusyIrptEn = hsts.Fields.BusyIrpt;

        return hcfg;
    }

    _SDPORT_EVENTS getSdportEventsFromSdhcEvents ( _HSTS sdhcEvents ) throw ()
    {
        _SDPORT_EVENTS sdportEvents{ 0 };

        sdportEvents.Fields.CardRwEnd = sdhcEvents.Fields.BlockIrpt;
        sdportEvents.Fields.CardResponse = sdhcEvents.Fields.BusyIrpt;
        sdportEvents.Fields.BufferEmpty = sdhcEvents.Fields.DataFlag;
        sdportEvents.Fields.BufferFull = sdhcEvents.Fields.DataFlag;
        sdportEvents.Fields.Error = (sdhcEvents.AsUint32 & _HSTS::UINT32_ERROR_MASK);
        sdportEvents.Fields.CardInterrupt = sdhcEvents.Fields.SdioIrpt;

        return sdportEvents;
    }

    _SDPORT_ERRORS getSdportErrorsFromSdhcErrors ( _HSTS sdhcErrors ) throw ()
    {
        _SDPORT_ERRORS sdportErrors{ 0 };

        sdportErrors.Fields.CmdCrcError =
            (sdhcErrors.Fields.Crc7Error | sdhcErrors.Fields.Crc16Error);
        sdportErrors.Fields.CmdTimeout = sdhcErrors.Fields.CmdTimeOut;
        sdportErrors.Fields.DataCrcError =
            (sdhcErrors.Fields.Crc7Error | sdhcErrors.Fields.Crc16Error);
        sdportErrors.Fields.DataTimeout = sdhcErrors.Fields.RewTimeOut;
        sdportErrors.Fields.GenericIoError = sdhcErrors.Fields.FifoError;

        return sdportErrors;
    }

    _HSTS getSdhcEventsFromSdportEvents ( _SDPORT_EVENTS sdportEvents ) throw ()
    {
        _HSTS sdhcEvents{ 0 };

        sdhcEvents.Fields.BlockIrpt = sdportEvents.Fields.CardRwEnd;
        sdhcEvents.Fields.BusyIrpt = sdportEvents.Fields.CardResponse;
        sdhcEvents.Fields.DataFlag =
            (sdportEvents.Fields.BufferEmpty | sdportEvents.Fields.BufferFull);
        sdhcEvents.Fields.SdioIrpt = sdportEvents.Fields.CardInterrupt;

        return sdhcEvents;
    }

    _HSTS getSdhcErrorsFromSdportErrors ( _SDPORT_ERRORS sdportErrors ) throw ()
    {
        _HSTS sdhcErrors{ 0 };

        sdhcErrors.Fields.Crc7Error =
            (sdportErrors.Fields.CmdCrcError | sdportErrors.Fields.DataCrcError);
        sdhcErrors.Fields.Crc16Error =
            (sdportErrors.Fields.CmdCrcError | sdportErrors.Fields.DataCrcError);
        sdhcErrors.Fields.CmdTimeOut = sdportErrors.Fields.CmdTimeout;
        sdhcErrors.Fields.RewTimeOut = sdportErrors.Fields.DataTimeout;
        sdhcErrors.Fields.FifoError = sdportErrors.Fields.GenericIoError;

        return sdhcErrors;
    }

    _IRQL_requires_max_(APC_LEVEL)
    KAFFINITY restrictCurrentThreadToSecondaryCores() throw ()
    {
        //
        // Set thread affinity mask to restrict scheduling of the current thread
        // on any processor but CPU0.
        //
        KAFFINITY callerAffinity;
        NT_ASSERTMSG("IRQL unexpected", KeGetCurrentIrql() < DISPATCH_LEVEL);
        ULONG numCpus = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
        ULONG noCpu0AffinityMask = (~(ULONG(~0x0) << numCpus) & ULONG(~0x1));
        callerAffinity = KeSetSystemAffinityThreadEx(KAFFINITY(noCpu0AffinityMask));
        NT_ASSERTMSG("Thread affinity not set as requested", KeGetCurrentProcessorNumberEx(NULL) != 0);

        return callerAffinity;
    }

    //
    // SDHC Parameters
    //

    PHYSICAL_ADDRESS basePhysicalAddress;
    void* basePtr;
    ULONG baseSpaceSize;
    SDPORT_CAPABILITIES sdhcCapabilities;

    //
    // Crashdump mode has a hard requirement of running at CLOCK_LEVEL IRQL
    // No interrupts are allowed, no memory allocations or event signaling
    //
    const BOOLEAN crashdumpMode;

#if ENABLE_PERFORMANCE_LOGGING

    //
    // Performance Logging
    //

    struct _REQUEST_STATISTICS {
        LARGE_INTEGER StartTimestamp;
        LONGLONG FifoIoTimeTicks;
        LONGLONG FifoWaitCount;
        LONGLONG FifoWaitTimeUs;
        LONGLONG FifoMaxWaitTimeUs;
        LONGLONG FsmStateWaitTimeUs;
        LONGLONG FsmStateWaitCount;
        LONGLONG FsmStateMinWaitTimeUs;
        LONGLONG FsmStateMaxWaitTimeUs;
        LONGLONG LongFsmStateWaitCount;
        LONGLONG LongFsmStateWaitTimeUs;
        USHORT BlockCount;
    } currRequestStats;

    struct _SDHC_STATISTICS {
        LONGLONG BlocksWrittenCount;
        LONGLONG PageSized4KWritesCount;
        LONGLONG TotalFsmStateWaitTimeUs;
        LONGLONG LongFsmStateWaitCount;
        LONGLONG TotalLongFsmStateWaitTimeUs;
    } sdhcStats;

#endif // ENABLE_PERFORMANCE_LOGGING

    //
    // PIO Transfer Worker State Management
    //

    static KSTART_ROUTINE transferWorker;
    KEVENT transferWorkerStartedEvt;
    KEVENT transferWorkerShutdownEvt;
    KEVENT transferWorkerDoIoEvt;
    PKTHREAD transferThreadObjPtr;

    //
    // An outstanding transfer request that is either owned by the SDHC
    // miniport or the transfer worker thread
    //
    SDPORT_REQUEST* outstandingRequestPtr;

    //
    // Used to serialize the PASSIVE_LEVEL execution of resetHost and transfer
    // worker DoIo event
    //
    FAST_MUTEX outstandingRequestLock;

    struct _REGISTERS_DUMP {
        _REGISTERS_DUMP () throw ();
        void UpdateAll ( const SDHC* SdhcPtr ) throw ();
        void UpdateStatus ( const SDHC* SdhcPtr ) throw ();

    private:
        _CMD cmd;
        _ARG arg;
        _TOUT tout;
        _CDIV cdiv;
        _RSP0 rsp0;
        _RSP1 rsp1;
        _RSP2 rsp2;
        _RSP3 rsp3;
        _HSTS hsts;
        _VDD vdd;
        _EDM edm;
        _HCFG hcfg;
        _HBCT hbct;
        _HBLC hblc;
    } mutable registersDump;

    void updateAllRegistersDump () const throw () { this->registersDump.UpdateAll(this); }

#if ENABLE_STATUS_SAMPLING // - dump registers for debugging

    static KSTART_ROUTINE sampleStatusWorker;
    KEVENT samplingStartedEvt;
    LONG shutdownSampling;
    PKTHREAD statusSamplingThreadObjPtr;
    void updateStatusRegistersDump () const throw () { this->registersDump.UpdateStatus(this); }

#endif // ENABLE_STATUS_SAMPLING

    _IRQL_requires_max_(PASSIVE_LEVEL)
    SDHC (
        PHYSICAL_ADDRESS BasePhysicalAddress,
        void* BasePtr,
        ULONG BaseSpaceSize,
        BOOLEAN CrashdumpMode
        ) throw ();

    _IRQL_requires_max_(PASSIVE_LEVEL)
    ~SDHC () throw ();
} // class SDHC

#endif // _SDHC_HPP_
