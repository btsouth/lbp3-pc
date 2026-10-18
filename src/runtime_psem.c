/* POSIX unnamed semaphores (sem_init, sem_wait, ...): the guest's sem_t (16 bytes on the PS4's
 * FreeBSD) holds a pointer to a host counter. POSIX results: 0, or -1 with the guest errno. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <errno.h>
#include <time.h>
#define PSEM_MAGIC UINT32_C(0x50534d31)
typedef struct { uint32_t magic; uint32_t count; pthread_mutex_t lock; pthread_cond_t event; } PosixSem;
typedef struct { int64_t seconds, nanoseconds; } GuestTimespec;
enum { G_EINVAL=22, G_EAGAIN=35, G_ETIMEDOUT=60, G_EOVERFLOW=84 };
static size_t created, waits, posts;

static int fail(int e) { *runtime_errno()=e; return -1; }
static PosixSem *sem_of(void **sem) {
    PosixSem *s=sem ? *sem : NULL;
    return s && s->magic==PSEM_MAGIC ? s : NULL;
}
static ABI int psem_init(void **sem, int shared, uint32_t value) {
    (void)shared;
    if (!sem || value>INT32_MAX) return fail(G_EINVAL);
    PosixSem *s=calloc(1,sizeof(*s));
    if (!s) return fail(G_EAGAIN);
    s->magic=PSEM_MAGIC; s->count=value;
    pthread_mutex_init(&s->lock,NULL);
    pthread_cond_init(&s->event,NULL); /* CLOCK_REALTIME: sem_timedwait takes wall-clock time */
    *sem=s;
    __atomic_add_fetch(&created,1,__ATOMIC_RELAXED);
    return 0;
}
static ABI int psem_destroy(void **sem) {
    PosixSem *s=sem_of(sem);
    if (!s) return fail(G_EINVAL);
    s->magic=0; /* not freed: a late wait on a destroyed semaphore fails instead of crashing */
    *sem=NULL;
    return 0;
}
static int wait_sem(void **sem, const GuestTimespec *deadline, int block) {
    PosixSem *s=sem_of(sem);
    if (!s) return fail(G_EINVAL);
    if (deadline && (deadline->nanoseconds<0 || deadline->nanoseconds>=1000000000)) return fail(G_EINVAL);
    pthread_mutex_lock(&s->lock);
    int r=0;
    __atomic_add_fetch(&waits,1,__ATOMIC_RELAXED);
    if (!s->count && !block) r=G_EAGAIN;
    else {
        struct timespec until={0,0};
        if (deadline) { until.tv_sec=(time_t)deadline->seconds; until.tv_nsec=(long)deadline->nanoseconds; }
        while (!s->count) {
            int e=deadline ? pthread_cond_timedwait(&s->event,&s->lock,&until) : pthread_cond_wait(&s->event,&s->lock);
            if (e==ETIMEDOUT && !s->count) { r=G_ETIMEDOUT; break; }
        }
    }
    if (!r) --s->count;
    pthread_mutex_unlock(&s->lock);
    return r ? fail(r) : 0;
}
static ABI int psem_wait(void **sem) { return wait_sem(sem,NULL,1); }
static ABI int psem_trywait(void **sem) { return wait_sem(sem,NULL,0); }
static ABI int psem_timedwait(void **sem, const GuestTimespec *deadline) {
    return deadline ? wait_sem(sem,deadline,1) : fail(G_EINVAL);
}
static ABI int psem_post(void **sem) {
    PosixSem *s=sem_of(sem);
    if (!s) return fail(G_EINVAL);
    pthread_mutex_lock(&s->lock);
    int r=0;
    if (s->count>=INT32_MAX) r=G_EOVERFLOW;
    else { ++s->count; pthread_cond_signal(&s->event); }
    pthread_mutex_unlock(&s->lock);
    __atomic_add_fetch(&posts,1,__ATOMIC_RELAXED);
    return r ? fail(r) : 0;
}
static ABI int psem_getvalue(void **sem, int *value) {
    PosixSem *s=sem_of(sem);
    if (!s || !value) return fail(G_EINVAL);
    pthread_mutex_lock(&s->lock);
    *value=(int)s->count;
    pthread_mutex_unlock(&s->lock);
    return 0;
}
static ABI int yield_thread(void) { sched_yield(); return 0; }
static const RuntimeExport exports[]={
    {"sem_init",psem_init}, {"sem_destroy",psem_destroy}, {"sem_wait",psem_wait},
    {"sem_trywait",psem_trywait}, {"sem_timedwait",psem_timedwait}, {"sem_post",psem_post},
    {"sem_getvalue",psem_getvalue}, {"pthread_yield",yield_thread}, {"sched_yield",yield_thread},
};
uintptr_t runtime_psem_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_psem_report(void) {
    printf("Runtime: POSIX semaphores created=%zu, waits=%zu, posts=%zu\n",created,waits,posts);
}
#else
uintptr_t runtime_psem_resolve(const char *name) { (void)name; return 0; }
void runtime_psem_report(void) {}
#endif
