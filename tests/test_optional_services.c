#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
void runtime_restart(void) { abort(); }
void runtime_guest_frames(uintptr_t a,uintptr_t b) { (void)a; (void)b; abort(); }
int main(void) {
    runtime_start(1);
    void *(ABI *self)(void)=(void *(ABI *)(void))runtime_thread_resolve("aI+OeCz8xrQ#p#J");
    int32_t (ABI *get)(void*,int*,int*)=(int32_t (ABI *)(void*,int*,int*))runtime_thread_resolve("P41kTWUS3EI#p#J");
    int32_t (ABI *set)(void*,int,const int*)=(int32_t (ABI *)(void*,int,const int*))runtime_thread_resolve("oIRFTjoILbg#p#J");
    assert(self && get && set);
    void *thread=self(); int policy=-1,priority=-1;
    assert(!get(thread,&policy,&priority));
    int saved_policy=policy,saved_priority=priority,new_priority=450;
    assert(!set(thread,2,&new_priority)); assert(!get(thread,&policy,&priority));
    assert(policy==2 && priority==450);
    assert((uint32_t)get(thread,NULL,&priority)==0x80020016);
    assert((uint32_t)set(thread,0,&new_priority)==0x80020016);
    assert(!set(thread,saved_policy,&saved_priority));
    int32_t (ABI *state)(int,int,uint32_t*,uint32_t*)=(int32_t (ABI *)(int,int,uint32_t*,uint32_t*))runtime_services_resolve("LHuSmO3SLd8#l#m");
    int32_t (ABI *unlock)(int,int,int,int*)=(int32_t (ABI *)(int,int,int,int*))runtime_services_resolve("28xmRUFao68#l#m");
    assert(state && unlock);
    uint32_t flags[4]={UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX},count=99;
    assert(!state(1,1,flags,&count)); assert(!count && !(flags[0]|flags[1]|flags[2]|flags[3]));
    int platinum=99; assert(!unlock(1,1,42,&platinum)); assert(!unlock(1,1,42,&platinum));
    assert(!state(1,1,flags,&count)); assert(count==1 && flags[1]==(1u<<10) && platinum==-1);
    assert(state(1,1,NULL,&count)<0);
    puts("optional services: scheduler round-trip and trophy bit/count contracts passed");
}
