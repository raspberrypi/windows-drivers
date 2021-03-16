/*
Copyright (c) 2012-2014, Broadcom Europe Ltd
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Copyright © 2015 Microsoft Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the “Software”), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#ifdef WIN32

    #ifdef WIN32_KERN
    #include <Ntifs.h>
    #include <wdf.h>
    #include <wdm.h>
    #else
    #include <Windows.h>
    #include <fcntl.h>
    #include <stdio.h>
    #endif

#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#endif

#include "interface/vchiq_arm/vchiq.h"
#include "vchiq_cfg.h"
#include "vchiq_ioctl.h"
#include "interface/vchi/vchi.h"
#include "interface/vchi/common/endian.h"
#include "interface/vcos/vcos.h"

#define IS_POWER_2(x) ((x & (x - 1)) == 0)
#define VCHIQ_MAX_INSTANCE_SERVICES 32
#define MSGBUF_SIZE (VCHIQ_MAX_MSG_SIZE + sizeof(VCHIQ_HEADER_T))

#define EWOULDBLOCK     140

#ifdef WIN32

// Stub definition of printf
#define fprintf

__pragma(code_seg(push))
__pragma(code_seg("INIT"))

// DriverEntry implementation, this is a potential option for init once routine
DRIVER_INITIALIZE DriverEntry;

NTSTATUS DriverEntry(
    PDRIVER_OBJECT  DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    return STATUS_SUCCESS;
}

__pragma(code_seg(pop))

__pragma(code_seg(push))

IO_STATUS_BLOCK g_errno;

int send_ioctl_sync(
    HANDLE device,
    DWORD ioctl_code,
    VOID* input_buffer,
    DWORD input_buffer_size,
    _Out_bytecap_(output_buffer_size) VOID* output_buffer,
    DWORD output_buffer_size,
    IO_STATUS_BLOCK* io_status_block
    )
{
    NTSTATUS status;
    HANDLE signal_ioctl_event;
    OBJECT_ATTRIBUTES event_attr;

    InitializeObjectAttributes(
        &event_attr, 
        NULL, 
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL, 
        NULL);

    status = ZwCreateEvent(
        &signal_ioctl_event,
        EVENT_ALL_ACCESS,
        &event_attr,
        NotificationEvent,
        FALSE);
    if (!NT_SUCCESS(status)) {
        return -1;
    }

    status = ZwDeviceIoControlFile(
        device,
        signal_ioctl_event,
        NULL,
        NULL,
        io_status_block,
        ioctl_code,
        input_buffer,
        input_buffer_size,
        output_buffer,
        output_buffer_size);
    if (!NT_SUCCESS(status)) {
        ZwClose(signal_ioctl_event);
        return -1;
    }

    if (status == STATUS_PENDING) {
        status = ZwWaitForSingleObject(signal_ioctl_event, FALSE, NULL);
        if (status != STATUS_SUCCESS) {
            ZwClose(signal_ioctl_event);
            return -1;
        }
    }

    ZwClose(signal_ioctl_event);

    if (NT_SUCCESS(io_status_block->Status)) {
        return 0;
    }

    return -1;
};

int send_ioctl_buffer(
    HANDLE device,
    DWORD ioctl_code,
    VOID* ioctl_buffer
    )
{
    IO_STATUS_BLOCK io_status_block;

    switch (ioctl_code)
    {
    case VCHIQ_IOC_CREATE_SERVICE:
        {
            return send_ioctl_sync(
                device,
                ioctl_code,
                ioctl_buffer,
                sizeof(VCHIQ_CREATE_SERVICE_T),
                NULL,
                0,
                &io_status_block);
        }
    case VCHIQ_IOC_QUEUE_MESSAGE:
        {
            return send_ioctl_sync(
                device,
                ioctl_code,
                ioctl_buffer,
                sizeof(VCHIQ_QUEUE_MESSAGE_T),
                NULL,
                0,
                &io_status_block);
        }
    case VCHIQ_IOC_QUEUE_BULK_TRANSMIT:
        {
            VCHIQ_QUEUE_BULK_TRANSFER_T* args = ioctl_buffer;

            return send_ioctl_sync(
                device,
                ioctl_code,
                args->data,
                args->size,
                args,
                sizeof(VCHIQ_QUEUE_BULK_TRANSFER_T),
                &io_status_block);
        }
    case VCHIQ_IOC_QUEUE_BULK_RECEIVE:
        {
            VCHIQ_QUEUE_BULK_TRANSFER_T* args = ioctl_buffer;

            return send_ioctl_sync(
                device,
                ioctl_code,
                args,
                sizeof(VCHIQ_QUEUE_BULK_TRANSFER_T),
                args->data,
                args->size,
                &io_status_block);
        }
    case VCHIQ_IOC_AWAIT_COMPLETION:
        {
            ULONG total_message = 0;

            if (send_ioctl_sync(
                device,
                ioctl_code,
                ioctl_buffer,
                sizeof(VCHIQ_AWAIT_COMPLETION_T),
                &total_message,
                sizeof(total_message),
                &io_status_block) == 0) {
                return total_message;
            }
            return 0;
        }
    case VCHIQ_IOC_GET_CONFIG:
        {
            return send_ioctl_sync(
                device,
                ioctl_code,
                ioctl_buffer,
                sizeof(VCHIQ_GET_CONFIG_T),
                NULL,
                0,
                &io_status_block);
        }
    case VCHIQ_IOC_SET_SERVICE_OPTION:
        {
            return send_ioctl_sync(
                device,
                ioctl_code,
                ioctl_buffer,
                sizeof(VCHIQ_SET_SERVICE_OPTION_T),
                ioctl_buffer,
                sizeof(VCHIQ_SET_SERVICE_OPTION_T),
                &io_status_block);
        }
    case VCHIQ_IOC_DUMP_PHYS_MEM:
        {
            return send_ioctl_sync(
                device,
                ioctl_code,
                NULL,
                0,
                ioctl_buffer,
                sizeof(VCHIQ_DUMP_MEM_T),
                &io_status_block);
        }
    case VCHIQ_IOC_DEQUEUE_MESSAGE:
    {
        // For VCHIQ_IOC_DEQUEUE_MESSAGE, megative value means error
        int total_message = -1;

        if (send_ioctl_sync(
            device,
            ioctl_code,
            ioctl_buffer,
            sizeof(VCHIQ_DEQUEUE_MESSAGE_T),
            &total_message,
            sizeof(total_message),
            &io_status_block) == 0) {
            g_errno = io_status_block;
            return total_message;
        }

        g_errno = io_status_block;

        return -1;
    }
    default:
        // Return non-zero, for linux zero means success
        return 1;
    }
};

int send_ioctl_func (
    HANDLE device,
    DWORD ioctl_code,
    ...
    )
{
    int return_value;
    va_list pl;

    va_start (pl, ioctl_code);

    switch (ioctl_code)
    {
    case VCHIQ_IOC_CONNECT:
    case VCHIQ_IOC_SHUTDOWN:
    case VCHIQ_IOC_REMOVE_SERVICE:
    case VCHIQ_IOC_GET_CLIENT_ID:
    case VCHIQ_IOC_CLOSE_SERVICE:
    case VCHIQ_IOC_USE_SERVICE:
    case VCHIQ_IOC_RELEASE_SERVICE:
    case VCHIQ_IOC_LIB_VERSION:
    case VCHIQ_IOC_CLOSE_DELIVERED:
        {
            unsigned int val;
            NTSTATUS status;
            IO_STATUS_BLOCK io_status_block;
            HANDLE signal_ioctl_event;
            OBJECT_ATTRIBUTES event_attr;

            return_value = -1;

            InitializeObjectAttributes(
                &event_attr, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

            status = ZwCreateEvent(
                &signal_ioctl_event,
                EVENT_ALL_ACCESS,
                &event_attr,
                SynchronizationEvent,
                FALSE);
            if (!NT_SUCCESS(status)) {
                break;
            }

            __pragma(prefast(suppress:25058, "Casting within va_arg"))
            val = va_arg(pl, unsigned int);
            
            status = ZwDeviceIoControlFile(
                device,
                signal_ioctl_event,
                NULL,
                NULL,
                &io_status_block,
                ioctl_code,
                &val,
                sizeof(val),
                NULL,
                0);
            if (!NT_SUCCESS(status)) {
                ZwClose(signal_ioctl_event);
                break;
            }

            status = ZwWaitForSingleObject(signal_ioctl_event, FALSE, NULL);
            if (status != STATUS_SUCCESS) {
                ZwClose(signal_ioctl_event);
                break;
            }

            ZwClose(signal_ioctl_event);

            if (NT_SUCCESS(io_status_block.Status)) {
                return_value = 0;
            }

            break;
        }
    default: 
        {
            void* val;

            val = va_arg(pl, void*);

            return_value =  send_ioctl_buffer(device, ioctl_code, val);
            break;
        }
    }

    va_end(pl);

    return return_value;
}

// Call ioctl only once for Windows instead of retrying
#define RETRY(r,x)  r = x
#define ioctl(a, b ,c) send_ioctl_func(a, b, c, NULL)

#else
#define RETRY(r,x) do { r = x; } while ((r == -1) && (errno == EINTR))
#endif

#define VCOS_LOG_CATEGORY (&vchiq_lib_log_category)

typedef struct vchiq_service_struct
{
   VCHIQ_SERVICE_BASE_T base;
   VCHIQ_SERVICE_HANDLE_T handle;
   VCHIQ_SERVICE_HANDLE_T lib_handle;
#ifdef WIN32
   HANDLE fd;
#else
   int fd;
#endif
   VCHI_CALLBACK_T vchi_callback;
   void *peek_buf;
   int peek_size;
   int client_id;
#ifdef WIN32
   unsigned int is_client;
#else
   char is_client;
#endif
} VCHIQ_SERVICE_T;

typedef struct vchiq_service_struct VCHI_SERVICE_T;

struct vchiq_instance_struct
{
#ifdef WIN32
   HANDLE fd;
#else
   int fd;
#endif
   int initialised;
   int connected;
   int use_close_delivered;
   VCOS_THREAD_T completion_thread;
   VCOS_MUTEX_T mutex;
   int used_services;
   VCHIQ_SERVICE_T services[VCHIQ_MAX_INSTANCE_SERVICES];
} vchiq_instance;

typedef struct vchiq_instance_struct VCHI_STATE_T;

/* Local data */
static VCOS_LOG_LEVEL_T vchiq_default_lib_log_level = VCOS_LOG_WARN;
static VCOS_LOG_CAT_T vchiq_lib_log_category;
static VCOS_MUTEX_T vchiq_lib_mutex;
static void *free_msgbufs;
static unsigned int handle_seq;

vcos_static_assert(IS_POWER_2(VCHIQ_MAX_INSTANCE_SERVICES));

/* Local utility functions */
static VCHIQ_INSTANCE_T
vchiq_lib_init(void);

static void *completion_thread(void *);

static VCHIQ_STATUS_T
create_service(VCHIQ_INSTANCE_T instance,
   const VCHIQ_SERVICE_PARAMS_T *params,
   VCHI_CALLBACK_T vchi_callback,
   int is_open,
   VCHIQ_SERVICE_HANDLE_T *phandle);

static int
fill_peek_buf(VCHI_SERVICE_T *service,
   VCHI_FLAGS_T flags);

static void *
alloc_msgbuf(void);

static void
free_msgbuf(void *buf);

static __inline int
is_valid_instance(VCHIQ_INSTANCE_T instance)
{
   return (instance == &vchiq_instance) && (instance->initialised > 0);
}

static inline VCHIQ_SERVICE_T *
handle_to_service(VCHIQ_SERVICE_HANDLE_T handle)
{
   return &vchiq_instance.services[handle & (VCHIQ_MAX_INSTANCE_SERVICES - 1)];
}

static VCHIQ_SERVICE_T *
find_service_by_handle(VCHIQ_SERVICE_HANDLE_T handle)
{
   VCHIQ_SERVICE_T *service;

   service = handle_to_service(handle);
   if (service && (service->lib_handle != handle))
      service = NULL;

   if (!service)
      vcos_log_info("Invalid service handle 0x%x", handle);

   return service;
}

/*
 * VCHIQ API
 */

VCHIQ_STATUS_T
vchiq_initialise(VCHIQ_INSTANCE_T *pinstance)
{
   VCHIQ_INSTANCE_T instance;

   instance = vchiq_lib_init();

   vcos_log_trace( "%s: returning instance handle %p", __func__, instance );

   *pinstance = instance;

   return (instance != NULL) ? VCHIQ_SUCCESS : VCHIQ_ERROR;
}

VCHIQ_STATUS_T
vchiq_shutdown(VCHIQ_INSTANCE_T instance)
{
   vcos_log_trace( "%s called", __func__ );

   if (!is_valid_instance(instance))
      return VCHIQ_ERROR;

   vcos_mutex_lock(&instance->mutex);

   if (instance->initialised == 1)
   {
      int i;

      instance->initialised = -1; /* Enter limbo */

      /* Remove all services */

      for (i = 0; i < instance->used_services; i++)
      {
         if (instance->services[i].lib_handle != VCHIQ_SERVICE_HANDLE_INVALID)
         {
            vchiq_remove_service(instance->services[i].lib_handle);
            instance->services[i].lib_handle = VCHIQ_SERVICE_HANDLE_INVALID;
         }
      }

      if (instance->connected)
      {
         int ret;
         RETRY(ret, ioctl(instance->fd, VCHIQ_IOC_SHUTDOWN, 0));
         vcos_assert(ret == 0);
         vcos_thread_join(&instance->completion_thread, NULL);
         instance->connected = 0;
      }
      #ifdef WIN32
        ZwClose(instance->fd);
        instance->fd = NULL;
      #else
        close(instance->fd);
        instance->fd = -1;
      #endif
   }
   else if (instance->initialised > 1)
   {
      instance->initialised--;
   }

   vcos_mutex_unlock(&instance->mutex);

   vcos_global_lock();

   if (instance->initialised == -1)
   {
      vcos_mutex_delete(&instance->mutex);
      instance->initialised = 0;
   }

   vcos_global_unlock();

   vcos_log_trace( "%s returning", __func__ );

   return VCHIQ_SUCCESS;
}

VCHIQ_STATUS_T
vchiq_connect(VCHIQ_INSTANCE_T instance)
{
   VCHIQ_STATUS_T status = VCHIQ_SUCCESS;
   VCOS_THREAD_ATTR_T attrs;
   int ret;

   vcos_log_trace( "%s called", __func__ );

   if (!is_valid_instance(instance))
      return VCHIQ_ERROR;

   vcos_mutex_lock(&instance->mutex);

   if (instance->connected)
      goto out;

   ret = ioctl(instance->fd, VCHIQ_IOC_CONNECT, 0);
   if (ret != 0)
   {
      status = VCHIQ_ERROR;
      goto out;
   }

   vcos_thread_attr_init(&attrs);
   if (vcos_thread_create(&instance->completion_thread, "VCHIQ completion",
                          &attrs, completion_thread, instance) != VCOS_SUCCESS)
   {
      status = VCHIQ_ERROR;
      goto out;
   }

   instance->connected = 1;

out:
   vcos_mutex_unlock(&instance->mutex);
   return status;
}

VCHIQ_STATUS_T
vchiq_add_service(VCHIQ_INSTANCE_T instance,
   const VCHIQ_SERVICE_PARAMS_T *params,
   VCHIQ_SERVICE_HANDLE_T *phandle)
{
   VCHIQ_STATUS_T status;

   vcos_log_trace( "%s called fourcc = 0x%08x (%c%c%c%c)",
                   __func__,
                   params->fourcc,
                   (params->fourcc >> 24) & 0xff,
                   (params->fourcc >> 16) & 0xff,
                   (params->fourcc >>  8) & 0xff,
                   (params->fourcc      ) & 0xff );

   if (!params->callback)
      return VCHIQ_ERROR;

   if (!is_valid_instance(instance))
      return VCHIQ_ERROR;

   status = create_service(instance,
      params,
      NULL/*vchi_callback*/,
      0/*!open*/,
      phandle);

   vcos_log_trace( "%s returning service handle = 0x%08x", __func__, (uint32_t)*phandle );

   return status;
}

VCHIQ_STATUS_T
vchiq_open_service(VCHIQ_INSTANCE_T instance,
   const VCHIQ_SERVICE_PARAMS_T *params,
   VCHIQ_SERVICE_HANDLE_T *phandle)
{
   VCHIQ_STATUS_T status;

   vcos_log_trace( "%s called fourcc = 0x%08x (%c%c%c%c)",
                   __func__,
                   params->fourcc,
                   (params->fourcc >> 24) & 0xff,
                   (params->fourcc >> 16) & 0xff,
                   (params->fourcc >>  8) & 0xff,
                   (params->fourcc      ) & 0xff );

   if (!params->callback)
      return VCHIQ_ERROR;

   if (!is_valid_instance(instance))
      return VCHIQ_ERROR;

   status = create_service(instance,
      params,
      NULL/*vchi_callback*/,
      1/*open*/,
      phandle);

   vcos_log_trace( "%s returning service handle = 0x%08x", __func__, (uint32_t)*phandle );

   return status;
}

VCHIQ_STATUS_T
vchiq_close_service(VCHIQ_SERVICE_HANDLE_T handle)
{
   VCHIQ_SERVICE_T *service = find_service_by_handle(handle);
   int ret;

   vcos_log_trace( "%s called service handle = 0x%08x", __func__, (uint32_t)handle );

   if (!service)
      return VCHIQ_ERROR;

   RETRY(ret,ioctl(service->fd, VCHIQ_IOC_CLOSE_SERVICE, service->handle));

   if (service->is_client)
      service->lib_handle = VCHIQ_SERVICE_HANDLE_INVALID;

   if (ret != 0)
      return VCHIQ_ERROR;

   return VCHIQ_SUCCESS;
}

VCHIQ_STATUS_T
vchiq_remove_service(VCHIQ_SERVICE_HANDLE_T handle)
{
   VCHIQ_SERVICE_T *service = find_service_by_handle(handle);
   int ret;

   vcos_log_trace( "%s called service handle = 0x%08x", __func__, (uint32_t)handle );

   if (!service)
      return VCHIQ_ERROR;

   RETRY(ret,ioctl(service->fd, VCHIQ_IOC_REMOVE_SERVICE, service->handle));

   service->lib_handle = VCHIQ_SERVICE_HANDLE_INVALID;

   if (ret != 0)
      return VCHIQ_ERROR;

   return VCHIQ_SUCCESS;
}

VCHIQ_STATUS_T
vchiq_queue_message(VCHIQ_SERVICE_HANDLE_T handle,
   const VCHIQ_ELEMENT_T *elements,
   int count)
{
   VCHIQ_SERVICE_T *service = find_service_by_handle(handle);
   VCHIQ_QUEUE_MESSAGE_T args;
   int ret;

   vcos_log_trace( "%s called service handle = 0x%08x", __func__, (uint32_t)handle );

   if (!service)
      return VCHIQ_ERROR;

   args.handle = service->handle;
   args.elements = elements;
   args.count = count;
   RETRY(ret, ioctl(service->fd, VCHIQ_IOC_QUEUE_MESSAGE, &args));

   return (ret >= 0) ? VCHIQ_SUCCESS : VCHIQ_ERROR;
}

void
vchiq_release_message(VCHIQ_SERVICE_HANDLE_T handle,
   VCHIQ_HEADER_T *header)
{
   vcos_log_trace( "%s handle=%08x, header=%p", __func__, (uint32_t)handle, header );

   free_msgbuf(header);
}

VCHIQ_STATUS_T
vchiq_queue_bulk_transmit(VCHIQ_SERVICE_HANDLE_T handle,
   const void *data,
   int size,
   void *userdata)
{
   VCHIQ_SERVICE_T *service = find_service_by_handle(handle);
   VCHIQ_QUEUE_BULK_TRANSFER_T args;
   int ret;

   vcos_log_trace( "%s called service handle = 0x%08x", __func__, (uint32_t)handle );

   if (!service)
      return VCHIQ_ERROR;

   args.handle = service->handle;
   args.data = (void *)data;
   args.size = size;
   args.userdata = userdata;
   args.mode = VCHIQ_BULK_MODE_CALLBACK;
   RETRY(ret, ioctl(service->fd, VCHIQ_IOC_QUEUE_BULK_TRANSMIT, &args));

   return (ret >= 0) ? VCHIQ_SUCCESS : VCHIQ_ERROR;
}

VCHIQ_STATUS_T
vchiq_queue_bulk_receive(VCHIQ_SERVICE_HANDLE_T handle,
   void *data,
   int size,
   void *userdata)
{
   VCHIQ_SERVICE_T *service = find_service_by_handle(handle);
   VCHIQ_QUEUE_BULK_TRANSFER_T args;
   int ret;

   vcos_log_trace( "%s called service handle = 0x%08x", __func__, (uint32_t)handle );

   if (!service)
      return VCHIQ_ERROR;

   args.handle = service->handle;
   args.data = data;
   args.size = size;
   args.userdata = userdata;
   args.mode = VCHIQ_BULK_MODE_CALLBACK;

   RETRY(ret, ioctl(service->fd, VCHIQ_IOC_QUEUE_BULK_RECEIVE, &args));

   return (ret >= 0) ? VCHIQ_SUCCESS : VCHIQ_ERROR;
}

VCHIQ_STATUS_T
vchiq_queue_bulk_transmit_handle(VCHIQ_SERVICE_HANDLE_T handle,
   VCHI_MEM_HANDLE_T memhandle,
   const void *offset,
   int size,
   void *userdata)
{
   UNREFERENCED_PARAMETER(memhandle);

   vcos_assert(memhandle == VCHI_MEM_HANDLE_INVALID);

   vcos_log_trace( "%s called service handle = 0x%08x", __func__, (uint32_t)handle );

   return vchiq_queue_bulk_transmit(handle, offset, size, userdata);
}

VCHIQ_STATUS_T
vchiq_queue_bulk_receive_handle(VCHIQ_SERVICE_HANDLE_T handle,
   VCHI_MEM_HANDLE_T memhandle,
   void *offset,
   int size,
   void *userdata)
{
   UNREFERENCED_PARAMETER(memhandle);

   vcos_assert(memhandle == VCHI_MEM_HANDLE_INVALID);

   vcos_log_trace( "%s called service handle = 0x%08x", __func__, (uint32_t)handle );

   return vchiq_queue_bulk_receive(handle, offset, size, userdata);
}

VCHIQ_STATUS_T
vchiq_bulk_transmit(VCHIQ_SERVICE_HANDLE_T handle,
   const void *data,
   int size,
   void *userdata,
   VCHIQ_BULK_MODE_T mode)
{
   VCHIQ_SERVICE_T *service = find_service_by_handle(handle);
   VCHIQ_QUEUE_BULK_TRANSFER_T args;
   int ret;

   vcos_log_trace( "%s called service handle = 0x%08x", __func__, (uint32_t)handle );

   if (!service)
      return VCHIQ_ERROR;

   args.handle = service->handle;
   args.data = (void *)data;
   args.size = size;
   args.userdata = userdata;
   args.mode = mode;
   RETRY(ret, ioctl(service->fd, VCHIQ_IOC_QUEUE_BULK_TRANSMIT, &args));

   return (ret >= 0) ? VCHIQ_SUCCESS : VCHIQ_ERROR;
}

VCHIQ_STATUS_T
vchiq_bulk_receive(VCHIQ_SERVICE_HANDLE_T handle,
   void *data,
   int size,
   void *userdata,
   VCHIQ_BULK_MODE_T mode)
{
   return vchiq_bulk_receive_handle(handle, VCHI_MEM_HANDLE_INVALID, data, size, userdata, mode, NULL);
}

VCHIQ_STATUS_T
vchiq_bulk_transmit_handle(VCHIQ_SERVICE_HANDLE_T handle,
   VCHI_MEM_HANDLE_T memhandle,
   const void *offset,
   int size,
   void *userdata,
   VCHIQ_BULK_MODE_T mode)
{
   UNREFERENCED_PARAMETER(memhandle);

   vcos_assert(memhandle == VCHI_MEM_HANDLE_INVALID);

   return vchiq_bulk_transmit(handle, offset, size, userdata, mode);
}

VCHIQ_STATUS_T
vchiq_bulk_receive_handle(VCHIQ_SERVICE_HANDLE_T handle,
   VCHI_MEM_HANDLE_T memhandle,
   void *offset,
   int size,
   void *userdata,
   VCHIQ_BULK_MODE_T mode,
   int (*copy_pagelist)())
{
   VCHIQ_SERVICE_T *service = find_service_by_handle(handle);
   VCHIQ_QUEUE_BULK_TRANSFER_T args;
   int ret;

   UNREFERENCED_PARAMETER(memhandle);
   UNREFERENCED_PARAMETER(copy_pagelist);

   vcos_assert(memhandle == VCHI_MEM_HANDLE_INVALID);

   vcos_log_trace( "%s called service handle = 0x%08x", __func__, (uint32_t)handle );

   if (!service)
      return VCHIQ_ERROR;

   args.handle = service->handle;
   args.data = offset;
   args.size = size;
   args.userdata = userdata;
   args.mode = mode;
   RETRY(ret, ioctl(service->fd, VCHIQ_IOC_QUEUE_BULK_RECEIVE, &args));

   return (ret >= 0) ? VCHIQ_SUCCESS : VCHIQ_ERROR;
}

int
vchiq_get_client_id(VCHIQ_SERVICE_HANDLE_T handle)
{
   VCHIQ_SERVICE_T *service = find_service_by_handle(handle);

   if (!service)
      return VCHIQ_ERROR;

   return ioctl(service->fd, VCHIQ_IOC_GET_CLIENT_ID, service->handle);
}

void *
vchiq_get_service_userdata(VCHIQ_SERVICE_HANDLE_T handle)
{
   VCHIQ_SERVICE_T *service = find_service_by_handle(handle);

   return service ? service->base.userdata : NULL;
}

int
vchiq_get_service_fourcc(VCHIQ_SERVICE_HANDLE_T handle)
{
   const VCHIQ_SERVICE_T *service = find_service_by_handle(handle);

   return service ? service->base.fourcc : 0;
}

VCHIQ_STATUS_T
vchiq_get_config(VCHIQ_INSTANCE_T instance,
   int config_size,
   VCHIQ_CONFIG_T *pconfig)
{
   VCHIQ_GET_CONFIG_T args;
   int ret;

   if (!is_valid_instance(instance))
      return VCHIQ_ERROR;

   args.config_size = config_size;
   args.pconfig = pconfig;

   RETRY(ret, ioctl(instance->fd, VCHIQ_IOC_GET_CONFIG, &args));

   return (ret >= 0) ? VCHIQ_SUCCESS : VCHIQ_ERROR;
}

int32_t
vchiq_use_service( const VCHIQ_SERVICE_HANDLE_T handle )
{
   VCHIQ_SERVICE_T *service = find_service_by_handle(handle);
   int ret;

   if (!service)
      return VCHIQ_ERROR;

   RETRY(ret,ioctl(service->fd, VCHIQ_IOC_USE_SERVICE, service->handle));
   return ret;
}

int32_t
vchiq_release_service( const VCHIQ_SERVICE_HANDLE_T handle )
{
   VCHIQ_SERVICE_T *service = find_service_by_handle(handle);
   int ret;

   if (!service)
      return VCHIQ_ERROR;

   RETRY(ret,ioctl(service->fd, VCHIQ_IOC_RELEASE_SERVICE, service->handle));
   return ret;
}

VCHIQ_STATUS_T
vchiq_set_service_option(VCHIQ_SERVICE_HANDLE_T handle,
   VCHIQ_SERVICE_OPTION_T option, int value)
{
   VCHIQ_SET_SERVICE_OPTION_T args;
   VCHIQ_SERVICE_T *service = find_service_by_handle(handle);
   int ret;

   if (!service)
      return VCHIQ_ERROR;

   args.handle = service->handle;
   args.option = option;
   args.value  = value;

   RETRY(ret, ioctl(service->fd, VCHIQ_IOC_SET_SERVICE_OPTION, &args));

   return (ret >= 0) ? VCHIQ_SUCCESS : VCHIQ_ERROR;
}

/*
 * VCHI API
 */

/* ----------------------------------------------------------------------
 * return pointer to the mphi message driver function table
 * -------------------------------------------------------------------- */
const VCHI_MESSAGE_DRIVER_T *
vchi_mphi_message_driver_func_table( void )
{
   return NULL;
}

/* ----------------------------------------------------------------------
 * return a pointer to the 'single' connection driver fops
 * -------------------------------------------------------------------- */
const VCHI_CONNECTION_API_T *
single_get_func_table( void )
{
   return NULL;
}

VCHI_CONNECTION_T *
vchi_create_connection( const VCHI_CONNECTION_API_T * function_table,
   const VCHI_MESSAGE_DRIVER_T * low_level )
{
   vcos_unused(function_table);
   vcos_unused(low_level);

   return NULL;
}

/***********************************************************
 * Name: vchi_msg_peek
 *
 * Arguments:  const VCHI_SERVICE_HANDLE_T handle,
 *             void **data,
 *             uint32_t *msg_size,
 *             VCHI_FLAGS_T flags
 *
 * Description: Routine to return a pointer to the current message (to allow in place processing)
 *              The message can be removed using vchi_msg_remove when you're finished
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t
vchi_msg_peek( VCHI_SERVICE_HANDLE_T handle,
   void **data,
   uint32_t *msg_size,
   VCHI_FLAGS_T flags )
{
   VCHI_SERVICE_T *service = find_service_by_handle(handle);
   int ret;

   if (!service)
      return VCHIQ_ERROR;

   ret = fill_peek_buf(service, flags);

   if (ret == 0)
   {
      *data = service->peek_buf;
      *msg_size = service->peek_size;
   }

   return ret;
}

/***********************************************************
 * Name: vchi_msg_remove
 *
 * Arguments:  const VCHI_SERVICE_HANDLE_T handle,
 *
 * Description: Routine to remove a message (after it has been read with vchi_msg_peek)
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t
vchi_msg_remove( VCHI_SERVICE_HANDLE_T handle )
{
   VCHI_SERVICE_T *service = find_service_by_handle(handle);

   if (!service)
      return VCHIQ_ERROR;

   /* Why would you call vchi_msg_remove without calling vchi_msg_peek first? */
   vcos_assert(service->peek_size >= 0);

   /* Invalidate the content but reuse the buffer */
   service->peek_size = -1;

   return 0;
}

/***********************************************************
 * Name: vchi_msg_queue
 *
 * Arguments:  VCHI_SERVICE_HANDLE_T handle,
 *             const void *data,
 *             uint32_t data_size,
 *             VCHI_FLAGS_T flags,
 *             void *msg_handle,
 *
 * Description: Thin wrapper to queue a message onto a connection
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t
vchi_msg_queue( VCHI_SERVICE_HANDLE_T handle,
   const void * data,
   uint32_t data_size,
   VCHI_FLAGS_T flags,
   void * msg_handle )
{
   VCHI_SERVICE_T *service = find_service_by_handle(handle);
   VCHIQ_QUEUE_MESSAGE_T args;
   VCHIQ_ELEMENT_T element;
   int ret;

   UNREFERENCED_PARAMETER(flags);

   element.data = data;
   element.size = data_size;

   vcos_unused(msg_handle);
   vcos_assert(flags == VCHI_FLAGS_BLOCK_UNTIL_QUEUED);

   if (!service)
      return VCHIQ_ERROR;

   args.handle = service->handle;
   args.elements = &element;
   args.count = 1;
   RETRY(ret, ioctl(service->fd, VCHIQ_IOC_QUEUE_MESSAGE, &args));

   return ret;
}

/***********************************************************
 * Name: vchi_bulk_queue_receive
 *
 * Arguments:  VCHI_BULK_HANDLE_T handle,
 *             void *data_dst,
 *             const uint32_t data_size,
 *             VCHI_FLAGS_T flags
 *             void *bulk_handle
 *
 * Description: Routine to setup a rcv buffer
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t
vchi_bulk_queue_receive( VCHI_SERVICE_HANDLE_T handle,
   void * data_dst,
   uint32_t data_size,
   VCHI_FLAGS_T flags,
   void * bulk_handle )
{
   VCHI_SERVICE_T *service = find_service_by_handle(handle);
   VCHIQ_QUEUE_BULK_TRANSFER_T args;
   int ret;

   if (!service)
      return VCHIQ_ERROR;

   switch ((int)flags) {
   case VCHI_FLAGS_CALLBACK_WHEN_OP_COMPLETE | VCHI_FLAGS_BLOCK_UNTIL_QUEUED:
      args.mode = VCHIQ_BULK_MODE_CALLBACK;
      break;
   case VCHI_FLAGS_BLOCK_UNTIL_OP_COMPLETE:
      args.mode = VCHIQ_BULK_MODE_BLOCKING;
      break;
   case VCHI_FLAGS_BLOCK_UNTIL_QUEUED:
   case VCHI_FLAGS_NONE:
      args.mode = VCHIQ_BULK_MODE_NOCALLBACK;
      break;
   default:
      vcos_assert(0);
      break;
   }

   args.handle = service->handle;
   args.data = data_dst;
   args.size = data_size;
   args.userdata = bulk_handle;
   RETRY(ret, ioctl(service->fd, VCHIQ_IOC_QUEUE_BULK_RECEIVE, &args));

   return ret;
}

/***********************************************************
 * Name: vchi_bulk_queue_transmit
 *
 * Arguments:  VCHI_BULK_HANDLE_T handle,
 *             const void *data_src,
 *             uint32_t data_size,
 *             VCHI_FLAGS_T flags,
 *             void *bulk_handle
 *
 * Description: Routine to transmit some data
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t
vchi_bulk_queue_transmit( VCHI_SERVICE_HANDLE_T handle,
   const void * data_src,
   uint32_t data_size,
   VCHI_FLAGS_T flags,
   void * bulk_handle )
{
   VCHI_SERVICE_T *service = find_service_by_handle(handle);
   VCHIQ_QUEUE_BULK_TRANSFER_T args;
   int ret;

   if (!service)
      return VCHIQ_ERROR;

   switch ((int)flags) {
   case VCHI_FLAGS_CALLBACK_WHEN_OP_COMPLETE | VCHI_FLAGS_BLOCK_UNTIL_QUEUED:
      args.mode = VCHIQ_BULK_MODE_CALLBACK;
      break;
   case VCHI_FLAGS_BLOCK_UNTIL_DATA_READ:
   case VCHI_FLAGS_BLOCK_UNTIL_OP_COMPLETE:
      args.mode = VCHIQ_BULK_MODE_BLOCKING;
      break;
   case VCHI_FLAGS_BLOCK_UNTIL_QUEUED:
   case VCHI_FLAGS_NONE:
      args.mode = VCHIQ_BULK_MODE_NOCALLBACK;
      break;
   default:
      vcos_assert(0);
      break;
   }

   args.handle = service->handle;
   args.data = (void *)data_src;
   args.size = data_size;
   args.userdata = bulk_handle;
   RETRY(ret, ioctl(service->fd, VCHIQ_IOC_QUEUE_BULK_TRANSMIT, &args));

   return ret;
}

/***********************************************************
 * Name: vchi_msg_dequeue
 *
 * Arguments:  VCHI_SERVICE_HANDLE_T handle,
 *             void *data,
 *             uint32_t max_data_size_to_read,
 *             uint32_t *actual_msg_size
 *             VCHI_FLAGS_T flags
 *
 * Description: Routine to dequeue a message into the supplied buffer
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t
vchi_msg_dequeue( VCHI_SERVICE_HANDLE_T handle,
   void *data,
   uint32_t max_data_size_to_read,
   uint32_t *actual_msg_size,
   VCHI_FLAGS_T flags )
{
   VCHI_SERVICE_T *service = find_service_by_handle(handle);
   VCHIQ_DEQUEUE_MESSAGE_T args;
   int ret;

   vcos_assert(flags == VCHI_FLAGS_NONE || flags == VCHI_FLAGS_BLOCK_UNTIL_OP_COMPLETE);

   if (!service)
      return VCHIQ_ERROR;

   if (service->peek_size >= 0)
   {
      vcos_log_error("vchi_msg_dequeue -> using peek buffer\n");
      if ((uint32_t)service->peek_size <= max_data_size_to_read)
      {
         memcpy(data, service->peek_buf, service->peek_size);
         *actual_msg_size = service->peek_size;
         /* Invalidate the peek data, but retain the buffer */
         service->peek_size = -1;
         ret = 0;
      }
      else
      {
         ret = -1;
      }
   }
   else
   {
      args.handle = service->handle;
      args.blocking = (flags == VCHI_FLAGS_BLOCK_UNTIL_OP_COMPLETE);
      args.bufsize = max_data_size_to_read;
      args.buf = data;
      RETRY(ret, ioctl(service->fd, VCHIQ_IOC_DEQUEUE_MESSAGE, &args));
      if (ret >= 0)
      {
         *actual_msg_size = ret;
         ret = 0;
      }
   }

   if ((ret < 0) && (g_errno.Status != EWOULDBLOCK)) {
       __debugbreak();
   }   

   return ret;
}

/***********************************************************
 * Name: vchi_msg_queuev
 *
 * Arguments:  VCHI_SERVICE_HANDLE_T handle,
 *             const void *data,
 *             uint32_t data_size,
 *             VCHI_FLAGS_T flags,
 *             void *msg_handle
 *
 * Description: Thin wrapper to queue a message onto a connection
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/

vcos_static_assert(sizeof(VCHI_MSG_VECTOR_T) == sizeof(VCHIQ_ELEMENT_T));
vcos_static_assert(offsetof(VCHI_MSG_VECTOR_T, vec_base) == offsetof(VCHIQ_ELEMENT_T, data));
vcos_static_assert(offsetof(VCHI_MSG_VECTOR_T, vec_len) == offsetof(VCHIQ_ELEMENT_T, size));

int32_t
vchi_msg_queuev( VCHI_SERVICE_HANDLE_T handle,
   VCHI_MSG_VECTOR_T * vector,
   uint32_t count,
   VCHI_FLAGS_T flags,
   void *msg_handle )
{
   VCHI_SERVICE_T *service = find_service_by_handle(handle);
   VCHIQ_QUEUE_MESSAGE_T args;
   int ret;

   UNREFERENCED_PARAMETER(flags);

   vcos_unused(msg_handle);

   vcos_assert(flags == VCHI_FLAGS_BLOCK_UNTIL_QUEUED);

   if (!service)
      return VCHIQ_ERROR;

   args.handle = service->handle;
   args.elements = (const VCHIQ_ELEMENT_T *)vector;
   args.count = count;
   RETRY(ret, ioctl(service->fd, VCHIQ_IOC_QUEUE_MESSAGE, &args));

   return ret;
}

/***********************************************************
 * Name: vchi_held_msg_release
 *
 * Arguments:  VCHI_HELD_MSG_T *message
 *
 * Description: Routine to release a held message (after it has been read with vchi_msg_hold)
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t
vchi_held_msg_release( VCHI_HELD_MSG_T *message )
{
   int ret = -1;

   if (message && message->message && !message->service)
   {
      free_msgbuf(message->message);
      ret = 0;
   }

   return ret;
}

/***********************************************************
 * Name: vchi_msg_hold
 *
 * Arguments:  VCHI_SERVICE_HANDLE_T handle,
 *             void **data,
 *             uint32_t *msg_size,
 *             VCHI_FLAGS_T flags,
 *             VCHI_HELD_MSG_T *message_handle
 *
 * Description: Routine to return a pointer to the current message (to allow in place processing)
 *              The message is dequeued - don't forget to release the message using
 *              vchi_held_msg_release when you're finished
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t
vchi_msg_hold( VCHI_SERVICE_HANDLE_T handle,
   void **data,
   uint32_t *msg_size,
   VCHI_FLAGS_T flags,
   VCHI_HELD_MSG_T *message_handle )
{
   VCHI_SERVICE_T *service = find_service_by_handle(handle);
   int ret;

   if (!service)
      return VCHIQ_ERROR;

   ret = fill_peek_buf(service, flags);

   if (ret == 0)
   {
      *data = service->peek_buf;
      *msg_size = service->peek_size;

      message_handle->message = service->peek_buf;
      message_handle->service = NULL;

      service->peek_size = -1;
      service->peek_buf = NULL;
   }

   return 0;
}

/***********************************************************
 * Name: vchi_initialise
 *
 * Arguments: VCHI_INSTANCE_T *instance_handle
 *            VCHI_CONNECTION_T **connections
 *            const uint32_t num_connections
 *
 * Description: Initialises the hardware but does not transmit anything
 *              When run as a Host App this will be called twice hence the need
 *              to malloc the state information
 *
 * Returns: 0 if successful, failure otherwise
 *
 ***********************************************************/
int32_t
vchi_initialise( VCHI_INSTANCE_T *instance_handle )
{
   VCHIQ_INSTANCE_T instance;

   instance = vchiq_lib_init();

   vcos_log_trace( "%s: returning instance handle %p", __func__, instance );

   *instance_handle = (VCHI_INSTANCE_T)instance;

   return (instance != NULL) ? 0 : -1;
}

/***********************************************************
 * Name: vchi_connect
 *
 * Arguments: VCHI_CONNECTION_T **connections
 *            const uint32_t num_connections
 *            VCHI_INSTANCE_T instance_handle )
 *
 * Description: Starts the command service on each connection,
 *              causing INIT messages to be pinged back and forth
 *
 * Returns: 0 if successful, failure otherwise
 *
 ***********************************************************/
int32_t
vchi_connect( VCHI_CONNECTION_T **connections,
   const uint32_t num_connections,
   VCHI_INSTANCE_T instance_handle )
{
   VCHIQ_STATUS_T status;

   vcos_unused(connections);
   vcos_unused(num_connections);

   status = vchiq_connect((VCHIQ_INSTANCE_T)instance_handle);

   return (status == VCHIQ_SUCCESS) ? 0 : -1;
}


/***********************************************************
 * Name: vchi_disconnect
 *
 * Arguments: VCHI_INSTANCE_T instance_handle
 *
 * Description: Stops the command service on each connection,
 *              causing DE-INIT messages to be pinged back and forth
 *
 * Returns: 0 if successful, failure otherwise
 *
 ***********************************************************/
int32_t
vchi_disconnect( VCHI_INSTANCE_T instance_handle )
{
   VCHIQ_STATUS_T status;

   status = vchiq_shutdown((VCHIQ_INSTANCE_T)instance_handle);

   return (status == VCHIQ_SUCCESS) ? 0 : -1;
}


/***********************************************************
 * Name: vchi_service_open
 * Name: vchi_service_create
 *
 * Arguments: VCHI_INSTANCE_T *instance_handle
 *            SERVICE_CREATION_T *setup,
 *            VCHI_SERVICE_HANDLE_T *handle
 *
 * Description: Routine to open a service
 *
 * Returns: int32_t - success == 0
 *
 ***********************************************************/
int32_t
vchi_service_open( VCHI_INSTANCE_T instance_handle,
   SERVICE_CREATION_T *setup,
   VCHI_SERVICE_HANDLE_T *handle )
{
   VCHIQ_SERVICE_PARAMS_T params;
   VCHIQ_STATUS_T status;

   memset(&params, 0, sizeof(params));
   params.fourcc = setup->service_id;
   params.userdata = setup->callback_param;
   params.version = (short)setup->version.version;
   params.version_min = (short)setup->version.version_min;

   status = create_service((VCHIQ_INSTANCE_T)instance_handle,
      &params,
      setup->callback,
      1/*open*/,
      (VCHIQ_SERVICE_HANDLE_T *)handle);

   return (status == VCHIQ_SUCCESS) ? 0 : -1;
}

int32_t
vchi_service_create( VCHI_INSTANCE_T instance_handle,
   SERVICE_CREATION_T *setup, VCHI_SERVICE_HANDLE_T *handle )
{
   VCHIQ_SERVICE_PARAMS_T params;
   VCHIQ_STATUS_T status;

   memset(&params, 0, sizeof(params));
   params.fourcc = setup->service_id;
   params.userdata = setup->callback_param;
   params.version = (short)setup->version.version;
   params.version_min = (short)setup->version.version_min;

   status = create_service((VCHIQ_INSTANCE_T)instance_handle,
      &params,
      setup->callback,
      0/*!open*/,
      (VCHIQ_SERVICE_HANDLE_T *)handle);

   return (status == VCHIQ_SUCCESS) ? 0 : -1;
}

int32_t
vchi_service_close( const VCHI_SERVICE_HANDLE_T handle )
{
   VCHI_SERVICE_T *service = find_service_by_handle(handle);
   int ret;

   if (!service)
      return VCHIQ_ERROR;

   RETRY(ret,ioctl(service->fd, VCHIQ_IOC_CLOSE_SERVICE, service->handle));

   if (service->is_client)
      service->lib_handle = VCHIQ_SERVICE_HANDLE_INVALID;

   return ret;
}

int32_t
vchi_service_destroy( const VCHI_SERVICE_HANDLE_T handle )
{
   VCHI_SERVICE_T *service = find_service_by_handle(handle);
   int ret;

   if (!service)
      return VCHIQ_ERROR;

   RETRY(ret,ioctl(service->fd, VCHIQ_IOC_REMOVE_SERVICE, service->handle));

   service->lib_handle = VCHIQ_SERVICE_HANDLE_INVALID;

   return ret;
}

/* ----------------------------------------------------------------------
 * read a uint32_t from buffer.
 * network format is defined to be little endian
 * -------------------------------------------------------------------- */
uint32_t
vchi_readbuf_uint32( const void *_ptr )
{
   const unsigned char *ptr = _ptr;
   return ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
}

/* ----------------------------------------------------------------------
 * write a uint32_t to buffer.
 * network format is defined to be little endian
 * -------------------------------------------------------------------- */
void
vchi_writebuf_uint32( void *_ptr, uint32_t value )
{
   unsigned char *ptr = _ptr;
   ptr[0] = (unsigned char)((value >> 0)  & 0xFF);
   ptr[1] = (unsigned char)((value >> 8)  & 0xFF);
   ptr[2] = (unsigned char)((value >> 16) & 0xFF);
   ptr[3] = (unsigned char)((value >> 24) & 0xFF);
}

/* ----------------------------------------------------------------------
 * read a uint16_t from buffer.
 * network format is defined to be little endian
 * -------------------------------------------------------------------- */
uint16_t
vchi_readbuf_uint16( const void *_ptr )
{
   const unsigned char *ptr = _ptr;
   return ptr[0] | (ptr[1] << 8);
}

/* ----------------------------------------------------------------------
 * write a uint16_t into the buffer.
 * network format is defined to be little endian
 * -------------------------------------------------------------------- */
void
vchi_writebuf_uint16( void *_ptr, uint16_t value )
{
   unsigned char *ptr = _ptr;
   ptr[0] = (value >> 0)  & 0xFF;
   ptr[1] = (value >> 8)  & 0xFF;
}

/***********************************************************
 * Name: vchi_service_use
 *
 * Arguments: const VCHI_SERVICE_HANDLE_T handle
 *
 * Description: Routine to increment refcount on a service
 *
 * Returns: void
 *
 ***********************************************************/
int32_t
vchi_service_use( const VCHI_SERVICE_HANDLE_T handle )
{
   VCHI_SERVICE_T *service = find_service_by_handle(handle);
   int ret;

   if (!service)
      return VCHIQ_ERROR;

   RETRY(ret,ioctl(service->fd, VCHIQ_IOC_USE_SERVICE, service->handle));
   return ret;
}

/***********************************************************
 * Name: vchi_service_release
 *
 * Arguments: const VCHI_SERVICE_HANDLE_T handle
 *
 * Description: Routine to decrement refcount on a service
 *
 * Returns: void
 *
 ***********************************************************/
int32_t vchi_service_release( const VCHI_SERVICE_HANDLE_T handle )
{
   VCHI_SERVICE_T *service = find_service_by_handle(handle);
   int ret;

   if (!service)
      return VCHIQ_ERROR;

   RETRY(ret,ioctl(service->fd, VCHIQ_IOC_RELEASE_SERVICE, service->handle));
   return ret;
}

/***********************************************************
 * Name: vchi_service_set_option
 *
 * Arguments: const VCHI_SERVICE_HANDLE_T handle
 *            VCHI_SERVICE_OPTION_T option
 *            int value
 *
 * Description: Routine to set a service control option
 *
 * Returns: 0 on success, otherwise a non-zero error code
 *
 ***********************************************************/
int32_t vchi_service_set_option( const VCHI_SERVICE_HANDLE_T handle,
   VCHI_SERVICE_OPTION_T option, int value)
{
   VCHIQ_SET_SERVICE_OPTION_T args;
   VCHI_SERVICE_T *service = find_service_by_handle(handle);
   int ret;

   switch (option)
   {
   case VCHI_SERVICE_OPTION_TRACE:
      args.option = VCHIQ_SERVICE_OPTION_TRACE;
      break;
   default:
      service = NULL;
      break;
   }

   if (!service)
      return VCHIQ_ERROR;

   args.handle = service->handle;
   args.value  = value;

   RETRY(ret, ioctl(service->fd, VCHIQ_IOC_SET_SERVICE_OPTION, &args));

   return ret;
}

/***********************************************************
 * Name: vchiq_dump_phys_mem
 *
 * Arguments: const VCHI_SERVICE_HANDLE_T handle
 *            void *buffer
 *            size_t num_bytes
 *
 * Description: Dumps the physical memory associated with
 *              a buffer.
 *
 * Returns: void
 *
 ***********************************************************/
VCHIQ_STATUS_T vchiq_dump_phys_mem( VCHIQ_SERVICE_HANDLE_T handle,
                             void *ptr,
                             size_t num_bytes )
{
   VCHIQ_SERVICE_T *service = (VCHIQ_SERVICE_T *)handle;
   VCHIQ_DUMP_MEM_T  dump_mem;
   int ret;

   if (!service)
      return VCHIQ_ERROR;

   dump_mem.virt_addr = ptr;
   dump_mem.num_bytes = num_bytes;

   RETRY(ret,ioctl(service->fd, VCHIQ_IOC_DUMP_PHYS_MEM, &dump_mem));
   return (ret >= 0) ? VCHIQ_SUCCESS : VCHIQ_ERROR;
}



/*
 * Support functions
 */

static VCHIQ_INSTANCE_T
vchiq_lib_init(void)
{
#pragma warning (push)
#pragma warning (disable : 4459)
   static int mutex_initialised = 0;
   VCHIQ_INSTANCE_T instance = &vchiq_instance;
#pragma warning (pop)

   vcos_global_lock();
   if (!mutex_initialised)
   {
      vcos_mutex_create(&vchiq_lib_mutex, "vchiq-init");

      vcos_log_set_level( &vchiq_lib_log_category, vchiq_default_lib_log_level );
      vcos_log_register( "vchiq_lib", &vchiq_lib_log_category );

      mutex_initialised = 1;
   }
   vcos_global_unlock();

   vcos_mutex_lock(&vchiq_lib_mutex);

   if (instance->initialised == 0)
   {
#ifdef WIN32
       OBJECT_ATTRIBUTES object_attributes;
       IO_STATUS_BLOCK io_status_block;
       UNICODE_STRING device_name;

       RtlInitUnicodeString(
           &device_name, 
           VCHIQ_SYMBOLIC_NAME_W);

       InitializeObjectAttributes(
           &object_attributes,
           &device_name,
           OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
           NULL,
           NULL);

       NTSTATUS status = ZwOpenFile(
           &instance->fd,
           GENERIC_READ | GENERIC_WRITE,
           &object_attributes,
           &io_status_block,
           FILE_SHARE_READ | FILE_SHARE_WRITE,
           0);
        if (!NT_SUCCESS(status)) {
            instance->fd = 0;
        }

      if (instance->fd > 0)
#else
       instance->fd = open("/dev/vchiq", O_RDWR);

      if (instance->fd >= 0)
#endif      
      {
         VCHIQ_GET_CONFIG_T args;
         VCHIQ_CONFIG_T config;
         int ret;
         args.config_size = sizeof(config);
         args.pconfig = &config;
         RETRY(ret, ioctl(instance->fd, VCHIQ_IOC_GET_CONFIG, &args));
         if ((ret == 0) && (config.version >= VCHIQ_VERSION_MIN) && (config.version_min <= VCHIQ_VERSION))
         {
            if (config.version >= VCHIQ_VERSION_LIB_VERSION)
            {
               RETRY(ret, ioctl(instance->fd, VCHIQ_IOC_LIB_VERSION, VCHIQ_VERSION));
            }
            if (ret == 0)
            {
               instance->used_services = 0;
               instance->use_close_delivered = (config.version >= VCHIQ_VERSION_CLOSE_DELIVERED);
               vcos_mutex_create(&instance->mutex, "VCHIQ instance");
               instance->initialised = 1;
            }
         }
         else
         {
            if (ret == 0)
            {
               vcos_log_error("Incompatible VCHIQ library - driver version %d (min %d), library version %d (min %d)",
                  config.version, config.version_min, VCHIQ_VERSION, VCHIQ_VERSION_MIN);
            }
            else
            {
               vcos_log_error("Very incompatible VCHIQ library - cannot retrieve driver version");
            }
            #ifdef WIN32
                ZwClose(instance->fd);
            #else
                close(instance->fd);
            #endif
            instance = NULL;
         }
      }
      else
      {
         instance = NULL;
      }
   }
   else if (instance->initialised > 0)
   {
      instance->initialised++;
   }

   vcos_mutex_unlock(&vchiq_lib_mutex);

   return instance;
}

static void *
completion_thread(void *arg)
{
   VCHIQ_INSTANCE_T instance = (VCHIQ_INSTANCE_T)arg;
   VCHIQ_AWAIT_COMPLETION_T args;
   VCHIQ_COMPLETION_DATA_T completions[8];
   void *msgbufs[8];

   static const VCHI_CALLBACK_REASON_T vchiq_reason_to_vchi[] =
   {
      VCHI_CALLBACK_SERVICE_OPENED,        // VCHIQ_SERVICE_OPENED
      VCHI_CALLBACK_SERVICE_CLOSED,        // VCHIQ_SERVICE_CLOSED
      VCHI_CALLBACK_MSG_AVAILABLE,         // VCHIQ_MESSAGE_AVAILABLE
      VCHI_CALLBACK_BULK_SENT,             // VCHIQ_BULK_TRANSMIT_DONE
      VCHI_CALLBACK_BULK_RECEIVED,         // VCHIQ_BULK_RECEIVE_DONE
      VCHI_CALLBACK_BULK_TRANSMIT_ABORTED, // VCHIQ_BULK_TRANSMIT_ABORTED
      VCHI_CALLBACK_BULK_RECEIVE_ABORTED,  // VCHIQ_BULK_RECEIVE_ABORTED
   };

   args.count = vcos_countof(completions);
   args.buf = completions;
   args.msgbufsize = MSGBUF_SIZE;
   args.msgbufcount = 0;
   args.msgbufs = msgbufs;

#pragma warning (push)
#pragma warning (disable : 4127)
   while (1)
   {
      int ret, i;

      while ((unsigned int)args.msgbufcount < vcos_countof(msgbufs))
      {
         void *msgbuf = alloc_msgbuf();
         if (msgbuf)
         {
            msgbufs[args.msgbufcount++] = msgbuf;
         }
         else
         {
            vcos_log_error("vchiq_lib: failed to allocate a message buffer\n");
            vcos_demand(args.msgbufcount != 0);
         }
      }

      RETRY(ret, ioctl(instance->fd, VCHIQ_IOC_AWAIT_COMPLETION, &args));

      if (ret <= 0)
         break;

      for (i = 0; i < ret; i++)
      {
         VCHIQ_COMPLETION_DATA_T *completion = &completions[i];
         VCHIQ_SERVICE_T *service = (VCHIQ_SERVICE_T *)completion->service_userdata;
         if (service->base.callback)
         {
            vcos_log_trace( "callback(%x, %p, %p(%u,%p), %p)",
               completion->reason, completion->header,
               &service->base, service->lib_handle, service->base.userdata, completion->bulk_userdata );
            service->base.callback(completion->reason, completion->header,
               service->lib_handle, completion->bulk_userdata);
         }
         else if (service->vchi_callback)
         {
            VCHI_CALLBACK_REASON_T vchi_reason =
               vchiq_reason_to_vchi[completion->reason];
            service->vchi_callback(service->base.userdata, vchi_reason, completion->bulk_userdata);
         }

         if ((completion->reason == VCHIQ_SERVICE_CLOSED) &&
             instance->use_close_delivered)
         {
            RETRY(ret,ioctl(service->fd, VCHIQ_IOC_CLOSE_DELIVERED, service->handle));
         }
      }
   }
#pragma warning (pop)

   while (args.msgbufcount)
   {
      void *msgbuf = msgbufs[--args.msgbufcount];
      free_msgbuf(msgbuf);
   }

   return NULL;
}

static VCHIQ_STATUS_T
create_service(VCHIQ_INSTANCE_T instance,
   const VCHIQ_SERVICE_PARAMS_T *params,
   VCHI_CALLBACK_T vchi_callback,
   int is_open,
   VCHIQ_SERVICE_HANDLE_T *phandle)
{
   VCHIQ_SERVICE_T *service = NULL;
   VCHIQ_STATUS_T status = VCHIQ_SUCCESS;
   int i;

   if (!is_valid_instance(instance))
      return VCHIQ_ERROR;

   vcos_mutex_lock(&instance->mutex);

   /* Find a free service */
   if (is_open)
   {
      /* Find a free service */
      for (i = 0; i < instance->used_services; i++)
      {
         if (instance->services[i].lib_handle == VCHIQ_SERVICE_HANDLE_INVALID)
         {
            service = &instance->services[i];
            break;
         }
      }
   }
   else
   {
      for (i = (instance->used_services - 1); i >= 0; i--)
      {
         VCHIQ_SERVICE_T *srv = &instance->services[i];
         if (srv->lib_handle == VCHIQ_SERVICE_HANDLE_INVALID)
         {
            service = srv;
         }
         else if (
            (srv->base.fourcc == params->fourcc) &&
            ((srv->base.callback != params->callback) ||
            (srv->vchi_callback != vchi_callback)))
         {
            /* There is another server using this fourcc which doesn't match */
            vcos_log_info("service %x already using fourcc 0x%x",
               srv->lib_handle, params->fourcc);
            service = NULL;
            status = VCHIQ_ERROR;
            break;
         }
      }
   }

   if (!service && (status == VCHIQ_SUCCESS))
   {
      if (instance->used_services < VCHIQ_MAX_INSTANCE_SERVICES)
         service = &instance->services[instance->used_services++];
      else
         status = VCHIQ_ERROR;
   }

   if (service)
   {
      if (!handle_seq)
         handle_seq = VCHIQ_MAX_INSTANCE_SERVICES;
      service->lib_handle = (ULONG)( handle_seq | (service - instance->services));
      handle_seq += VCHIQ_MAX_INSTANCE_SERVICES;
   }

   vcos_mutex_unlock(&instance->mutex);

   if (service)
   {
      VCHIQ_CREATE_SERVICE_T args;
      int ret;

      service->base.fourcc = params->fourcc;
      service->base.callback = params->callback;
      service->vchi_callback = vchi_callback;
      service->base.userdata = params->userdata;
      service->fd = instance->fd;
      service->peek_size = -1;
      service->peek_buf = NULL;
      service->is_client = is_open;

      args.params = *params;
      args.params.userdata = service;
      args.is_open = is_open;
      args.is_vchi = (params->callback == NULL);
      args.handle = VCHIQ_SERVICE_HANDLE_INVALID; /* OUT parameter */
      RETRY(ret, ioctl(instance->fd, VCHIQ_IOC_CREATE_SERVICE, &args));
      if (ret == 0)
         service->handle = args.handle;
      else
         status = VCHIQ_ERROR;
   }

   if (status == VCHIQ_SUCCESS)
   {
      *phandle = service->lib_handle;
      vcos_log_info("service handle %x lib_handle %x using fourcc 0x%x",
         service->handle, service->lib_handle, params->fourcc);
   }
   else
   {
      vcos_mutex_lock(&instance->mutex);

      if (service)
         service->lib_handle = VCHIQ_SERVICE_HANDLE_INVALID;

      vcos_mutex_unlock(&instance->mutex);

      *phandle = VCHIQ_SERVICE_HANDLE_INVALID;
   }

   return status;
}

static int
fill_peek_buf(VCHI_SERVICE_T *service,
   VCHI_FLAGS_T flags)
{
   VCHIQ_DEQUEUE_MESSAGE_T args;
   int ret = 0;

   vcos_assert(flags == VCHI_FLAGS_NONE || flags == VCHI_FLAGS_BLOCK_UNTIL_OP_COMPLETE);

   if (service->peek_size < 0)
   {
      if (!service->peek_buf)
         service->peek_buf = alloc_msgbuf();

      if (service->peek_buf)
      {
         args.handle = service->handle;
         args.blocking = (flags == VCHI_FLAGS_BLOCK_UNTIL_OP_COMPLETE);
         args.bufsize = MSGBUF_SIZE;
         args.buf = service->peek_buf;

         RETRY(ret, ioctl(service->fd, VCHIQ_IOC_DEQUEUE_MESSAGE, &args));

         if (ret >= 0)
         {
            service->peek_size = ret;
            ret = 0;
         }
         else
         {
            ret = -1;
         }
      }
      else
      {
         ret = -1;
      }
   }

   return ret;
}


static void *
alloc_msgbuf(void)
{
   void *msgbuf;
   vcos_mutex_lock(&vchiq_lib_mutex);
   msgbuf = free_msgbufs;
   if (msgbuf)
      free_msgbufs = *(void **)msgbuf;
   vcos_mutex_unlock(&vchiq_lib_mutex);
   if (!msgbuf)
      msgbuf = vcos_malloc(MSGBUF_SIZE, "alloc_msgbuf");
   return msgbuf;
}

static void
free_msgbuf(void *buf)
{
   vcos_mutex_lock(&vchiq_lib_mutex);
   *(void **)buf = free_msgbufs;
   free_msgbufs = buf;
   vcos_mutex_unlock(&vchiq_lib_mutex);
}

__pragma(code_seg(pop))