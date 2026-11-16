/* Differential check against the uncached state builder, using the actual renderer. */
#include "../../recomp/port/render.c"
#include <stdio.h>
uint32_t port_frames;
void port_log(const char* format, ...){(void)format;}
double port_now_ms(void){return 0;}
void port_report_api_time(uint32_t frames){(void)frames;}
static uint32_t seed=1234567;
static uint32_t next(void){seed^=seed<<13;seed^=seed>>17;return seed^=seed<<5;}
static void uncached(int32_t* st){
 memset(st,0,41*4);
 for(int s=0;s<2;s++){
  for(int k=0;k<6;k++)st[6*s+k]=(int32_t)R.tss[s][1+k];
  st[12+s]=(int32_t)gl_texture(R.tex_obj[s]);
  st[29+5*s]=(int32_t)R.samp[s][5];st[30+5*s]=(int32_t)R.samp[s][6];st[31+5*s]=(int32_t)R.samp[s][7];st[32+5*s]=(int32_t)R.samp[s][1];st[33+5*s]=(int32_t)R.samp[s][2];
 }
 st[14]=R.rs[60];st[15]=R.rs[15];st[16]=R.rs[25];st[17]=R.rs[24];st[18]=R.rs[27];st[19]=R.rs[19];st[20]=R.rs[20];st[21]=R.rs[171]?R.rs[171]:1;
 st[22]=has_depth?R.rs[7]:0;st[23]=R.rs[14];st[24]=R.rs[23];draw_area(st+25,st+26,st+27,st+28);st[38]=R.rs[34];st[39]=R.rs[28]!=0;st[40]=R.rs[22];
}
static uint32_t surface(void){uint32_t s=(uint32_t)(uintptr_t)calloc(1,128);M32(s+12)=8;M32(s+16)=8;M32(s+20)=21;M32(s+28)=(uint32_t)(uintptr_t)calloc(1,256);M32(s+56)=2;return s;}
static int same(int iteration){int32_t a[41],b[41];gl_state(a);uncached(b);if(memcmp(a,b,sizeof a)){printf("FAIL state iteration %d\n",iteration);return 0;}return 1;}
int main(void){
 uint32_t a=surface(),b=surface(),rect=(uint32_t)(uintptr_t)calloc(1,16),bits=M32(a+28);reset_state(a);
 uint8_t saved[sizeof R],mask[sizeof R];memset(mask,255,sizeof mask);
 for(int i=0;i<100000;i++){
  uint32_t value=next();switch(i%10){
   case 0:r_set_render_state(next()%256,value);break;
   case 1:r_set_tss(next()%8,next()%33,value);break;
   case 2:r_set_sampler(next()%8,next()%14,value);break;
   case 3:r_set_depth(value&1);break;
   case 4:for(int k=0;k<4;k++)M32(rect+4*k)=next()%2048;r_set_scissor(rect);break;
   case 5:for(int k=0;k<4;k++)M32(rect+4*k)=next()%2048;r_set_viewport(rect);break;
   case 6:r_set_render_target(value&1?a:b);break;
   case 7:r_set_texture(value&1,value&2?a:b);break;
   case 8:r_state_save(saved);r_set_render_state(60,value);r_state_load(saved,mask);break;
   case 9:if((i%100)==99)r_reset(a);break;
  }
  if(!same(i)||!same(i))return 1;
 }
 r_set_texture(0,a);if(!same(100001))return 1;M32(a+28)=0;if(!same(100002))return 1;M32(a+28)=bits;
 uint32_t before=glq_len;M32(a+56)=0;M32(a+88)=1;gl_state((int32_t[41]){0});if(glq_len<=before||M32(a+56)!=2){puts("FAIL: cached state skipped a dirty texture upload");return 1;}
 if(!same(100003))return 1;
 gl_state((int32_t[41]){0});r_set_render_state(7,R.rs[7]);r_set_tss(0,1,R.tss[0][1]);r_set_sampler(0,5,R.samp[0][5]);if(gl_state_dirty){puts("FAIL: unchanged setters invalidate the state cache");return 1;}
 puts("PASS: 100000 state transitions match uncached words, including reset, state blocks, depth, viewport/scissor, rebinding, invalid/dirty textures and unchanged setters.");return 0;
}
