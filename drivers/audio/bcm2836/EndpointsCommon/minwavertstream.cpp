/*++

Copyright (c) Microsoft Corporation All Rights Reserved

--*/

//4127: conditional expression is constant
//4595: illegal inline operator
#pragma warning (disable : 4127)
#pragma warning (disable : 4595)

#include <rpiwav.h>
#include <limits.h>
#include <ks.h>
#include <ntddk.h>
#include "simple.h"
#include "minwavert.h"
#include "minwavertstream.h"
#define MINWAVERTSTREAM_POOLTAG 'SRWM'

//=============================================================================
// CMiniportWaveRTStream
//=============================================================================

//=============================================================================
#pragma code_seg("PAGE")
CMiniportWaveRTStream::~CMiniportWaveRTStream
( 
    VOID 
)
/*++

Routine Description:

    Destructor for CMiniportWaveRTStream 

Arguments:

    None

Return Value:

    None

--*/
{
    PAGED_CODE();

    if (NULL != m_pMiniport)
    {
        if (m_bUnregisterStream)
        {
            m_pMiniport->StreamClosed(m_ulPin, this);
            m_bUnregisterStream = FALSE;
        }

        if (m_pMiniport->m_PwmDevice)
        {
            ObDereferenceObject(m_pMiniport->m_PwmDevice);
            m_pMiniport->m_PwmDevice = NULL;
        }

        m_pMiniport->Release();
        m_pMiniport = NULL;
    }

    if (m_pDpc)
    {
        ExFreePoolWithTag( m_pDpc, MINWAVERTSTREAM_POOLTAG );
        m_pDpc = NULL;
    }

    if (m_pWfExt)
    {
        ExFreePoolWithTag( m_pWfExt, MINWAVERTSTREAM_POOLTAG );
        m_pWfExt = NULL;
    }

    //
    // Wait for all queued DPCs.
    //
    KeFlushQueuedDpcs();

    DPF_ENTER(("[CMiniportWaveRTStream::~CMiniportWaveRTStream]"));
} 

//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
NTSTATUS
CMiniportWaveRTStream::Init
( 
    PCMiniportWaveRT           Miniport,
    PPORTWAVERTSTREAM          PortStream,
    ULONG                      Pin,
    BOOLEAN                    Capture,
    PKSDATAFORMAT              DataFormat,
    GUID                       SignalProcessingMode
)
/*++

Routine Description:

    Initializes the stream object.

Arguments:

    Miniport - pointer to the miniport class this stream belongs to

    Pin - pin number of this stream

    Capture - flag if this is capture or playback stream

    DataFormat - dataformat of this stream

    SignalProcessingMode - used to configure hardware specific processing

Return Value:

    NT status code

--*/
{
    PAGED_CODE();

    PWAVEFORMATEX pWfEx = NULL;
    NTSTATUS ntStatus = STATUS_SUCCESS;
    PFILE_OBJECT fileObject = NULL;

    DECLARE_UNICODE_STRING_SIZE(pwmDeviceName, 128);

    DPF_ENTER(("[CMiniportWaveRTStream::Init]"));

    m_pMiniport = NULL;
    m_ulPin = 0;
    m_bUnregisterStream = FALSE;
    m_ulDmaBufferSize = 0;
    m_DataBuffer = NULL;
    m_PwmState = KSSTATE_STOP;
    m_PwmInitialized = FALSE;
    m_RestartPacketNumber = 0;
    m_RestartInProgress = FALSE;
    m_ulNotificationsPerBuffer = 0;
    m_ulPacketsTransferred = 0;
    m_KsState = KSSTATE_STOP;
    m_pDpc = NULL;
    m_ullPlayPosition = 0;
    m_pWfExt = NULL;
    m_SignalProcessingMode = SignalProcessingMode;

    m_pPortStream = PortStream;

    KeQueryPerformanceCounter(&m_PerformanceCounterFrequency);

    InitializeListHead(&m_NotificationList);

    pWfEx = GetWaveFormatEx(DataFormat);
    if (NULL == pWfEx) 
    { 
        return STATUS_UNSUCCESSFUL; 
    }

    m_pMiniport = reinterpret_cast<CMiniportWaveRT*>(Miniport);
    if (m_pMiniport == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    m_pMiniport->AddRef();

    //
    // Open PWM driver if not already opened.
    //
    if (m_pMiniport->m_PwmDevice == NULL)
    {
        RtlInitUnicodeString(&pwmDeviceName, BCM_PWM_SYMBOLIC_NAME);
        ntStatus = IoGetDeviceObjectPointer(&pwmDeviceName,
            FILE_READ_DATA,
            &fileObject,
            &m_pMiniport->m_PwmDevice);

        if (!NT_SUCCESS(ntStatus))
        {
            return ntStatus;
        }

        //
        // Obtain a reference to the device object. This reference will be released
        // only when the audio buffer gets destroyed, thus ensuring that the 
        // PWM device doesn't go away while calling it.
        // 
        ntStatus = ObReferenceObjectByPointer(m_pMiniport->m_PwmDevice,
            GENERIC_WRITE,
            NULL,
            KernelMode);
        if (!NT_SUCCESS(ntStatus))
        {
            m_pMiniport->m_PwmDevice = NULL;
            if (fileObject != NULL)
            {
                ObDereferenceObject(fileObject);
            }
            return ntStatus;
        }

        if (fileObject != NULL)
        {
            ObDereferenceObject(fileObject);
        }
    }

    m_ulPin = Pin;

    if (Capture)
    {
        return STATUS_INVALID_PARAMETER;
    }

    m_pDpc = (PRKDPC)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(KDPC), MINWAVERTSTREAM_POOLTAG);
    if (!m_pDpc)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    m_pWfExt = (PWAVEFORMATEXTENSIBLE)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(WAVEFORMATEX) + pWfEx->cbSize, MINWAVERTSTREAM_POOLTAG);
    if (m_pWfExt == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(m_pWfExt, pWfEx, sizeof(WAVEFORMATEX) + pWfEx->cbSize);

    //
    // Register this stream.
    //
    ntStatus = m_pMiniport->StreamCreated(m_ulPin, this);
    if (NT_SUCCESS(ntStatus))
    {
        m_bUnregisterStream = TRUE;
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::NonDelegatingQueryInterface
( 
    REFIID  Interface,
    PVOID * Object
)
/*++

Routine Description:

    Returns the interface, if it is supported.

Arguments:

    Interface - interface GUID

    Object - interface pointer to be returned

Return Value:

    NT status code

--*/
{
    PAGED_CODE();

    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERTSTREAM(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStream))
    {
        *Object = PVOID(PMINIPORTWAVERTSTREAM(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStreamNotification))
    {
        *Object = PVOID(PMINIPORTWAVERTSTREAMNOTIFICATION(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTOutputStream))
    {
        // This interface is supported only on render streams
        *Object = PVOID(PMINIPORTWAVERTOUTPUTSTREAM(this));
    }
    else
    {
        *Object = NULL;
    }

    if (*Object)
    {
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
}

//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
NTSTATUS 
CMiniportWaveRTStream::AllocateBufferWithNotification
(
    ULONG               NotificationCount,
    ULONG               RequestedSize,
    PMDL                *AudioBufferMdl,
    ULONG               *ActualSize,
    ULONG               *OffsetFromFirstPage,
    MEMORY_CACHING_TYPE *CacheType
)
/*++

Routine Description:

    Allocates a buffer the audio stack is writing the data in.

Arguments:

    NotificationCount - number of notifications to generate per buffer

    RequestedSize - size of the buffer to allocate

    AudioBufferMdl - ptr to a MDL describing the buffer

    ActualSize - size of the allocated buffer

    OffsetFromFirstPage - offset of the buffer start in the first page of the MDL

    CacheType - caching used for the buffer

Return Value:

    NT status code

--*/
{
    PAGED_CODE();

    if ( (0 == RequestedSize) || (RequestedSize < m_pWfExt->Format.nBlockAlign) )
    { 
        return STATUS_UNSUCCESSFUL; 
    }
    
    if ((NotificationCount == 0) || (RequestedSize % NotificationCount != 0))
    {
        return STATUS_INVALID_PARAMETER;
    }
    
    RequestedSize -= RequestedSize % (m_pWfExt->Format.nBlockAlign);

    PHYSICAL_ADDRESS highAddress;
    highAddress.HighPart = 0;
    highAddress.LowPart = MAXULONG;

    PMDL pBufferMdl = m_pPortStream->AllocatePagesForMdl (highAddress, RequestedSize);

    if (NULL == pBufferMdl)
    {
        return STATUS_UNSUCCESSFUL;
    }

    // From MSDN: 
    // "Since the Windows audio stack does not support a mechanism to express memory access 
    //  alignment requirements for buffers, audio drivers must select a caching type for mapped
    //  memory buffers that does not impose platform-specific alignment requirements. In other 
    //  words, the caching type used by the audio driver for mapped memory buffers, must not make 
    //  assumptions about the memory alignment requirements for any specific platform.
    //
    //  This method maps the physical memory pages in the MDL into kernel-mode virtual memory. 
    //  Typically, the miniport driver calls this method if it requires software access to the 
    //  scatter-gather list for an audio buffer. In this case, the storage for the scatter-gather 
    //  list must have been allocated by the IPortWaveRTStream::AllocatePagesForMdl or 
    //  IPortWaveRTStream::AllocateContiguousPagesForMdl method. 
    //
    //  A WaveRT miniport driver should not require software access to the audio buffer itself."
    //   
    m_DataBuffer = (BYTE*)m_pPortStream->MapAllocatedPages(pBufferMdl, MmCached);
    if (m_DataBuffer)
    {
        m_ulNotificationsPerBuffer = NotificationCount;
        m_ulDmaBufferSize = RequestedSize;
        m_ulBytesPerPacket = m_ulDmaBufferSize / m_ulNotificationsPerBuffer;
        m_ulSamplesPerPacket = m_ulBytesPerPacket / PCMBYTESPERSAMPLE;

        *AudioBufferMdl = pBufferMdl;
        *ActualSize = RequestedSize;
        *OffsetFromFirstPage = 0;
        *CacheType = MmCached;
    }
    else
    {
        DPF(D_ERROR, ("[CMiniportWaveRTStream::AllocateBufferWithNotification] Could not allocate buffer for audio."));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
VOID 
CMiniportWaveRTStream::FreeBufferWithNotification
(
    PMDL    Mdl,
    ULONG   Size
)
/*++

Routine Description:

    Frees the audio buffer.

Arguments:

    Mdl - MDL of the buffer to free
    
    Size - size of the buffer to free
    
Return Value:

    None

--*/
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Size);

    if (Mdl != NULL)
    {
        if (m_DataBuffer != NULL)
        {
            m_pPortStream->UnmapAllocatedPages(m_DataBuffer, Mdl);
            m_DataBuffer = NULL;
        }
        
        m_pPortStream->FreePagesFromMdl(Mdl);
        Mdl = NULL;
    }

    m_ulNotificationsPerBuffer = 0;
    m_ulDmaBufferSize = 0;
    m_ulBytesPerPacket = 0;
    m_ulSamplesPerPacket = 0;

    return;
}

//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
NTSTATUS 
CMiniportWaveRTStream::RegisterNotificationEvent
(
    PKEVENT NotificationEvent
)
/*++

Routine Description:

    Registers a notification event.

Arguments:

    NotificationEvent - Notification event which should be signaled

Return Value:

    NT status code

--*/
{
    PAGED_CODE();

    NTSTATUS ntStatus = STATUS_SUCCESS;

    NOTIFICATION_LIST_ENTRY *notification = (PNOTIFICATION_LIST_ENTRY)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        sizeof(NOTIFICATION_LIST_ENTRY),
        MINWAVERT_POOLTAG);
    if (NULL == notification)
    {
        DPF(D_VERBOSE, ("[CMiniportWaveRTStream::RegisterNotificationEvent] Insufficient resources for notification"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ntStatus = PwmIoctlCall(IOCTL_BCM_PWM_REGISTER_AUDIO_NOTIFICATION, &NotificationEvent, sizeof(PKEVENT), NULL, 0);
    if (NT_SUCCESS(ntStatus))
    {
        DPF(D_VERBOSE, ("[CMiniportWaveRTStream::RegisterNotificationEvent] Successfully registered notification event 0x%p with PWM", NotificationEvent));
    }
    else
    {
        DPF(D_ERROR, ("[CMiniportWaveRTStream::RegisterNotificationEvent] Error registering notification event (0x%x)", ntStatus));
    }

    if (NT_SUCCESS(ntStatus))
    {
        notification->NotificationEvent = NotificationEvent;
        if (!IsListEmpty(&m_NotificationList))
        {
            PLIST_ENTRY currentNotificationListEntry = m_NotificationList.Flink;
            while (currentNotificationListEntry != &m_NotificationList)
            {
                PNOTIFICATION_LIST_ENTRY currentNotification = CONTAINING_RECORD(currentNotificationListEntry, NOTIFICATION_LIST_ENTRY, ListEntry);
                if (currentNotification->NotificationEvent == NotificationEvent)
                {
                    RemoveEntryList(currentNotificationListEntry);
                    ExFreePoolWithTag(notification, MINWAVERT_POOLTAG);
                    return STATUS_UNSUCCESSFUL;
                }
                currentNotificationListEntry = currentNotificationListEntry->Flink;
            }
        }
        InsertTailList(&m_NotificationList, &(notification->ListEntry));

        DPF(D_VERBOSE, ("[CMiniportWaveRTStream::RegisterNotificationEvent] Notification event registered: 0x%p", NotificationEvent));
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
NTSTATUS 
CMiniportWaveRTStream::UnregisterNotificationEvent
(
    PKEVENT NotificationEvent
)
/*++

Routine Description:

    Unregisters a notification event.

Arguments:

    NotificationEvent - Notification event to unregister

Return Value:

    NT status code

--*/
{
    PAGED_CODE();

    NTSTATUS ntStatus = STATUS_SUCCESS;

    ntStatus = PwmIoctlCall(IOCTL_BCM_PWM_UNREGISTER_AUDIO_NOTIFICATION, &NotificationEvent, sizeof(PKEVENT), NULL, 0);
    if (NT_SUCCESS(ntStatus))
    {
        DPF(D_VERBOSE, ("[CMiniportWaveRTStream::UnregisterNotificationEvent] Successfully unregistered notification event 0x%p", NotificationEvent));
    }
    else
    {
        DPF(D_ERROR, ("[CMiniportWaveRTStream::UnregisterNotificationEvent] Error unregistering notification event (0x%X)", ntStatus));
    }

    if (!IsListEmpty(&m_NotificationList))
    {
        PLIST_ENTRY currentNotificationListEntry = m_NotificationList.Flink;
        PLIST_ENTRY nextNotificationListEntry = NULL;
        while (currentNotificationListEntry != &m_NotificationList)
        {
            PNOTIFICATION_LIST_ENTRY currentNotfication = CONTAINING_RECORD(currentNotificationListEntry, NOTIFICATION_LIST_ENTRY, ListEntry);
            if (currentNotfication->NotificationEvent == NotificationEvent)
            {
                nextNotificationListEntry = currentNotificationListEntry->Flink;
                RemoveEntryList(currentNotificationListEntry);
                ExFreePoolWithTag(currentNotfication, MINWAVERT_POOLTAG);
                DPF(D_VERBOSE, ("[CMiniportWaveRTStream::UnregisterNotificationEvent] Notification event (0x%p) unregistered", NotificationEvent));
                currentNotificationListEntry = nextNotificationListEntry;
            }
            else
            {
                currentNotificationListEntry = currentNotificationListEntry->Flink;
            }
        }
    }

    return ntStatus;
}


//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
NTSTATUS 
CMiniportWaveRTStream::GetClockRegister
(
    PKSRTAUDIO_HWREGISTER Register
)
/*++

Routine Description:

    Provides hardware clock regster information.

Arguments:

    Register - HW clock register info

Return Value:

    NT status code

--*/
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Register);

    DPF(D_TERSE, ("[CMiniportWaveRTStream::GetClockRegister] Not supported"));

    return STATUS_NOT_IMPLEMENTED;
}

//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
NTSTATUS 
CMiniportWaveRTStream::GetPositionRegister
(
    PKSRTAUDIO_HWREGISTER Register
)
/*++

Routine Description:

    Provides hardware position regster information.

Arguments:

    Register - HW position register info

Return Value:

    NT status code

--*/
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Register);

    DPF(D_TERSE, ("[CMiniportWaveRTStream::GetPositionRegister] Not supported"));

    return STATUS_NOT_IMPLEMENTED;
}

//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
VOID 
CMiniportWaveRTStream::GetHWLatency
(
    PKSRTAUDIO_HWLATENCY  Latency
)
/*++

Routine Description:

    Provides info on the latency introduced by the hardware.

Arguments:

    Latency - HW latency info

Return Value:

    NT status code

--*/
{
    PAGED_CODE();

    ASSERT(Latency);

    Latency->ChipsetDelay = 0;
    Latency->CodecDelay = 0;
    Latency->FifoSize = 32;
}

//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
VOID 
CMiniportWaveRTStream::FreeAudioBuffer
(
    PMDL        Mdl,
    ULONG       Size
)
/*++

Routine Description:

    Frees a memory buffer.

Arguments:

    Mdl - MDL of the buffer to free
    
    Size - Size of the buffer to free

Return Value:

    None

--*/
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Mdl);
    UNREFERENCED_PARAMETER(Size);

    DPF(D_TERSE, ("[CMiniportWaveRTStream::FreeAudioBuffer] Not supported"));

    return;
}

//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
NTSTATUS 
CMiniportWaveRTStream::AllocateAudioBuffer
(
    ULONG                   RequestedSize,
    PMDL                   *AudioBufferMdl,
    ULONG                  *ActualSize,
    ULONG                  *OffsetFromFirstPage,
    MEMORY_CACHING_TYPE    *CacheType
)
/*++

Routine Description:

    Allocates a buffer the audio stack is writing the data in.

Arguments:

    RequestedSize - size of the buffer to allocate

    AudioBufferMdl - ptr to a MDL describing the buffer

    ActualSize - size of the allocated buffer

    OffsetFromFirstPage - offset of the buffer start in the first page of the MDL

    CacheType - caching used for the buffer

Return Value:

    NT status code

--*/
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(RequestedSize);
    UNREFERENCED_PARAMETER(AudioBufferMdl);
    UNREFERENCED_PARAMETER(ActualSize);
    UNREFERENCED_PARAMETER(OffsetFromFirstPage);
    UNREFERENCED_PARAMETER(CacheType);

    DPF(D_TERSE, ("[CMiniportWaveRTStream::AllocateAudioBuffer] Not supported"));

    return STATUS_NOT_IMPLEMENTED;
}

//=============================================================================
#pragma code_seg()
_Use_decl_annotations_
NTSTATUS
CMiniportWaveRTStream::GetPosition
(
    KSAUDIO_POSITION    *Position
)
/*++

Routine Description:

    Returns the current stream playback/recording position as byte offset from the beginning of the buffer.

Arguments:

    Position - position of playback/recording

Return Value:

    NT status code

--*/
{
    UNREFERENCED_PARAMETER(Position);

    DPF(D_TERSE, ("[CMiniportWaveRTStream::GetPosition] Not supported"));

    return STATUS_NOT_IMPLEMENTED;
}

#pragma code_seg()
VOID
CMiniportWaveRTStream::ConvertPCMToPWM
(
    _In_reads_(SampleCount)       PINT16  InBuffer,
    _Out_writes_all_(SampleCount) PUINT32 OutBuffer,
    _In_                          DWORD   SampleCount
)
/*++

Routine Description:

    Converts 16 bit audio samples with 16-bit valid audio data from the audio stack to 32 bit PWM samples with 11 bit valid audio data.
    Input buffer is the audio buffer filled by the audio stack, output buffer is the DMA buffer used by the PWM driver.

Arguments:

    InBuffer - 16 bit sample input buffer

    OutBuffer - 32 bit sample output buffer

    SampleCount - number of samples to convert

Return Value:

    None

--*/
{
    while (SampleCount--)
    {
        *OutBuffer++ = (*InBuffer++ / PCMTOPWMDIV) + PWMSILENCE;
    }
}

#pragma code_seg()
VOID
CMiniportWaveRTStream::SilenceToPWM
(
    _Out_writes_all_(SampleCount) PUINT32 OutBuffer,
    _In_                          DWORD   SampleCount
)
/*++

Routine Description:

    Fills a given 32 bit samples buffer with silence data.
    Output buffer is the DMA buffer used by the PWM driver.

Arguments:

    OutBuffer - 32 bit sample output buffer

    SampleCount - number of samples fill with silence data

Return Value:

    None

--*/
{
    while (SampleCount--)
    {
        *OutBuffer++ = PWMSILENCE;
    }
}

#pragma code_seg()
VOID 
CMiniportWaveRTStream::RequestNextPacket
(
    VOID
)
/*++

Routine Description:

    Notifies all registered events, which requests the next packets.

Arguments:

    None

Return Value:

    None

--*/
{
    //
    // Notify all registered listeners.
    //

    if (!IsListEmpty(&m_NotificationList))
    {
        PLIST_ENTRY currentNotificationListEntry = m_NotificationList.Flink;
        while (currentNotificationListEntry != &m_NotificationList)
        {
            PNOTIFICATION_LIST_ENTRY currentNotification = CONTAINING_RECORD(currentNotificationListEntry, NOTIFICATION_LIST_ENTRY, ListEntry);
            KeSetEvent(currentNotification->NotificationEvent, 0, FALSE);
            currentNotificationListEntry = currentNotificationListEntry->Flink;
        }
    }
}

#pragma code_seg()
_Use_decl_annotations_
VOID
CMiniportWaveRTStream::AddPacketToDma
(
    ULONG PacketNumber
)
/*++

Routine Description:

    This function adds a packet for processing by the DMA controller. This uses data provided by the PWM driver
    during the buffer configuration allocation call and saves calling into the PWM driver each time.

Arguments:

    PacketNumber - Number of the packet to add to the DMA processing

Return Value:

    None

--*/
{
    ULONG packetIndex = PacketNumber%m_PwmAudioConfig.DmaNumPackets;

    //
    // Link the packet into the DMA controllers control block list, by establishing the link to the previous packet.
    //
    // The dmaPacketLinkInfo resides in non cached memory, which could not be used with InterlockedExchange.
    // Since the linkPtr points to the NEXTCONBK field of a DMA control block, the pointer points to a 32 bit
    // aligned memory location and the access is atomic by default.
    //

    ASSERT((ULONG *)(m_PwmAudioConfig.DmaPacketLinkInfo[packetIndex].LinkPtr));
    *((ULONG *)(m_PwmAudioConfig.DmaPacketLinkInfo[packetIndex].LinkPtr)) = m_PwmAudioConfig.DmaPacketLinkInfo[packetIndex].LinkValue;

    //
    // One more packet in the DMA controllers list to process.
    //

    InterlockedIncrement((LONG*)m_PwmAudioConfig.DmaPacketsInUse);
}



#pragma code_seg()
_Use_decl_annotations_
NTSTATUS 
CMiniportWaveRTStream::SetWritePacket
(
    ULONG PacketNumber,
    DWORD Flags,
    ULONG EosPacketLength
)
/*++

Routine Description:

    This function is called by the audio stack, when packet with PacketNumber_ has been written into the audio buffer.
    The function converts the data and adds the packet to DMA processing. To keep the DMA buffer filled with enough packets,
    it implements also logic for requesting more packets if the PWM DMA logic detects that the number of packets falls
    below a certain threshold. This function does also handle the processing required when the DMA processing detects
    an underflow condition, which is signaled by a flag.

Arguments:

    PacketNumber - Number of the packet to add to the DMA processing

    Flags - flags special processing requirements as end of stream

    EosPacketLength - length of valid data in the last packet of the stream

Return Value:

    NT status code

--*/
{
    NTSTATUS ntStatus = STATUS_SUCCESS;
    ULONG orgPacketNumber = PacketNumber;

    LARGE_INTEGER currentTime = KeQueryPerformanceCounter(NULL);

    //
    // Update positions.
    //
    if (m_pMiniport->m_PwmDevice && m_KsState == KSSTATE_RUN)
    {
        UpdatePosition();
    }

    ULONG sampleCount = 0;

    if (!(Flags & KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM))
    {
        sampleCount = m_ulSamplesPerPacket;
    }
    else
    {
        sampleCount = EosPacketLength / PCMBYTESPERSAMPLE;
        ASSERT(m_ulSamplesPerPacket >= sampleCount);
    }

    //
    // Copy data into the PWM DMA buffer
    //
    if (m_pMiniport->m_PwmDevice && m_KsState == KSSTATE_RUN)
    {
        //
        // Process restart request.
        //
        if (m_RestartInProgress == FALSE && *m_PwmAudioConfig.DmaRestartRequired == TRUE)
        {
            DPF(D_TERSE, ("[CMiniportWaveRTStream::SetWritePacket] Restart required at packet %d after %d packets. Last SetWritePacket call %d msec ago.",
                orgPacketNumber, (orgPacketNumber - m_RestartPacketNumber), (ULONG)((currentTime.QuadPart - m_LastSetWritePacket.QuadPart) / (double)m_PerformanceCounterFrequency.QuadPart * 1000.0)));

            m_PwmState = KSSTATE_STOP;
            m_RestartPacketNumber = PacketNumber;
            m_RestartInProgress = TRUE;

            //
            // Unlink all packets.
            //
            for (ULONG packetIndex = 0; packetIndex < m_PwmAudioConfig.DmaNumPackets; packetIndex++)
            {
                *(ULONG *)(m_PwmAudioConfig.DmaPacketLinkInfo[packetIndex].LinkPtr) = 0;
            }
            InterlockedExchange((LONG*)m_PwmAudioConfig.DmaPacketsInUse, 0);

            // Spew an event for a glitch event
            // Event type: eMINIPORT_GLITCH_REPORT
            // Parameter 1: Current linear buffer position	
            // Parameter 2: Current WaveRtBufferWritePosition	
            // Parameter 3: 1->WaveRT buffer under run, 2->Decoder errors, 3->Receive the same WareRT buffer write position twice in a row
            // Parameter 4: 0
            PADAPTERCOMMON  pAdapterComm = m_pMiniport->GetAdapterCommObj();
            pAdapterComm->WriteEtwEvent(eMINIPORT_GLITCH_REPORT,
                m_ullPlayPosition,
                0,
                1,
                0);

            //
            // Request next packet.
            //
            RequestNextPacket();

            return ntStatus;
        }

        PacketNumber = PacketNumber - m_RestartPacketNumber;
        m_LastSetWritePacket.QuadPart = currentTime.QuadPart;

        //
        // Check if there is enough space in the PWM packet buffer.
        //
        if (*m_PwmAudioConfig.DmaPacketsInUse == m_PwmAudioConfig.DmaNumPackets)
        {
            return STATUS_DATA_OVERRUN;
        }

        //
        // Typically we are called with a PacketNumber equal 1 on the first call of a playback. Sometimes (most
        // likely only during debugging) we are called with a larger PacketNumber. We put in silence into the DMA
        // buffer before the current packet. This is important since we start DMA always at packet 0 and need the
        // packets in the DMA buffer linked together. 
        //
        ULONG packetBaseIndex;
        ULONG dmaPacketBaseIndex;
        if (m_PwmState == KSSTATE_STOP)
        {
            ULONG packetIndex = 0;
            ASSERT(m_PwmAudioConfig.DmaNumPackets - 1);
            while (packetIndex < PacketNumber && packetIndex < m_PwmAudioConfig.DmaNumPackets - 1)
            {
                dmaPacketBaseIndex = (packetIndex % m_PwmAudioConfig.DmaNumPackets) * m_ulSamplesPerPacket;
                SilenceToPWM((PUINT32)m_PwmAudioConfig.DmaBuffer + dmaPacketBaseIndex, m_ulSamplesPerPacket);
                AddPacketToDma(packetIndex);
                packetIndex++;
            }

            //
            // Set initial priming.
            //
            InterlockedExchange((LONG*)m_PwmAudioConfig.DmaPacketsToPrime, m_PwmAudioConfig.DmaNumPackets / 2);
        }

        packetBaseIndex = (orgPacketNumber % m_ulNotificationsPerBuffer) * m_ulSamplesPerPacket;
        dmaPacketBaseIndex = (PacketNumber % m_PwmAudioConfig.DmaNumPackets) * m_ulSamplesPerPacket;

        ConvertPCMToPWM((PINT16)m_DataBuffer + packetBaseIndex, (PUINT32)m_PwmAudioConfig.DmaBuffer + dmaPacketBaseIndex, sampleCount);

        if ((Flags & KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM) &&
            (m_ulSamplesPerPacket > sampleCount))
        {
            SilenceToPWM((PUINT32)m_PwmAudioConfig.DmaBuffer + dmaPacketBaseIndex + sampleCount, m_ulSamplesPerPacket - sampleCount);
        }
        AddPacketToDma(PacketNumber);
        m_ulPacketsTransferred++;

        ULONG packetsToPrime = InterlockedExchange((LONG*)m_PwmAudioConfig.DmaPacketsToPrime, *((LONG*)m_PwmAudioConfig.DmaPacketsToPrime));
        if (packetsToPrime)
        {
            for (ULONG ul = 0; ul < packetsToPrime; ul++)
            {
                InterlockedDecrement((LONG*)m_PwmAudioConfig.DmaPacketsToPrime);
                RequestNextPacket();
            }
        }

        if (m_PwmState != KSSTATE_RUN)
        {
            //
            // Handle restart flags.
            //

            if (m_RestartInProgress)
            {
                m_RestartInProgress = FALSE;
                InterlockedExchange((PLONG)m_PwmAudioConfig.DmaRestartRequired, (LONG)FALSE);
            }

            //
            // Start PWM DMA.
            //

            ntStatus = PwmIoctlCall(IOCTL_BCM_PWM_START_AUDIO, NULL, 0, NULL, 0);
            if (!NT_SUCCESS(ntStatus))
            {
                DPF(D_ERROR, ("[CMiniportWaveRTStream::SetWritePacket] Could not start PWM audio DMA (0x%X)", ntStatus));
            }
            m_PwmState = KSSTATE_RUN;
        }
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg()
_Use_decl_annotations_
NTSTATUS 
CMiniportWaveRTStream::GetOutputStreamPresentationPosition
(
    KSAUDIO_PRESENTATION_POSITION *PresentationPosition
)
/*++

Routine Description:

    Returns the current position current playied back at the audio output of the system.

Arguments:

    PresentationPosition - Current position

Return Value:

    NT status code

--*/
{
    ASSERT (PresentationPosition);
    DPF_ENTER(("CMiniportWaveRTStream::GetOutputStreamPresentationPosition"));

    LARGE_INTEGER timeStamp;
    ULONGLONG ullLinearPosition = { 0 };
    ullLinearPosition = m_ullPlayPosition;
    timeStamp = m_PlayQpcTime;

    PresentationPosition->u64PositionInBlocks = ullLinearPosition * m_pWfExt->Format.nSamplesPerSec / m_pWfExt->Format.nAvgBytesPerSec;
    PresentationPosition->u64QPCPosition = (UINT64)timeStamp.QuadPart;

    // Spew an event for get presentation position event
    // Event type: eMINIPORT_GET_PRESENTATION_POSITION
    // Parameter 1: Current linear buffer position	
    // Parameter 2: the previous WaveRtBufferWritePosition that the drive received	
    // Parameter 3: Presentation position
    // Parameter 4: 0
    PADAPTERCOMMON pAdapterComm = m_pMiniport->GetAdapterCommObj();
    pAdapterComm->WriteEtwEvent(eMINIPORT_GET_PRESENTATION_POSITION,
        ullLinearPosition, 
        0,
        PresentationPosition->u64PositionInBlocks,
        0);  


    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg()
_Use_decl_annotations_
NTSTATUS 
CMiniportWaveRTStream::GetPacketCount
(
    ULONG *PacketCount
)
/*++

Routine Description:

    Returns the packet number transferred from the audio buffer to the DMA buffer.

Arguments:

    PacketCount - Packet count transfered

Return Value:

    NT status code

--*/
{
    ASSERT(PacketCount);
 
    DPF_ENTER(("CMiniportWaveRTStream::GetPacketCount"));

    *PacketCount = m_ulPacketsTransferred;

    DPF(D_BLAB, ("[CMiniportWaveRTStream::GetPacketCount] PacketCount: %d", *PacketCount));
    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
NTSTATUS 
CMiniportWaveRTStream::PwmIoctlCall
(
    ULONG IoctlCode,
    PVOID InputBuffer,
    ULONG InputBufferSize,
    PVOID OutputBuffer,
    ULONG OutputBufferSize
)
/*++

Routine Description:

    Calls into the PWM driver.

Arguments:

    IoctlCode - IOCTL code to use for the PWM driver call
    
    InputBuffer - Input buffer
    
    InputBufferSize - Size of the input buffer in byte
    
    OutputBuffer - Output buffer
    
    OutputBufferSize - Output buffer size
    
Return Value:

    NT status code

--*/
{
    PAGED_CODE();

    PIRP irp;
    KEVENT eventObject;
    IO_STATUS_BLOCK iosb;
    NTSTATUS ntStatus = STATUS_SUCCESS;

    KeInitializeEvent(&eventObject, SynchronizationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(
        IoctlCode,
        m_pMiniport->m_PwmDevice,
        InputBuffer,
        InputBufferSize,
        OutputBuffer,
        OutputBufferSize,
        FALSE,
        &eventObject,
        &iosb);
    if (irp == NULL)
    {
        return STATUS_UNSUCCESSFUL;
    }

    ntStatus = IoCallDriver(m_pMiniport->m_PwmDevice, irp);
    if (ntStatus == STATUS_PENDING)
    {
        ntStatus = KeWaitForSingleObject(&eventObject,
            Executive,
            KernelMode,
            FALSE,
            NULL);
    }

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = iosb.Status;
    }
    return ntStatus;
}


//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
NTSTATUS 
CMiniportWaveRTStream::SetState
(
    KSSTATE State
)
/*++

Routine Description:

    Sets the stream state.

Arguments:

    State - new stream state to set

Return Value:

    NT status code

--*/
{
    PAGED_CODE();

    NTSTATUS        ntStatus        = STATUS_SUCCESS;

    // Spew an event for a pin state change request from portcls
    // Event type: eMINIPORT_PIN_STATE
    // Parameter 1: Current linear buffer position	
    // Parameter 2: Current WaveRtBufferWritePosition	
    // Parameter 3: Pin State 0->KS_STOP, 1->KS_ACQUIRE, 2->KS_PAUSE, 3->KS_RUN 
    //Parameter 4: 0
    PADAPTERCOMMON  pAdapterComm = m_pMiniport->GetAdapterCommObj();
    pAdapterComm->WriteEtwEvent(eMINIPORT_PIN_STATE,
                                m_ullPlayPosition, 
                                0,
                                State,
                                0); 

    switch (State)
    {
        case KSSTATE_STOP:
            DPF(D_TERSE, ("[CMiniportWaveRTStream::SetState] KSSTATE_STOP requested"));

            if (m_ulPacketsTransferred)
            {
                DPF(D_TERSE, ("[CMiniportWaveRTStream::SetState] Packets transferred: %d", m_ulPacketsTransferred));
            }

            // Reset DMA
            m_ullPlayPosition = 0;
            m_ulPacketsTransferred = 0;

            //
            // Stop PWM
            //
            if (m_pMiniport->m_PwmDevice)
            {
                m_PwmState = KSSTATE_STOP;
                m_RestartPacketNumber = 0;
                m_RestartInProgress = FALSE;
                m_PwmInitialized = FALSE;
            }
            break;

        case KSSTATE_ACQUIRE:
            DPF(D_TERSE, ("[CMiniportWaveRTStream::SetState] KSSTATE_ACQUIRE requested"));

            if (m_pMiniport->m_PwmDevice && m_KsState == KSSTATE_STOP)
            {
                //
                // About to start, aquire PWM for audio.
                //
                ntStatus = PwmIoctlCall(IOCTL_BCM_PWM_AQUIRE_AUDIO, NULL, 0, NULL, 0);
                if (!NT_SUCCESS(ntStatus))
                {
                    DPF(D_ERROR, ("[CMiniportWaveRTStream::SetState] Could not aquire PWM for audio mode (0x%X)", ntStatus));
                    return ntStatus;
                }
            }
            else if (m_pMiniport->m_PwmDevice && m_KsState == KSSTATE_PAUSE)
            {
                //
                // About to stop. Stop audio PWM before we release the audio mode.
                //
                ntStatus = PwmIoctlCall(IOCTL_BCM_PWM_STOP_AUDIO, NULL, 0, NULL, 0);
                if (!NT_SUCCESS(ntStatus))
                {
                    DPF(D_ERROR, ("[CMiniportWaveRTStream::SetState] Could not stop audio (0x%X)", ntStatus));
                    return ntStatus;
                }

                //
                // Release PWM audio.
                //
                ntStatus = PwmIoctlCall(IOCTL_BCM_PWM_RELEASE_AUDIO, NULL, 0, NULL, 0);
                if (!NT_SUCCESS(ntStatus))
                {
                    DPF(D_ERROR, ("[CMiniportWaveRTStream::SetState] Could not release PWM audio mode (0x%X)", ntStatus));
                    return ntStatus;
                }
            }
            else
            {
                DPF(D_TERSE, ("[CMiniportWaveRTStream::SetState] Unexpected previous state: %d", m_KsState));
            }
            break;

        case KSSTATE_PAUSE:
            DPF(D_TERSE, ("[CMiniportWaveRTStream::SetState] KSSTATE_PAUSE requested"));

            //
            // Pause DMA
            //

            if (m_pMiniport->m_PwmDevice && m_KsState == KSSTATE_RUN && m_PwmState == KSSTATE_RUN)
                {
                ntStatus = PwmIoctlCall(IOCTL_BCM_PWM_PAUSE_AUDIO, NULL, 0, NULL, 0);
                if (!NT_SUCCESS(ntStatus))
                {
                    DPF(D_TERSE, ("[CMiniportWaveRTStream::SetState] Could not pause audio DMA (0x%X)", ntStatus));
                    return ntStatus;
                }
                m_PwmState = KSSTATE_PAUSE;

                if (m_ulPacketsTransferred)
                {
                    DPF(D_TERSE, ("[CMiniportWaveRTStream::SetState] Packets transferred: %d", m_ulPacketsTransferred));
                }
            }
            break;

        case KSSTATE_RUN:
            DPF(D_TERSE, ("[CMiniportWaveRTStream::SetState] KSSTATE_RUN requested"));

            //
            // Start PWM
            //
            if (m_pMiniport->m_PwmDevice && m_KsState < KSSTATE_RUN)
            {
                if (m_PwmInitialized)
                {
                    //
                    // All is initialized already, so we are just paused and resume now.
                    //
                    if (m_PwmState == KSSTATE_PAUSE)
                    {
                        DPF(D_TERSE, ("[CMiniportWaveRTStream::SetState] Audio DMA is only paused. Resume."));
                        ntStatus = PwmIoctlCall(IOCTL_BCM_PWM_RESUME_AUDIO, NULL, 0, NULL, 0);
                        if (!NT_SUCCESS(ntStatus))
                        {
                            DPF(D_ERROR, ("[CMiniportWaveRTStream::SetState] Could not resume audio DMA (0x%X)", ntStatus));
                            return ntStatus;
                        }
                        m_PwmState = KSSTATE_RUN;
                    }
                }
                else
                {
                    BCM_PWM_AUDIO_CONFIG audioConfig;

                    RtlZeroMemory(&audioConfig, sizeof(BCM_PWM_AUDIO_CONFIG));
                    audioConfig.RequestedBufferSize = m_ulDmaBufferSize * 2;
                    audioConfig.NotificationsPerBuffer = m_ulNotificationsPerBuffer;
                    audioConfig.PwmRange = PWMRANGE;
                    ntStatus = PwmIoctlCall(IOCTL_BCM_PWM_INITIALIZE_AUDIO, &audioConfig, sizeof(BCM_PWM_AUDIO_CONFIG), &m_PwmAudioConfig, sizeof(BCM_PWM_AUDIO_CONFIG));
                    if (!NT_SUCCESS(ntStatus))
                    {
                        DPF(D_ERROR, ("[CMiniportWaveRTStream::SetState] Could not initialize audio buffer configuration (0x%X)", ntStatus));
                        return ntStatus;
                    }

                    //
                    // Sanity check sample width.
                    //
                    ASSERT(PCMBYTESPERSAMPLE == m_pWfExt->Format.wBitsPerSample / 8);

                    m_PwmInitialized = TRUE;
                }
            }
            break;
    }

    m_KsState = State;

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
_Use_decl_annotations_
NTSTATUS 
CMiniportWaveRTStream::SetFormat(
    KSDATAFORMAT    *DataFormat
    )
/*++

Routine Description:

    Sets the data format for the stream.

Arguments:

    DataFormat - data format to set

Return Value:

    NT status code

--*/
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DataFormat);

    DPF(D_TERSE, ("[CMiniportWaveRTStream::SetFormat] Not supported"));

    return STATUS_NOT_SUPPORTED;
}

//=============================================================================
#pragma code_seg()
VOID 
CMiniportWaveRTStream::UpdatePosition
(
    VOID
)
/*++

Routine Description:

    Updates the playback position.

Arguments:

    None

Return Value:

    None

--*/
{
    DPF_ENTER(("[CMiniportWaveRTStream::UpdatePosition]"));
    if (m_PwmState == KSSTATE_RUN)
    {
        m_ullPlayPosition = *m_PwmAudioConfig.DmaPacketsProcessed * m_ulBytesPerPacket;
        m_PlayQpcTime = *m_PwmAudioConfig.DmaLastProcessedPacketTime;
    }
}
