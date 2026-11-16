#include "../../recomp/port/hle_d3d9.c"
#include <time.h>

static unsigned allocations;
uint32_t guest_alloc(uint32_t size,uint32_t align) { (void)align; allocations++;return (uint32_t)(uintptr_t)calloc(1,size); }
void guest_free(uint32_t pointer) { free(GPTR(pointer)); }
static uint32_t seed=12345;
static uint32_t next(void){seed^=seed<<13;seed^=seed>>17;return seed^=seed<<5;}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000.0+t.tv_nsec/1e6;}
int main(void){
    CPU c={0};uint32_t frame=guest_alloc(16,16),object=guest_alloc(64,16),matrix=guest_alloc(64,16);c.esp=frame;M32(frame+4)=object;M32(object+40)=matrix;
    float base[16],factor[16],before[16];
    for(int iteration=0;iteration<100000;iteration++){
        for(int k=0;k<16;k++){base[k]=(int32_t)(next()%2001-1000)/37.0f;factor[k]=(int32_t)(next()%2001-1000)/31.0f;}
        if(iteration%3==0)for(int k=0;k<16;k++){uint32_t a=next()&0xff7fffff,b=next()&0xff7fffff;memcpy(base+k,&a,4);memcpy(factor+k,&b,4);}
        if(iteration%100==0){uint32_t edge[]={0x80000000,0x7f800000,0xff800000,0x7fc01234,1};memcpy(base,edge,sizeof edge);}
        memcpy(GPTR(matrix),base,64);c.eax=123;ms_apply_legacy(&c,factor,iteration&1);memcpy(before,GPTR(matrix),64);CPU expected=c;
        memcpy(GPTR(matrix),base,64);c.eax=123;ms_apply(&c,factor,iteration&1);
        if(memcmp(before,GPTR(matrix),64)||memcmp(&c,&expected,sizeof c)){printf("FAIL iteration %d: CPU diff %d\n",iteration,memcmp(&c,&expected,sizeof c));for(int k=0;k<16;k++)if(memcmp(before+k,GPTR(matrix+4*k),4))printf("  %d: %08x vs %08x\n",k,((uint32_t*)before)[k],M32(matrix+4*k));return 1;}
    }
    for(int iteration=0;iteration<200000;iteration++){
        uint32_t axisBits[3];for(int k=0;k<3;k++)axisBits[k]=next();
        if(iteration%4==0){axisBits[0]=0;axisBits[1]=0x80000000;axisBits[2]=0x3f800000;}
        for(int k=0;k<16;k++){uint32_t bits=next();memcpy(base+k,&bits,4);}
        if(iteration%8==0){uint32_t edge[]={0x80000000,0,1,0x7f7fffff};for(int k=0;k<16;k++)memcpy(base+k,edge+(k%4),4);}
        float x=fa(axisBits[0]),y=fa(axisBits[1]),z=fa(axisBits[2]);int scale=iteration&1,local=(iteration>>1)&1;
        if(scale)scaling(factor,x,y,z);else translation(factor,x,y,z);
        memcpy(GPTR(matrix),base,64);c.eax=123;ms_apply_legacy(&c,factor,local);memcpy(before,GPTR(matrix),64);CPU expected=c;
        memcpy(GPTR(matrix),base,64);c.eax=123;ms_affine(&c,x,y,z,local,scale);
        if(memcmp(before,GPTR(matrix),64)||memcmp(&c,&expected,sizeof c)){printf("FAIL affine iteration %d scale %d local %d\n",iteration,scale,local);for(int k=0;k<16;k++)if(memcmp(before+k,GPTR(matrix+4*k),4))printf("  %d: %08x vs %08x\n",k,((uint32_t*)before)[k],M32(matrix+4*k));return 1;}
    }
    mat_identity(matrix);memcpy(base,GPTR(matrix),64);scaling(factor,1.5f,2.0f,0.5f);
    allocations=0;double start=now();for(int i=0;i<200000;i++){memcpy(GPTR(matrix),base,64);ms_apply_legacy(&c,factor,i&1);}double old=now()-start;unsigned oldAlloc=allocations;
    allocations=0;start=now();for(int i=0;i<200000;i++){memcpy(GPTR(matrix),base,64);ms_apply(&c,factor,i&1);}double fast=now()-start;
    printf("PASS: 100000 matrix/guest-state differential cases, including NaNs, infinities and signed zero.\n");
    printf("200000 matrix operations: old %.1f ms (%u allocations), stack %.1f ms (%u allocations)\n",old,oldAlloc,fast,allocations);
    start=now();for(int i=0;i<200000;i++){memcpy(GPTR(matrix),base,64);ms_affine(&c,1.5f,2.0f,0.5f,i&1,1);}double affine=now()-start;
    printf("PASS: 200000 specialized affine differential cases, including random float bits, exceptional inputs, overflow, subnormals and signed zero.\n");
    printf("200000 specialized scale operations: %.1f ms\n",affine);
    return 0;
}
