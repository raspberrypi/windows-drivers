#ifndef _BCMAUXSPI_H_
#define _BCMAUXSPI_H_
//
// Copyright (C) Microsoft.  All rights reserved.
//
//
// Module Name:
//
//   bcmauxspi.h
//
// Abstract:
//
//   BCM AUX SPI Driver Declarations
//

//
// Macros to be used for proper PAGED/NON-PAGED code placement
//

#define AUXSPI_NONPAGED_SEGMENT_BEGIN \
    __pragma(code_seg(push)) \
    //__pragma(code_seg(.text))

#define AUXSPI_NONPAGED_SEGMENT_END \
    __pragma(code_seg(pop))

#define AUXSPI_PAGED_SEGMENT_BEGIN \
    __pragma(code_seg(push)) \
    __pragma(code_seg("PAGE"))

#define AUXSPI_PAGED_SEGMENT_END \
    __pragma(code_seg(pop))

#define AUXSPI_INIT_SEGMENT_BEGIN \
    __pragma(code_seg(push)) \
    __pragma(code_seg("INIT"))

#define AUXSPI_INIT_SEGMENT_END \
    __pragma(code_seg(pop))

#define AUXSPI_ASSERT_MAX_IRQL(Irql) NT_ASSERT(KeGetCurrentIrql() <= (Irql))

//
// If nonzero, the driver will enable the device in the AUX_ENABLES register
// if it is not already enabled. The default behavior is to fail the load
// of the driver if the device is not already enabled.
//
//   Key: Driver Parameters Subkey
//   Type: REG_DWORD
//
#define REGSTR_VAL_AUXSPI_FORCE_ENABLE L"ForceEnable"

enum : ULONG { AUXSPI_POOL_TAG = 'IPSA' };

//
// Placement new operator
//

__forceinline void* __cdecl operator new ( size_t, void* Ptr ) throw ()
{
    return Ptr;
} // operator new ( size_t, void* )

__forceinline void __cdecl operator delete ( void*, void* ) throw ()
{} // void operator delete ( void*, void* )

__forceinline void* __cdecl operator new[] ( size_t, void* Ptr ) throw ()
{
    return Ptr;
} // operator new[] ( size_t, void* )

__forceinline void __cdecl operator delete[] ( void*, void* ) throw ()
{} // void operator delete[] ( void*, void* )

//
// class AUXSPI_DEVICE
//

class AUXSPI_DEVICE {
public: // NONPAGED

    enum class _TRANSFER_STATE {
        INVALID,
        WRITE,
        READ,
        SEQUENCE_WRITE,
        SEQUENCE_READ_INIT,
        SEQUENCE_READ,
        FULL_DUPLEX,
    };

    enum class _FIFO_MODE {
        FIXED_4,
        VARIABLE_3,
        FIXED_3_SHIFTED,
        VARIABLE_2_SHIFTED,
    };

    enum class _SPI_DATA_MODE : UCHAR { Mode0, Mode1, Mode2, Mode3 };
    enum class _CHIP_SELECT_LINE : UCHAR { CE0, CE1, CE2 };

    struct _TARGET_CONTEXT {
        ULONG ClockFrequency;
        USHORT DataBitLength;
        _SPI_DATA_MODE DataMode;
        _CHIP_SELECT_LINE ChipSelectLine;
    };

    struct _CONTROL_REGS {
        BCM_AUXSPI_CNTL0_REG Cntl0Reg;
        BCM_AUXSPI_CNTL1_REG Cntl1Reg;
    };

    struct _WRITE_CONTEXT {
        const BYTE* const WriteBufferPtr;
        const size_t BytesToWrite;
        size_t BytesWritten;
    };

    struct _READ_CONTEXT {
        BYTE* const ReadBufferPtr;
        const size_t BytesToRead;
        size_t BytesRead;
    };

    struct _SEQUENCE_CONTEXT {
        PMDL CurrentWriteMdl;
        const size_t BytesToWrite;
        size_t BytesWritten;
        size_t CurrentWriteMdlOffset;

        PMDL CurrentReadMdl;
        const size_t BytesToRead;
        size_t BytesRead;
        size_t CurrentReadMdlOffset;
    };

    struct _INTERRUPT_CONTEXT {
        volatile BCM_AUX_REGISTERS* const AuxRegistersPtr;
        volatile BCM_AUXSPI_REGISTERS* const RegistersPtr;

        struct _REQUEST {
            _TRANSFER_STATE TransferState : 16;
            _FIFO_MODE FifoMode : 16;
            union {
                _WRITE_CONTEXT Write;
                _READ_CONTEXT Read;
                _SEQUENCE_CONTEXT Sequence;
            } DUMMYUNIONNAME;
            SPBREQUEST volatile SpbRequest;
            const _TARGET_CONTEXT* TargetContextPtr;

            __forceinline _REQUEST () :
                TransferState(),
                FifoMode(),
                SpbRequest(),
                TargetContextPtr()
                {}

            __forceinline _REQUEST (
                _TRANSFER_STATE TransferState_,
                _FIFO_MODE FifoMode_,
                SPBREQUEST SpbRequest_,
                const _TARGET_CONTEXT* TargetContextPtr_
                ) :
                TransferState(TransferState_),
                FifoMode(FifoMode_),
                SpbRequest(SpbRequest_),
                TargetContextPtr(TargetContextPtr_)
                {}

        } Request;

        _CONTROL_REGS ControlRegs;
        bool SpbControllerLocked;

        __forceinline _INTERRUPT_CONTEXT (
            volatile BCM_AUX_REGISTERS* auxRegistersPtr,
            volatile BCM_AUXSPI_REGISTERS* registersPtr
            ) :
            AuxRegistersPtr(auxRegistersPtr),
            RegistersPtr(registersPtr),
            ControlRegs(),
            SpbControllerLocked(false) {}
    };

    static EVT_WDF_INTERRUPT_ISR EvtInterruptIsr;
    static EVT_WDF_INTERRUPT_DPC EvtInterruptDpc;

    static EVT_SPB_CONTROLLER_LOCK EvtSpbControllerLock;
    static EVT_SPB_CONTROLLER_UNLOCK EvtSpbControllerUnlock;

    static EVT_SPB_CONTROLLER_READ EvtSpbIoRead;
    static EVT_SPB_CONTROLLER_WRITE EvtSpbIoWrite;
    static EVT_SPB_CONTROLLER_SEQUENCE EvtSpbIoSequence;
    static EVT_SPB_CONTROLLER_OTHER EvtSpbIoOther;              // FullDuplex
    static EVT_WDF_IO_IN_CALLER_CONTEXT EvtIoInCallerContext;   // FullDuplex

    static EVT_WDF_REQUEST_CANCEL EvtRequestCancel;

    __forceinline AUXSPI_DEVICE (
        WDFDEVICE WdfDevice,
        WDFINTERRUPT WdfInterrupt
        ) :
        wdfDevice(WdfDevice),
        wdfInterrupt(WdfInterrupt)
        {}

private: // NONPAGED

    static size_t writeFifo (
        volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
        _In_reads_(Length) const BYTE* WriteBufferPtr,
        size_t Length,
        _FIFO_MODE FifoMode
        );

    static size_t writeFifoMdl (
        volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
        _Inout_ PMDL* MdlPtr,
        _Inout_ size_t* OffsetPtr,
        _FIFO_MODE FifoMode
        );

    static size_t writeFifoZeros (
        volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
        size_t MaxCount,
        _FIFO_MODE FifoMode
        );

    static size_t readFifo (
        volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
        _Out_writes_to_(Length, return) BYTE* ReadBufferPtr,
        size_t Length,
        _FIFO_MODE FifoMode
        );

    static size_t readFifoMdl (
        volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
        size_t Length,
        _Inout_ PMDL* MdlPtr,
        _Inout_ size_t* OffsetPtr,
        _FIFO_MODE FifoMode
        );

    static size_t extractFifoBuffer (
        _In_reads_(BCM_AUXSPI_FIFO_DEPTH) const ULONG* FifoBuffer,
        _Out_writes_to_(Length, return) BYTE* ReadBufferPtr,
        size_t Length,
        _FIFO_MODE FifoMode
        );

    struct _FIFO_FIXED_4 {
        enum { FIFO_CAPACITY = BCM_AUXSPI_FIFO_DEPTH * sizeof(ULONG) };

        static size_t Write (
            volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
            _In_reads_(Length) const ULONG* WriteBufferPtr,
            size_t Length
            );

        static void Extract (
            _In_reads_(BCM_AUXSPI_FIFO_DEPTH) const ULONG* FifoBuffer,
            _Out_writes_(Length) ULONG* ReadBufferPtr,
            _In_range_(1, BCM_AUXSPI_FIFO_DEPTH) size_t Length
            );
    };

    struct _FIFO_VARIABLE_3 {
        enum { FIFO_CAPACITY = BCM_AUXSPI_FIFO_DEPTH * 3 };

        static size_t Write (
            volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
            _In_reads_(Length) const BYTE* WriteBufferPtr,
            size_t Length
            );

        static void Extract (
            _In_reads_(BCM_AUXSPI_FIFO_DEPTH) const ULONG* FifoBuffer,
            _Out_writes_(Length) BYTE* ReadBufferPtr,
            _In_range_(1, FIFO_CAPACITY) size_t Length
            );
    };

    //
    // The "SHIFTED" FIFO modes below are for use with data modes 1 and 3.
    // The controller starts shifting out data one bit too early, so to
    // compensate we place the data in the FIFO shifted one bit to the right.
    //

    struct _FIFO_FIXED_3_SHIFTED {
        enum { FIFO_CAPACITY = BCM_AUXSPI_FIFO_DEPTH * 3 };

        static size_t Write (
            volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
            _In_reads_(Length) const BYTE* WriteBufferPtr,
            size_t Length
            );

        static void Extract (
            _In_reads_(BCM_AUXSPI_FIFO_DEPTH) const ULONG* FifoBuffer,
            _Out_writes_(Length) BYTE* ReadBufferPtr,
            _In_range_(3, FIFO_CAPACITY) size_t Length
            );
    };

    struct _FIFO_VARIABLE_2_SHIFTED {
        enum { FIFO_CAPACITY = BCM_AUXSPI_FIFO_DEPTH * 2 };

        static size_t Write (
            volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
            _In_reads_(Length) const BYTE* WriteBufferPtr,
            size_t Length
            );

        static void Extract (
            _In_reads_(BCM_AUXSPI_FIFO_DEPTH) const ULONG* FifoBuffer,
            _Out_writes_(Length) BYTE* ReadBufferPtr,
            _In_range_(1, FIFO_CAPACITY) size_t Length
            );
    };

    static size_t copyBytesToMdl (
        _Inout_ PMDL* MdlPtr,
        _Inout_ size_t* MdlOffsetPtr,
        _In_reads_(Length) const BYTE* Buffer,
        size_t Length
        );

    static size_t copyBytesFromMdl (
        _Inout_ PMDL* MdlPtr,
        _Inout_ size_t* MdlOffsetPtr,
        _Out_writes_to_(Length, return) BYTE* Buffer,
        size_t Length
        );

    static NTSTATUS processRequestCompletion (
        const _INTERRUPT_CONTEXT* InterruptContextPtr,
        _Out_ ULONG_PTR* InformationPtr
        );

    static _CONTROL_REGS computeControlRegisters (
        const _TARGET_CONTEXT* TargetContextPtr,
        _FIFO_MODE FifoMode
        );

    static void assertCsBegin (
        volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
        const _CONTROL_REGS& ControlRegs
        );

    static void assertCsComplete (
        volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
        const _CONTROL_REGS& ControlRegs
        );

    static void deassertCs (
        volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
        BCM_AUXSPI_CNTL0_REG Cntl0Reg
        );

    static void abortTransfer ( _INTERRUPT_CONTEXT* InterruptContextPtr );

    static _FIFO_MODE selectFifoMode (
        _SPI_DATA_MODE SpiDataMode,
        size_t Length
        );

    static ULONG getMinClock ();
    __forceinline static ULONG getMaxClock () { return 20000000; /*20Mhz*/ }

    _Ret_range_(<=, 16)
    __forceinline static size_t getFifoCapacity ( _FIFO_MODE FifoMode )
    {
        switch (FifoMode) {
        case _FIFO_MODE::FIXED_4: return _FIFO_FIXED_4::FIFO_CAPACITY;
        case _FIFO_MODE::VARIABLE_3: return _FIFO_VARIABLE_3::FIFO_CAPACITY;
        case _FIFO_MODE::FIXED_3_SHIFTED: return _FIFO_FIXED_3_SHIFTED::FIFO_CAPACITY;
        case _FIFO_MODE::VARIABLE_2_SHIFTED: return _FIFO_VARIABLE_2_SHIFTED::FIFO_CAPACITY;
        default: NT_ASSERT(FALSE); return 0;
        }
    }

    static void ASSERT_ULONG_ALIGNED (const BYTE* BufferPtr, size_t Length)
    {
        UNREFERENCED_PARAMETER(BufferPtr);
        UNREFERENCED_PARAMETER(Length);
        NT_ASSERT((reinterpret_cast<UINT_PTR>(BufferPtr) &
                FILE_LONG_ALIGNMENT) == 0);
        NT_ASSERT((Length % sizeof(UINT_PTR)) == 0);
    }

    volatile BCM_AUXSPI_REGISTERS* registersPtr;
    _INTERRUPT_CONTEXT* interruptContextPtr;
    WDFDEVICE wdfDevice;
    WDFINTERRUPT wdfInterrupt;

    volatile BCM_AUX_REGISTERS* auxRegistersPtr;

public: // PAGED

    static EVT_SPB_TARGET_CONNECT EvtSpbTargetConnect;

    static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtDevicePrepareHardware;
    static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtDeviceReleaseHardware;

private: // PAGED

    _IRQL_requires_max_(PASSIVE_LEVEL)
    static NTSTATUS queryForceEnableSetting ( WDFDRIVER WdfDevice );
};

extern "C" DRIVER_INITIALIZE DriverEntry;

//
// class AUXSPI_DRIVER
//

class AUXSPI_DRIVER {
    friend DRIVER_INITIALIZE DriverEntry;

public: // NONPAGED

    __forceinline static ULONG SystemClockFrequency () { return systemClockFrequency; }

private: // NONPAGED

    static ULONG systemClockFrequency;

public: // PAGED

    static EVT_WDF_DRIVER_DEVICE_ADD EvtDriverDeviceAdd;

    static EVT_WDF_DRIVER_UNLOAD EvtDriverUnload;

    _IRQL_requires_max_(PASSIVE_LEVEL)
    static NTSTATUS QuerySystemClockFrequency (_Out_ ULONG* ClockFrequencyPtr);

    _IRQL_requires_max_(PASSIVE_LEVEL)
    static NTSTATUS OpenDevice (
        const UNICODE_STRING* FileNamePtr,
        ACCESS_MASK DesiredAccess,
        ULONG ShareAccess,
        _Outptr_ FILE_OBJECT** FileObjectPPtr
        );

    _IRQL_requires_max_(PASSIVE_LEVEL)
    static NTSTATUS SendIoctlSynchronously (
        FILE_OBJECT* FileObjectPtr,
        ULONG IoControlCode,
        _In_reads_bytes_(InputBufferLength) void* InputBufferPtr,
        ULONG InputBufferLength,
        _Out_writes_bytes_(OutputBufferLength) void* OutputBufferPtr,
        ULONG OutputBufferLength,
        BOOLEAN InternalDeviceIoControl,
        _Out_ ULONG_PTR* InformationPtr
        );

private: // PAGED
};

#endif // _BCMAUXSPI_H_
