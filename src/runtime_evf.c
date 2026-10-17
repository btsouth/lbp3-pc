/* Kernel event flags (sceKernel*EventFlag) and POSIX semaphores (sem_*), after shadPS4's
 * kernel/threads/event_flag.cpp and semaphore.cpp. Waiters are served FIFO. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <time.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
enum { EVF_SINGLE=0x10, EVF_MULTI=0x20, WAIT_AND=1, WAIT_OR=2, CLEAR_ALL=0x10, CLEAR_PAT=0x20 };
typedef struct EvfWaiter {
    uint64_t bits, result;
    uint32_t mode;
    int ready, canceled, deleted;
    pthread_cond_t cv;
    struct EvfWaiter *next;
} EvfWaiter;
typedef struct {
    pthread_mutex_t lock;
    uint64_t bits;
    uint32_t attr;
    int deleted, waiting;
    EvfWaiter *first;
    char name[32];
} EventFlag;
static int met(uint64_t bits,const EvfWaiter *w) {
    return (w->mode&0xf)==WAIT_AND ? (bits&w->bits)==w->bits : (bits&w->bits)!=0;
}
static void clear(EventFlag *e,const EvfWaiter *w) {
    if (w->mode&CLEAR_ALL) e->bits=0;
    else if (w->mode&CLEAR_PAT) e->bits&=~w->bits;
}
static ABI int32_t evf_create(EventFlag **out,const char *name,uint32_t attr,uint64_t pattern,const void *options) {
    if (!out || !name || options || attr>(EVF_MULTI|2) || (attr&0xf)==3 || (attr&0xf0)==0x30) return ERR(22);
    if (strlen(name)>=32) return ERR(63);
    EventFlag *e=calloc(1,sizeof(*e));
    if (!e) return ERR(12);
    pthread_mutex_init(&e->lock,NULL);
    e->bits=pattern; e->attr=attr;
    snprintf(e->name,sizeof(e->name),"%s",name);
    *out=e;
    printf("Runtime: event flag '%s' attr=%#x pattern=%#llx\n",name,attr,(unsigned long long)pattern);
    return 0;
}
static ABI int32_t evf_delete(EventFlag *e) {
    if (!e) return ERR(3);
    pthread_mutex_lock(&e->lock);
    e->deleted=1;
    for (EvfWaiter *w=e->first;w;w=w->next) { w->deleted=1; pthread_cond_signal(&w->cv); }
    int busy=e->waiting;
    pthread_mutex_unlock(&e->lock);
    if (!busy) free(e); /* else the last waiter frees it */
    return 0;
}
static int32_t evf_wait_common(EventFlag *e,uint64_t bits,uint32_t mode,uint64_t *result,uint32_t *timeout,int poll) {
    if (!e) return ERR(3);
    if (!bits || ((mode&0xf)!=WAIT_AND && (mode&0xf)!=WAIT_OR) || (mode&0xf0)==0x30) return ERR(22);
    pthread_mutex_lock(&e->lock);
    if (e->deleted) { pthread_mutex_unlock(&e->lock); return ERR(13); }
    if ((e->attr&0xf0)!=EVF_MULTI && e->first) { pthread_mutex_unlock(&e->lock); return ERR(1); }
    EvfWaiter w={.bits=bits,.mode=mode};
    if (met(e->bits,&w)) {
        if (result) *result=e->bits;
        clear(e,&w);
        pthread_mutex_unlock(&e->lock);
        return 0;
    }
    if (poll || (timeout && !*timeout)) {
        if (result) *result=e->bits;
        pthread_mutex_unlock(&e->lock);
        return poll ? ERR(16) : ERR(60);
    }
    pthread_condattr_t ca;
    pthread_condattr_init(&ca); pthread_condattr_setclock(&ca,CLOCK_MONOTONIC);
    pthread_cond_init(&w.cv,&ca); pthread_condattr_destroy(&ca);
    EvfWaiter **tail=&e->first;
    while (*tail) tail=&(*tail)->next;
    *tail=&w; ++e->waiting;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC,&end);
    uint64_t deadline=timeout ? (uint64_t)end.tv_sec*1000000000+(uint64_t)end.tv_nsec+(uint64_t)*timeout*1000 : 0;
    end.tv_sec=(time_t)(deadline/1000000000); end.tv_nsec=(long)(deadline%1000000000);
    int timed_out=0;
    const uint64_t start=runtime_wait_clock();
    while (!w.ready && !w.canceled && !w.deleted) {
        int r=timeout ? pthread_cond_timedwait(&w.cv,&e->lock,&end) : pthread_cond_wait(&w.cv,&e->lock);
        if (r==ETIMEDOUT && !w.ready && !w.canceled && !w.deleted) { timed_out=1; break; }
    }
    runtime_wait_note(2,runtime_wait_clock()-start);
    EvfWaiter **p=&e->first;
    while (*p && *p!=&w) p=&(*p)->next;
    if (*p) *p=w.next;
    int32_t rc=0;
    if (timed_out) { rc=ERR(60); if (result) *result=e->bits; }
    else {
        if (result) *result=w.result;
        rc=w.canceled ? ERR(85) : w.deleted ? ERR(13) : 0;
    }
    if (timeout) {
        uint64_t now=runtime_wait_clock();
        *timeout=timed_out || now>=deadline ? 0 : (uint32_t)((deadline-now)/1000);
    }
    pthread_cond_destroy(&w.cv);
    int free_it=!--e->waiting && e->deleted;
    pthread_mutex_unlock(&e->lock);
    if (free_it) free(e);
    return rc;
}
static ABI int32_t evf_wait(EventFlag *e,uint64_t bits,uint32_t mode,uint64_t *result,uint32_t *timeout) {
    return evf_wait_common(e,bits,mode,result,timeout,0);
}
static ABI int32_t evf_poll(EventFlag *e,uint64_t bits,uint32_t mode,uint64_t *result) {
    return evf_wait_common(e,bits,mode,result,NULL,1);
}
static ABI int32_t evf_set(EventFlag *e,uint64_t bits) {
    if (!e) return ERR(3);
    pthread_mutex_lock(&e->lock);
    e->bits|=bits;
    for (EvfWaiter *w=e->first;w;w=w->next) {
        if (w->ready || w->canceled || !met(e->bits,w)) continue;
        w->result=e->bits; clear(e,w); w->ready=1;
        pthread_cond_signal(&w->cv);
    }
    pthread_mutex_unlock(&e->lock);
    return 0;
}
static ABI int32_t evf_clear(EventFlag *e,uint64_t bits) {
    if (!e) return ERR(3);
    pthread_mutex_lock(&e->lock); e->bits&=bits; pthread_mutex_unlock(&e->lock);
    return 0;
}
static ABI int32_t evf_cancel(EventFlag *e,uint64_t pattern,int32_t *waiters) {
    if (!e) return ERR(3);
    pthread_mutex_lock(&e->lock);
    int n=0;
    e->bits=pattern;
    for (EvfWaiter *w=e->first;w;w=w->next) {
        if (w->ready || w->canceled) continue;
        w->result=pattern; w->canceled=1; ++n;
        pthread_cond_signal(&w->cv);
    }
    if (waiters) *waiters=n;
    pthread_mutex_unlock(&e->lock);
    return 0;
}

/* POSIX semaphores: the guest's sem_t holds a pointer to a host semaphore. Errors return -1
 * with the guest errno (FreeBSD numbers). */
static int32_t posix_error(int e) {
    *runtime_errno()=runtime_guest_errno(e);
    return -1;
}
static ABI int32_t psem_init(sem_t **sem,int32_t shared,uint32_t value) {
    (void)shared;
    if (!sem || value>0x7fffffffu) return posix_error(EINVAL);
    sem_t *s=malloc(sizeof(*s));
    if (!s) return posix_error(ENOMEM);
    sem_init(s,0,value);
    *sem=s;
    return 0;
}
static ABI int32_t psem_destroy(sem_t **sem) {
    if (!sem || !*sem) return posix_error(EINVAL);
    sem_destroy(*sem); free(*sem); *sem=NULL;
    return 0;
}
static ABI int32_t psem_wait(sem_t **sem) {
    if (!sem || !*sem) return posix_error(EINVAL);
    const uint64_t start=runtime_wait_clock();
    while (sem_wait(*sem)) if (errno!=EINTR) return posix_error(errno);
    runtime_wait_note(2,runtime_wait_clock()-start);
    return 0;
}
static ABI int32_t psem_trywait(sem_t **sem) {
    if (!sem || !*sem) return posix_error(EINVAL);
    return sem_trywait(*sem) ? posix_error(errno) : 0;
}
static ABI int32_t psem_timedwait(sem_t **sem,const struct timespec *abstime) {
    if (!sem || !*sem || !abstime) return posix_error(EINVAL);
    const uint64_t start=runtime_wait_clock();
    while (sem_timedwait(*sem,abstime)) if (errno!=EINTR) return posix_error(errno);
    runtime_wait_note(2,runtime_wait_clock()-start);
    return 0;
}
static ABI int32_t psem_post(sem_t **sem) {
    if (!sem || !*sem) return posix_error(EINVAL);
    return sem_post(*sem) ? posix_error(errno) : 0;
}
static ABI int32_t psem_getvalue(sem_t **sem,int32_t *value) {
    if (!sem || !*sem || !value) return posix_error(EINVAL);
    int v=0; sem_getvalue(*sem,&v); *value=v;
    return 0;
}

static const RuntimeExport exports[]={
    {"sceKernelCreateEventFlag",evf_create}, {"sceKernelDeleteEventFlag",evf_delete},
    {"sceKernelWaitEventFlag",evf_wait}, {"sceKernelPollEventFlag",evf_poll},
    {"sceKernelSetEventFlag",evf_set}, {"sceKernelClearEventFlag",evf_clear},
    {"sceKernelCancelEventFlag",evf_cancel},
    {"sem_init",psem_init}, {"sem_destroy",psem_destroy}, {"sem_wait",psem_wait},
    {"sem_trywait",psem_trywait}, {"sem_timedwait",psem_timedwait}, {"sem_post",psem_post},
    {"sem_getvalue",psem_getvalue},
};
uintptr_t runtime_evf_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_evf_report(void) {}
#else
uintptr_t runtime_evf_resolve(const char *name) { (void)name; return 0; }
void runtime_evf_report(void) {}
#endif
