/* Kernel event flags: a 64-bit pattern that threads wait on (AND/OR of bits, optional clear).
 * The handle is a host pointer. One condition variable per flag; waiters recheck on wakeup,
 * Set serves them in arrival order (priority-ordered flags are served FIFO, as semaphores). */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <errno.h>
#include <time.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
enum { WAIT_AND=1, WAIT_OR=2, CLEAR_ALL=0x10, CLEAR_PAT=0x20, ATTR_MULTI=0x20 };
#define EVF_MAGIC UINT32_C(0x45564631)
typedef struct EvfWaiter {
    uint64_t bits, result;
    uint32_t mode;
    int ready, canceled, deleted;
    struct EvfWaiter *next;
} EvfWaiter;
typedef struct {
    uint32_t magic, attr;
    uint64_t pattern;
    int deleted;
    unsigned active;
    EvfWaiter *first;
    pthread_mutex_t lock;
    pthread_cond_t event;
    char name[32];
} EventFlag;
static size_t created, waits, sets;

static int met(const EventFlag *f,uint64_t bits,uint32_t mode) {
    return (mode&WAIT_AND) ? (f->pattern&bits)==bits : (f->pattern&bits)!=0;
}
static void clear(EventFlag *f,uint64_t bits,uint32_t mode) {
    if (mode&CLEAR_ALL) f->pattern=0;
    else if (mode&CLEAR_PAT) f->pattern&=~bits;
}
static int valid_mode(uint32_t mode) {
    uint32_t wait=mode&0xf, clr=mode&0xf0;
    return (wait==WAIT_AND || wait==WAIT_OR) && (clr==0 || clr==CLEAR_ALL || clr==CLEAR_PAT) && !(mode&~0xffu);
}
static EventFlag *flag(void *handle) {
    EventFlag *f=handle;
    return f && f->magic==EVF_MAGIC ? f : NULL;
}
static ABI int32_t evf_create(void **out,const char *name,uint32_t attr,uint64_t pattern,const void *options) {
    if (!out || !name || options || (attr&~0x33u) || (attr&0xf)==3 || (attr&0x30)==0x30) return ERR(22);
    if (strlen(name)>=32) return ERR(63);
    EventFlag *f=calloc(1,sizeof(*f));
    if (!f) return ERR(12);
    f->magic=EVF_MAGIC; f->attr=attr; f->pattern=pattern;
    snprintf(f->name,sizeof(f->name),"%s",name);
    pthread_mutex_init(&f->lock,NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca,CLOCK_MONOTONIC);
    pthread_cond_init(&f->event,&ca);
    pthread_condattr_destroy(&ca);
    __atomic_add_fetch(&created,1,__ATOMIC_RELAXED);
    *out=f;
    return 0;
}
static ABI int32_t evf_delete(void *handle) {
    EventFlag *f=flag(handle);
    if (!f) return ERR(3);
    pthread_mutex_lock(&f->lock);
    f->deleted=1;
    for (EvfWaiter *w=f->first;w;w=w->next) w->deleted=1;
    pthread_cond_broadcast(&f->event);
    while (f->active) pthread_cond_wait(&f->event,&f->lock);
    f->magic=0;
    pthread_mutex_unlock(&f->lock);
    /* Not freed: a late call with the stale handle then fails the magic check instead of
     * reading freed memory. */
    return 0;
}
/* timeout: NULL waits forever; *timeout is microseconds, set to the time left. */
static int32_t wait_flag(void *handle,uint64_t bits,uint32_t mode,uint64_t *result,uint32_t *timeout,int block) {
    EventFlag *f=flag(handle);
    if (!f) return ERR(3);
    if (!bits || !valid_mode(mode)) return ERR(22);
    pthread_mutex_lock(&f->lock);
    int32_t r=0;
    if (f->deleted) r=ERR(13);
    else if (met(f,bits,mode)) {
        if (result) *result=f->pattern;
        clear(f,bits,mode);
    } else if (!(f->attr&ATTR_MULTI) && f->active) r=ERR(1);
    else if (!block) { r=ERR(16); if (result) *result=f->pattern; }
    else {
        EvfWaiter w={.bits=bits,.mode=mode};
        EvfWaiter **tail=&f->first;
        while (*tail) tail=&(*tail)->next;
        *tail=&w; ++f->active;
        __atomic_add_fetch(&waits,1,__ATOMIC_RELAXED);
        struct timespec start,deadline;
        clock_gettime(CLOCK_MONOTONIC,&start);
        deadline=start;
        if (timeout) {
            uint64_t ns=(uint64_t)start.tv_nsec+(uint64_t)*timeout*1000;
            deadline.tv_sec+=ns/1000000000; deadline.tv_nsec=ns%1000000000;
        }
        int timed_out=0;
        while (!w.ready && !w.canceled && !w.deleted && !timed_out) {
            if (timeout) timed_out=pthread_cond_timedwait(&f->event,&f->lock,&deadline)==ETIMEDOUT;
            else pthread_cond_wait(&f->event,&f->lock);
        }
        for (EvfWaiter **p=&f->first;*p;p=&(*p)->next) if (*p==&w) { *p=w.next; break; }
        --f->active;
        if (f->deleted) pthread_cond_broadcast(&f->event);
        if (w.ready || w.canceled) { if (result) *result=w.result; }
        else if (result) *result=f->pattern;
        if (w.canceled) r=ERR(85);
        else if (w.deleted) r=ERR(13);
        else if (!w.ready) r=ERR(60);
        if (timeout) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC,&now);
            int64_t spent=(int64_t)(now.tv_sec-start.tv_sec)*1000000+(now.tv_nsec-start.tv_nsec)/1000;
            *timeout=w.ready || spent<(int64_t)*timeout ? (uint32_t)((int64_t)*timeout-spent>0 ? (int64_t)*timeout-spent : 0) : 0;
        }
    }
    pthread_mutex_unlock(&f->lock);
    return r;
}
static ABI int32_t evf_wait(void *f,uint64_t bits,uint32_t mode,uint64_t *result,uint32_t *timeout) {
    return wait_flag(f,bits,mode,result,timeout,1);
}
static ABI int32_t evf_poll(void *f,uint64_t bits,uint32_t mode,uint64_t *result) {
    return wait_flag(f,bits,mode,result,NULL,0);
}
static ABI int32_t evf_set(void *handle,uint64_t bits) {
    EventFlag *f=flag(handle);
    if (!f) return ERR(3);
    pthread_mutex_lock(&f->lock);
    f->pattern|=bits;
    int woke=0;
    for (EvfWaiter *w=f->first;w;w=w->next) {
        if (w->ready || !met(f,w->bits,w->mode)) continue;
        w->result=f->pattern;
        clear(f,w->bits,w->mode);
        w->ready=woke=1;
    }
    if (woke) pthread_cond_broadcast(&f->event);
    pthread_mutex_unlock(&f->lock);
    __atomic_add_fetch(&sets,1,__ATOMIC_RELAXED);
    return 0;
}
/* sceKernelClearEventFlag keeps the bits that are set in the argument. */
static ABI int32_t evf_clear(void *handle,uint64_t bits) {
    EventFlag *f=flag(handle);
    if (!f) return ERR(3);
    pthread_mutex_lock(&f->lock);
    f->pattern&=bits;
    pthread_mutex_unlock(&f->lock);
    return 0;
}
static ABI int32_t evf_cancel(void *handle,uint64_t pattern,int32_t *waiting) {
    EventFlag *f=flag(handle);
    if (!f) return ERR(3);
    pthread_mutex_lock(&f->lock);
    if (waiting) *waiting=(int32_t)f->active;
    f->pattern=pattern;
    for (EvfWaiter *w=f->first;w;w=w->next) { w->result=pattern; w->canceled=1; }
    pthread_cond_broadcast(&f->event);
    pthread_mutex_unlock(&f->lock);
    return 0;
}
static const RuntimeExport exports[]={
    {"sceKernelCreateEventFlag",evf_create}, {"sceKernelDeleteEventFlag",evf_delete},
    {"sceKernelWaitEventFlag",evf_wait}, {"sceKernelPollEventFlag",evf_poll},
    {"sceKernelSetEventFlag",evf_set}, {"sceKernelClearEventFlag",evf_clear},
    {"sceKernelCancelEventFlag",evf_cancel},
};
uintptr_t runtime_evf_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_evf_report(void) {
    printf("Runtime: event flags created=%zu, waits=%zu, sets=%zu\n",created,waits,sets);
}
#else
uintptr_t runtime_evf_resolve(const char *name) { (void)name; return 0; }
void runtime_evf_report(void) {}
#endif
