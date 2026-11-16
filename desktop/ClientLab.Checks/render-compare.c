/* Compile with emcc -O2 -Irecomp/port this-file -o render-compare.js, then run with Node. */
#include "render_compare.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static uint32_t random_state = 0x12345678;
static uint32_t next(void) { random_state ^= random_state << 13; random_state ^= random_state >> 17; return random_state ^= random_state << 5; }
static int old_batch(const int32_t* a, const int32_t* b) { return a[13] == b[13] && !memcmp(a+15,b+15,48) && !memcmp(a+43,b+43,1176); }
static int old_material(const int32_t* a, const int32_t* b) { return !memcmp(a+21,b+21,20) && !memcmp(a+81,b+81,60) && !memcmp(a+99,b+99,640); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000.0+t.tv_nsec/1e6; }
int main(void) {
    int32_t a[342], b[342];
    for (int i=0;i<100000;i++) {
        for (int k=0;k<342;k++) a[k]=(int32_t)next();
        a[25]=(int32_t)(next()%9); memset(a+99+20*a[25],0,4*(160-20*a[25])); memcpy(b,a,sizeof a);
        if (i&1) { int k=(int)(next()%338); if(k!=25)b[k]^=1u<<(next()%32); }
        memset(b+99+20*b[25],0,4*(160-20*b[25]));
        if (r_tnl_batch_equal(a,b)!=old_batch(a,b) || r_tnl_material_equal(a,b)!=old_material(a,b)) { puts("FAIL: draw state changed");return 1; }
        uint32_t n=next()%339,offset=next()%4;
        if(r_words_equal(a+offset,b+offset,n)!=(memcmp(a+offset,b+offset,n*4)==0)) { puts("FAIL: word comparison changed bit equality");return 1; }
        int32_t saved[338],copy[338];for(int k=0;k<338;k++)saved[k]=copy[k]=(int32_t)next();r_tnl_copy(copy,a);
        for(int k=0;k<338;k++){
            int active=k<99||k>=259||k<99+20*a[25];
            if(copy[k]!=(active?a[k]:saved[k])){puts("FAIL: effective state copy changed active bits or touched inactive storage");return 1;}
        }
        if(!r_tnl_batch_equal(copy,a)||!r_tnl_material_equal(copy,a)){puts("FAIL: copied state does not match its source");return 1;}
    }
    memset(a,0,sizeof a);memset(b,0,sizeof b);
    // Inactive slots must not prevent batching; the renderer never reads them.
    b[150]=0x7fc01234;
    if(!r_tnl_batch_equal(a,b)||!r_tnl_material_equal(a,b)) { puts("FAIL: inactive light was treated as active");return 1; }
    b[25]=1;if(r_tnl_batch_equal(a,b)) { puts("FAIL: changed light count was ignored");return 1; }
    a[25]=9;r_tnl_copy(b,a);if(memcmp(a,b,338*4)){puts("FAIL: invalid light count did not preserve full-copy fallback");return 1;}a[25]=0;
    memset(b,0,sizeof b);
    volatile int result=0;double start=now();
    for(int i=0;i<200000;i++){a[0]=i;result+=old_batch(a,b);}
    double before=now()-start;start=now();
    for(int i=0;i<200000;i++){a[0]=i;result+=r_tnl_batch_equal(a,b);}
    double after=now()-start;
    printf("PASS: 100000 randomized draw states preserve batching/material and exact word bits; inactive lights ignored.\n");
    printf("200000 equivalent no-light comparisons: old %.1f ms, new %.1f ms, result %d\n",before,after,result);
    start=now();for(int i=0;i<200000;i++){a[13]=i;memcpy(b,a,338*4);__asm__ __volatile__("" : : "r"(a),"r"(b) : "memory");}before=now()-start;
    start=now();for(int i=0;i<200000;i++){a[13]=i;r_tnl_copy(b,a);__asm__ __volatile__("" : : "r"(a),"r"(b) : "memory");}after=now()-start;
    printf("200000 no-light state snapshots: full %.1f ms, active %.1f ms\n",before,after);
    return 0;
}
