/*
Copyright (c) 2012, Broadcom Europe Ltd
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

/*=============================================================================
VideoCore OS Abstraction Layer - pthreads types
=============================================================================*/

/* Do not include this file directly - instead include it via vcos.h */

/** @file
  *
  * Pthreads implementation of VCOS.
  *
  */

#ifndef VCOS_PLATFORM_H
#define VCOS_PLATFORM_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef WIN32_KERN
#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <wdm.h>
#include <Ntstrsafe.h>
#else
#include <Windows.h>
#endif
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>

// Disable prefast specific warning to maintain consistent vchiq, vchi and vcos
// interface. This also minimize porting overhead between OS.
#pragma prefast(disable:28718, "Disable Unannotated Buffer. Maintain interface defintion")
#pragma prefast(disable:25004, "Nonconst Param. Maintain interface defintion")
#pragma prefast(disable:25033, "Nonconst Buffer Param. Maintain interface defintion")
#pragma prefast(disable:28023, "Missing _Function_class_ annotation. Not all parameter use the same function entry definition")
#pragma prefast(disable:25135, "Missing locking annotation. Maintain interface defintion")
#pragma prefast(disable:25120, "Count required for void ptr buffer. Maintain interface defintion")
#pragma prefast(disable:25057, "Count required for writable buffer. Maintain interface defintion")
// Disabling dangerous cast as VCHI and VCHIQ shares structure. It is advice
// to enable this prefast warning during development
#pragma prefast(disable:25024, "Dangerous Cast")

#define VCOS_HAVE_RTOS         1
#define VCOS_HAVE_SEMAPHORE    1
#define VCOS_HAVE_EVENT        1
#define VCOS_HAVE_QUEUE        0
#define VCOS_HAVE_LEGACY_ISR   0
#define VCOS_HAVE_TIMER        1
#define VCOS_HAVE_CANCELLATION_SAFE_TIMER 1
#define VCOS_HAVE_MEMPOOL      0
#define VCOS_HAVE_ISR          0
#define VCOS_HAVE_ATOMIC_FLAGS 1
#define VCOS_HAVE_THREAD_AT_EXIT        1
#define VCOS_HAVE_ONCE         1
#define VCOS_HAVE_BLOCK_POOL   1
#define VCOS_HAVE_FILE         0
#define VCOS_HAVE_PROC         0
#define VCOS_HAVE_CFG          0
#define VCOS_HAVE_ALIEN_THREADS  1
#define VCOS_HAVE_CMD          1
#define VCOS_HAVE_EVENT_FLAGS  1
#define VCOS_WANT_LOG_CMD      0    /* User apps should do their own thing */

#define VCOS_ALWAYS_WANT_LOGGING

#ifdef WIN32_KERN
// Disable some warning so minimal code change is required between
// Windows and Linux implementation
#pragma warning(disable : 4127) // conditional expression is constant
#else

#endif

#if defined(WIN32DLL_EXPORTS) && defined(VCOS_PROVIDER)
#define WIN32DLL_VCOS_API __declspec(dllexport)
#else
#define WIN32DLL_VCOS_API __declspec(dllimport)
#endif

#define VCOS_ONCE_INIT  INIT_ONCE_STATIC_INIT

#ifdef WIN32_KERN

RTL_RUN_ONCE_INIT_FN  InitHandleFunction;

struct timespec
{
    time_t tv_sec;  // Seconds - >= 0
    long   tv_nsec; // Nanoseconds - [0, 999999999]
};

#else

BOOL CALLBACK InitHandleFunction (PINIT_ONCE InitOnce,
    PVOID Parameter,
    PVOID *lpContext);

#endif

#define VCOS_SO_EXT  ".dll"

/* Linux/pthreads seems to have different timer characteristics */
#define VCOS_TIMER_MARGIN_EARLY 0
#define VCOS_TIMER_MARGIN_LATE 15

#ifdef WIN32_KERN
    typedef KSEMAPHORE            VCOS_SEMAPHORE_T;
    typedef uint32_t              VCOS_UNSIGNED;
    typedef uint32_t              VCOS_OPTION;
    typedef uint32_t              VCOS_TLS_KEY_T;
    typedef RTL_RUN_ONCE          VCOS_ONCE_T;
#else
    typedef HANDLE                VCOS_SEMAPHORE_T;
    typedef uint32_t              VCOS_UNSIGNED;
    typedef uint32_t              VCOS_OPTION;
    // TODO implement pthread_key_t & pthread_once_t
    typedef uint32_t               VCOS_TLS_KEY_T;
    typedef INIT_ONCE              VCOS_ONCE_T;
#endif


typedef struct VCOS_LLTHREAD_T
{
    HANDLE thread;
} VCOS_LLTHREAD_T;

/* VCOS_CASSERT(offsetof(VCOS_LLTHREAD_T, thread) == 0); */

#ifdef WIN32_KERN
    typedef KMUTEX VCOS_MUTEX_T;
#else
    typedef HANDLE VCOS_MUTEX_T;
#endif

typedef struct
{
   VCOS_MUTEX_T   mutex;
#ifdef WIN32_KERN
   KEVENT         sem;
#else
   HANDLE         sem;
#endif
} VCOS_EVENT_T;

typedef struct VCOS_TIMER_T
{
    HANDLE thread;                         /**< id of the timer thread */

    HANDLE lock;                           /**< lock protecting all other members of the struct */
    HANDLE settings_changed;               /**< cond. var. for informing the timer thread about changes*/

   int quit;                              /**< non-zero if the timer thread is requested to quit*/

   struct timespec expires;               /**< absolute time of next expiration, or 0 if disarmed*/

   void (*orig_expiration_routine)(void*);/**< the expiration routine provided by the user of the timer*/
   void *orig_context;                    /**< the context for exp. routine provided by the user*/

} VCOS_TIMER_T;

/** Thread attribute structure. Don't use pthread_attr directly, as
  * the calls can fail, and inits must match deletes.
  */
typedef struct VCOS_THREAD_ATTR_T
{
   void *ta_stackaddr;
   VCOS_UNSIGNED ta_stacksz;
   VCOS_UNSIGNED ta_priority;
   VCOS_UNSIGNED ta_affinity;
   VCOS_UNSIGNED ta_timeslice;
   VCOS_UNSIGNED legacy;
} VCOS_THREAD_ATTR_T;

/** Called at thread exit.
  */
typedef struct VCOS_THREAD_EXIT_T
{
   void (*pfn)(void *);
   void *cxt;
} VCOS_THREAD_EXIT_T;
#define VCOS_MAX_EXIT_HANDLERS  4

typedef struct VCOS_THREAD_T
{
   HANDLE thread;                /**< The thread itself */
   VCOS_THREAD_ENTRY_FN_T entry; /**< The thread entry point */
   void *arg;                    /**< The argument to be passed to entry */
   VCOS_SEMAPHORE_T suspend;     /**< For support event groups and similar - a per thread semaphore */

   VCOS_TIMER_T task_timer;
   int task_timer_created;       /**< non-zero if the task timer has already been created*/
   void (*orig_task_timer_expiration_routine)(void*);
   void *orig_task_timer_context;

   VCOS_UNSIGNED legacy;
   char name[16];                /**< Record the name of this thread, for diagnostics */
   VCOS_UNSIGNED dummy;          /**< Dummy thread created for non-vcos created threads */

   /** Callback invoked at thread exit time */
   VCOS_THREAD_EXIT_T at_exit[VCOS_MAX_EXIT_HANDLERS];
} VCOS_THREAD_T;


#define VCOS_SUSPEND          -1
#define VCOS_NO_SUSPEND       0

#define VCOS_START 1
#define VCOS_NO_START 0

#define VCOS_THREAD_PRI_MIN   THREAD_PRIORITY_LOWEST
#define VCOS_THREAD_PRI_MAX   THREAD_PRIORITY_HIGHEST

#define VCOS_THREAD_PRI_INCREASE (1)
#define VCOS_THREAD_PRI_HIGHEST  VCOS_THREAD_PRI_MAX
#define VCOS_THREAD_PRI_LOWEST   VCOS_THREAD_PRI_MIN
#define VCOS_THREAD_PRI_NORMAL ((VCOS_THREAD_PRI_MAX+VCOS_THREAD_PRI_MIN)/2)
#define VCOS_THREAD_PRI_BELOW_NORMAL (VCOS_THREAD_PRI_NORMAL-VCOS_THREAD_PRI_INCREASE)
#define VCOS_THREAD_PRI_ABOVE_NORMAL (VCOS_THREAD_PRI_NORMAL+VCOS_THREAD_PRI_INCREASE)
#define VCOS_THREAD_PRI_REALTIME VCOS_THREAD_PRI_MAX

#define _VCOS_AFFINITY_DEFAULT 0
#define _VCOS_AFFINITY_CPU0    0x100
#define _VCOS_AFFINITY_CPU1    0x200
#define _VCOS_AFFINITY_MASK    0x300
#define VCOS_CAN_SET_STACK_ADDR  0

#define VCOS_TICKS_PER_SECOND _vcos_get_ticks_per_second()

#include "interface/vcos/generic/vcos_generic_event_flags.h"
#include "interface/vcos/generic/vcos_generic_blockpool.h"
#include "interface/vcos/generic/vcos_mem_from_malloc.h"

/** Convert errno values into the values recognized by vcos */
VCOSPRE_ VCOS_STATUS_T vcos_pthreads_map_error(int error);
VCOSPRE_ VCOS_STATUS_T VCOSPOST_ vcos_pthreads_map_errno(void);

/** Register a function to be called when the current thread exits.
  */
WIN32DLL_VCOS_API VCOS_STATUS_T vcos_thread_at_exit(void (*pfn)(void*), void *cxt);

WIN32DLL_VCOS_API uint32_t _vcos_get_ticks_per_second(void);

typedef struct {
   VCOS_MUTEX_T mutex;
   uint32_t flags;
} VCOS_ATOMIC_FLAGS_T;

WIN32DLL_VCOS_API int vcos_use_android_log;

#if defined(VCOS_INLINE_BODIES)

#undef VCOS_ASSERT_LOGGING_DISABLE
#define VCOS_ASSERT_LOGGING_DISABLE 1


/*
 * Counted Semaphores
 */
VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_semaphore_wait(VCOS_SEMAPHORE_T *sem) {
#ifdef WIN32_KERN
    (void)KeWaitForSingleObject(sem, Executive, KernelMode, FALSE, NULL);
#else
    (void)WaitForSingleObject(*sem, INFINITE);
    return VCOS_SUCCESS;
#endif
    return VCOS_SUCCESS;
}

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_semaphore_trywait(VCOS_SEMAPHORE_T *sem) {
#ifdef WIN32_KERN
    LARGE_INTEGER timeoutL;
    timeoutL.QuadPart = WDF_REL_TIMEOUT_IN_MS(1000);
    DWORD result = KeWaitForSingleObject(sem, Executive, KernelMode, FALSE, &timeoutL);
    switch (result)
    {
    case STATUS_SUCCESS:
        return VCOS_SUCCESS;
    case STATUS_TIMEOUT:
        return VCOS_EAGAIN;
    default:
        return VCOS_EINVAL;
    }
#else
    DWORD result = WaitForSingleObject(*sem, 0);
    switch (result)
    {
    case WAIT_OBJECT_0:
        return VCOS_SUCCESS;
    case WAIT_TIMEOUT:
        return VCOS_EAGAIN;
    default:
        return VCOS_EINVAL;
    }
#endif
}

/**
  * \brief Wait on a semaphore with a timeout.
  *
  * Note that this function may not be implemented on all
  * platforms, and may not be efficient on all platforms
  * (see comment in vcos_semaphore_wait)
  *
  * Try to obtain the semaphore. If it is already taken, return
  * VCOS_EAGAIN.
  * @param sem Semaphore to wait on
  * @param timeout Number of milliseconds to wait before
  *                returning if the semaphore can't be acquired.
  * @return VCOS_SUCCESS - semaphore was taken.
  *         VCOS_EAGAIN - could not take semaphore (i.e. timeout
  *         expired)
  *         VCOS_EINVAL - Some other error (most likely bad
  *         parameters).
  */
VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_semaphore_wait_timeout(VCOS_SEMAPHORE_T *sem, VCOS_UNSIGNED timeout) {
#ifdef WIN32_KERN
    LARGE_INTEGER timeoutL;
    timeoutL.QuadPart = WDF_REL_TIMEOUT_IN_MS(timeout);
    DWORD result = KeWaitForSingleObject(sem, Executive, KernelMode, FALSE, &timeoutL);
    switch (result)
    {
    case STATUS_SUCCESS:
        return VCOS_SUCCESS;
    case STATUS_TIMEOUT:
        return VCOS_EAGAIN;
    default:
        return VCOS_EINVAL;
    }
#else
    DWORD result = WaitForSingleObject(*sem, timeout);

    switch (result)
    {
    case WAIT_OBJECT_0:
        return VCOS_SUCCESS;
    case WAIT_TIMEOUT:
        return VCOS_EAGAIN;
    default:
        return VCOS_EINVAL;
    }
#endif
}

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_semaphore_create(VCOS_SEMAPHORE_T *sem,
                                    const char *name,
                                    VCOS_UNSIGNED initial_count) 
{

#ifdef WIN32_KERN
    UNREFERENCED_PARAMETER(name);
    KeInitializeSemaphore(sem, initial_count, INT_MAX);
#else
    UNREFERENCED_PARAMETER(name);
    *sem = CreateSemaphore(NULL, initial_count, INT_MAX, NULL);
    if (*sem == NULL) {
        return vcos_pthreads_map_errno();
    }
#endif
    
    return VCOS_SUCCESS;
}

VCOS_INLINE_IMPL
void vcos_semaphore_delete(VCOS_SEMAPHORE_T *sem) 
{
#ifdef WIN32_KERN
    // nothing to do here
    UNREFERENCED_PARAMETER(sem);
#else
    CloseHandle(*sem);
#endif
}

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_semaphore_post(VCOS_SEMAPHORE_T *sem) {
#ifdef WIN32_KERN
    KeReleaseSemaphore(sem, 0, 1, FALSE);
#else
    ReleaseSemaphore(*sem, 1, NULL);
#endif
    return VCOS_SUCCESS;
}

/***********************************************************
 *
 * Threads
 *
 ***********************************************************/


WIN32DLL_VCOS_API VCOS_THREAD_T *vcos_dummy_thread_create(void);

WIN32DLL_VCOS_API DWORD _vcos_thread_current_key;

WIN32DLL_VCOS_API uint64_t vcos_getmicrosecs64_internal(void);

VCOS_INLINE_IMPL
uint32_t vcos_getmicrosecs(void) { return (uint32_t)vcos_getmicrosecs64_internal(); }

VCOS_INLINE_IMPL
uint64_t vcos_getmicrosecs64(void) { return vcos_getmicrosecs64_internal(); }

VCOS_INLINE_IMPL
VCOS_THREAD_T *vcos_thread_current(void) 
{
    void *ret = NULL;
    // TODO implement pthread_getspecific

#ifdef __cplusplus
   return static_cast<VCOS_THREAD_T*>(ret);
#else
   return (VCOS_THREAD_T *)ret;
#endif
}

VCOS_INLINE_IMPL
void vcos_sleep(uint32_t ms) {
#ifdef WIN32_KERN
    LARGE_INTEGER sleepTime;

    sleepTime.QuadPart = WDF_REL_TIMEOUT_IN_MS(ms);
    KeDelayExecutionThread(KernelMode, FALSE, &sleepTime);
#else
    Sleep(ms);
#endif
}

VCOS_INLINE_IMPL
void vcos_thread_attr_setstack(VCOS_THREAD_ATTR_T *attr, void *addr, VCOS_UNSIGNED sz) {
   attr->ta_stackaddr = addr;
   attr->ta_stacksz = sz;
}

VCOS_INLINE_IMPL
void vcos_thread_attr_setstacksize(VCOS_THREAD_ATTR_T *attr, VCOS_UNSIGNED sz) {
   attr->ta_stacksz = sz;
}

VCOS_INLINE_IMPL
void vcos_thread_attr_setpriority(VCOS_THREAD_ATTR_T *attr, VCOS_UNSIGNED pri) {
   (void)attr;
   (void)pri;
}

VCOS_INLINE_IMPL
void vcos_thread_set_priority(VCOS_THREAD_T *thread, VCOS_UNSIGNED p) {
   /* not implemented */
   (void)thread;
   (void)p;
}

VCOS_INLINE_IMPL
VCOS_UNSIGNED vcos_thread_get_priority(VCOS_THREAD_T *thread) {
   /* not implemented */
   (void)thread;
   return 0;
}

VCOS_INLINE_IMPL
void vcos_thread_set_affinity(VCOS_THREAD_T *thread, VCOS_UNSIGNED affinity) {
   /* not implemented */
   vcos_unused(thread);
   vcos_unused(affinity);
}


VCOS_INLINE_IMPL
void vcos_thread_attr_setaffinity(VCOS_THREAD_ATTR_T *attrs, VCOS_UNSIGNED affinity) {
   attrs->ta_affinity = affinity;
}

VCOS_INLINE_IMPL
void vcos_thread_attr_settimeslice(VCOS_THREAD_ATTR_T *attrs, VCOS_UNSIGNED ts) {
   attrs->ta_timeslice = ts;
}

VCOS_INLINE_IMPL
void _vcos_thread_attr_setlegacyapi(VCOS_THREAD_ATTR_T *attrs, VCOS_UNSIGNED legacy) {
   attrs->legacy = legacy;
}

VCOS_INLINE_IMPL
void vcos_thread_attr_setautostart(VCOS_THREAD_ATTR_T *attrs, VCOS_UNSIGNED autostart) {
   (void)attrs;
   (void)autostart;
}

VCOS_INLINE_IMPL
VCOS_LLTHREAD_T *vcos_llthread_current(void) 
{
#ifdef WIN32_KERN
    return PsGetCurrentThreadId();
#else
    return GetCurrentThread();
#endif
}

/*
 * Mutexes
 */

#ifndef VCOS_USE_VCOS_FUTEX

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_mutex_create(VCOS_MUTEX_T *latch, const char *name) 
{
#ifdef WIN32_KERN 
    UNREFERENCED_PARAMETER(name);
    KeInitializeMutex(latch, 0);

    return VCOS_SUCCESS;
#else
    UNREFERENCED_PARAMETER(name);
    *latch = CreateMutex(NULL, FALSE, NULL);
    if (*latch != NULL) {
        return VCOS_SUCCESS;
    }
    return vcos_pthreads_map_errno();
#endif
}

VCOS_INLINE_IMPL
void vcos_mutex_delete(VCOS_MUTEX_T *latch) 
{
#ifdef WIN32_KERN 
    // Do nothing
    UNREFERENCED_PARAMETER(latch);
#else
    CloseHandle(*latch);
#endif
}

#pragma prefast(Suppress:26135, "Suppress locking annotation for consistent vcos interface")
VCOS_INLINE_IMPL VCOS_STATUS_T vcos_mutex_lock(VCOS_MUTEX_T *latch) 
{
#ifdef WIN32_KERN 
    KeWaitForMutexObject(latch, Executive, KernelMode, FALSE, NULL);
#else
    (void)WaitForSingleObject(*latch, INFINITE);
#endif
    return VCOS_SUCCESS;
}

#pragma prefast(Suppress:26135, "Suppress locking annotation for consistent vcos interface")
VCOS_INLINE_IMPL void vcos_mutex_unlock(VCOS_MUTEX_T *latch) 
{
#ifdef WIN32_KERN 
    KeReleaseMutex(latch, FALSE);
#else
    ReleaseMutex(*latch);
#endif
}

VCOS_INLINE_IMPL
int vcos_mutex_is_locked(VCOS_MUTEX_T *m) 
{
#ifdef WIN32_KERN 
    return KeReadStateMutex(m);
#else
    DWORD result = WaitForSingleObject(*m, 0);
    if (result == WAIT_OBJECT_0) {
        ReleaseMutex(*m);
        return 0;
    }
    return 1;
#endif
}

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_mutex_trylock(VCOS_MUTEX_T *m) 
{
#ifdef WIN32_KERN 
    LARGE_INTEGER timeoutL;
    timeoutL.QuadPart = WDF_REL_TIMEOUT_IN_MS(0);
    DWORD result = KeWaitForMutexObject(m, Executive, KernelMode, FALSE, &timeoutL);
    switch (result)
    {
    case STATUS_SUCCESS:
        return VCOS_SUCCESS;
    case STATUS_TIMEOUT:
        return VCOS_EAGAIN;
    default:
        return VCOS_EINVAL;
    }
#else
    DWORD result = WaitForSingleObject(*m, 0);
    if (result == WAIT_OBJECT_0) {
        return VCOS_SUCCESS;
    }
    return VCOS_EAGAIN;
#endif
}

#endif /* VCOS_USE_VCOS_FUTEX */

/*
 * Events
 */

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_event_create(VCOS_EVENT_T *event, const char *debug_name)
{
#ifdef WIN32_KERN 
    VCOS_STATUS_T status;

    KeInitializeEvent(&event->sem, SynchronizationEvent, FALSE);

    status = vcos_mutex_create(&event->mutex, debug_name);
    if (status != VCOS_SUCCESS) {
        return status;
    }
    return VCOS_SUCCESS;
#else
    VCOS_STATUS_T status;

    event->sem = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (event->sem == NULL) return vcos_pthreads_map_errno();

    status = vcos_mutex_create(&event->mutex, debug_name);
    if (status != VCOS_SUCCESS) {
        vcos_semaphore_delete(&event->sem);
        return status;
    }
    return VCOS_SUCCESS;
#endif
}

VCOS_INLINE_IMPL
void vcos_event_signal(VCOS_EVENT_T *event)
{

   int ok = 0;

   if (vcos_mutex_lock(&event->mutex) != VCOS_SUCCESS)
      goto fail_mtx;

#ifdef WIN32_KERN
   int value;
   value = KeSetEvent(&event->sem, 0, FALSE);

   if (value != 0) {
       goto fail_sem;
   }
#else
   if (!SetEvent(event->sem)) {
       goto fail_sem;
   }
#endif
   
   ok = 1;
fail_sem:
   vcos_mutex_unlock(&event->mutex);
fail_mtx:
   vcos_assert(ok);
}

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_event_wait(VCOS_EVENT_T *event)
{
#ifdef WIN32_KERN 
    (void)KeWaitForSingleObject(&event->sem, Executive, KernelMode, FALSE, NULL);
    return VCOS_SUCCESS;
#else
    DWORD result = WaitForSingleObject(event->sem, INFINITE);
    if (result == WAIT_OBJECT_0) {
        return VCOS_SUCCESS;
    }
    return VCOS_EAGAIN;
#endif
}

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_event_try(VCOS_EVENT_T *event)
{
#ifdef WIN32_KERN 
    LARGE_INTEGER timeoutL;
    timeoutL.QuadPart = WDF_REL_TIMEOUT_IN_MS(0);
    DWORD result = KeWaitForSingleObject(&event->sem, Executive, KernelMode, FALSE, &timeoutL);
    switch (result)
    {
    case STATUS_SUCCESS:
        return VCOS_SUCCESS;
    case STATUS_TIMEOUT:
        return VCOS_EAGAIN;
    default:
        return VCOS_EINVAL;
    }
#else
    DWORD result = WaitForSingleObject(event->sem, 0);
    if (result == WAIT_OBJECT_0) {
        return VCOS_SUCCESS;
    }
    return VCOS_EAGAIN;
#endif
}

VCOS_INLINE_IMPL
void vcos_event_delete(VCOS_EVENT_T *event)
{
#ifdef WIN32_KERN 
    // nothing to do here
#else
    CloseHandle(event->sem);
#endif

   vcos_mutex_delete(&event->mutex);
}

VCOS_INLINE_IMPL
VCOS_UNSIGNED vcos_process_id_current(void) 
{
#ifdef WIN32_KERN
    vcos_assert(0);
    return 0;
#else
    return GetCurrentProcessId();
#endif
}

VCOS_INLINE_IMPL
int vcos_strcasecmp(const char *s1, const char *s2) 
{
    return _stricmp(s1, s2);
#ifdef ORI
    return strcasecmp(s1,s2);
#endif
}

VCOS_INLINE_IMPL
int vcos_strncasecmp(const char *s1, const char *s2, size_t n) 
{
    return strncmp(s1, s2, n);
}

VCOS_INLINE_IMPL
int vcos_in_interrupt(void) 
{
   return 0;
}

/* For support event groups - per thread semaphore */
VCOS_INLINE_IMPL
void _vcos_thread_sem_wait(void) 
{
   VCOS_THREAD_T *t = vcos_thread_current();
   vcos_semaphore_wait(&t->suspend);
}

VCOS_INLINE_IMPL
void _vcos_thread_sem_post(VCOS_THREAD_T *target) 
{
   vcos_semaphore_post(&target->suspend);
}

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_tls_create(VCOS_TLS_KEY_T *key) 
{
#ifdef WIN32_KERN
    UNREFERENCED_PARAMETER(key);
    vcos_assert(0);
#else
    *key = TlsAlloc();
    return VCOS_SUCCESS;
#endif
}

VCOS_INLINE_IMPL
void vcos_tls_delete(VCOS_TLS_KEY_T tls) 
{
#ifdef WIN32_KERN 
    UNREFERENCED_PARAMETER(tls);
    vcos_assert(0);
#else
    TlsFree(tls);
#endif
}

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_tls_set(VCOS_TLS_KEY_T tls, void *v) 
{
#ifdef WIN32_KERN 
    UNREFERENCED_PARAMETER(tls);
    UNREFERENCED_PARAMETER(v);
    vcos_assert(0);
#else
    TlsSetValue(tls, v);
#endif
    return VCOS_SUCCESS;
}

VCOS_INLINE_IMPL
void *vcos_tls_get(VCOS_TLS_KEY_T tls) 
{
#ifdef WIN32_KERN 
    UNREFERENCED_PARAMETER(tls);
    vcos_assert(0);
    return NULL;
#else
    return TlsGetValue(tls);
#endif
}

#if VCOS_HAVE_ATOMIC_FLAGS

/*
 * Atomic flags
 */

/* TODO implement properly... */

VCOS_INLINE_IMPL
VCOS_STATUS_T vcos_atomic_flags_create(VCOS_ATOMIC_FLAGS_T *atomic_flags)
{
   atomic_flags->flags = 0;
   return vcos_mutex_create(&atomic_flags->mutex, "VCOS_ATOMIC_FLAGS_T");
}

VCOS_INLINE_IMPL
void vcos_atomic_flags_or(VCOS_ATOMIC_FLAGS_T *atomic_flags, uint32_t flags)
{
   vcos_mutex_lock(&atomic_flags->mutex);
   atomic_flags->flags |= flags;
   vcos_mutex_unlock(&atomic_flags->mutex);
}

VCOS_INLINE_IMPL
uint32_t vcos_atomic_flags_get_and_clear(VCOS_ATOMIC_FLAGS_T *atomic_flags)
{
   uint32_t flags;
   vcos_mutex_lock(&atomic_flags->mutex);
   flags = atomic_flags->flags;
   atomic_flags->flags = 0;
   vcos_mutex_unlock(&atomic_flags->mutex);
   return flags;
}

VCOS_INLINE_IMPL
void vcos_atomic_flags_delete(VCOS_ATOMIC_FLAGS_T *atomic_flags)
{
   vcos_mutex_delete(&atomic_flags->mutex);
}

#endif

#if defined(linux) || defined(_HAVE_SBRK)

/* not exactly the free memory, but a measure of it */

VCOS_INLINE_IMPL
unsigned long vcos_get_free_mem(void) {
#ifdef ORI
   return (unsigned long)sbrk(0);
#else
    //TODO Implement
#endif
}

#endif

#undef VCOS_ASSERT_LOGGING_DISABLE
#define VCOS_ASSERT_LOGGING_DISABLE 0

#endif /* VCOS_INLINE_BODIES */

#define  vcos_log_platform_init()               _vcos_log_platform_init()
VCOSPRE_ void VCOSPOST_             _vcos_log_platform_init(void);

VCOS_INLINE_DECL void _vcos_thread_sem_wait(void);
VCOS_INLINE_DECL void _vcos_thread_sem_post(VCOS_THREAD_T *);

#define VCOS_APPLICATION_ARGC          vcos_get_argc()
#define VCOS_APPLICATION_ARGV          vcos_get_argv()

#include "interface/vcos/generic/vcos_generic_reentrant_mtx.h"
#include "interface/vcos/generic/vcos_generic_named_sem.h"
#include "interface/vcos/generic/vcos_generic_quickslow_mutex.h"
#include "interface/vcos/generic/vcos_common.h"

#ifdef WIN32_KERN
// Disable logging in kernel mode
#define _VCOS_LOG_LEVEL() 0
#else
#define _VCOS_LOG_LEVEL() getenv("VC_LOGLEVEL")
#endif

VCOS_STATIC_INLINE
char *vcos_strdup(const char *str)
{
   return _strdup(str);
}

typedef void (*VCOS_ISR_HANDLER_T)(VCOS_UNSIGNED vecnum);

// Undefine for Windows
#define VCOS_DL_LAZY    0
#define VCOS_DL_NOW     1
#define VCOS_DL_LOCAL   2
#define VCOS_DL_GLOBAL  3

#ifdef WIN32_KERN
__inline int getpagesize()
{
    
    vcos_assert(0);

    return 4096;
}

#else

int getpagesize();

__inline int getpagesize()
{
    SYSTEM_INFO siSysInfo;

    GetSystemInfo(&siSysInfo);

    return siSysInfo.dwPageSize;
}

#endif

#ifdef __cplusplus
}
#endif
#endif /* VCOS_PLATFORM_H */

