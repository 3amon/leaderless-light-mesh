/* Bake solo normal-mode frames through main.c's pixel pipeline. */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include "social_scene.h"
#include "led_budget.h"
#ifndef TAG
#define TAG "x"
#endif
#define LEDS 16
#define FPS 20
#define SECS 16
#define BRIGHT 0.45f
static const uint32_t seeds[3] = {0x1a2b3c4dU, 0x51a7c0deU, 0x89abcdefU};
int main(void){
  uint32_t budget = led_budget_total(LEDS, LED_BUDGET_PER_LED_DEFAULT);
  printf("{");
  for(int fam=0;fam<3;fam++){
    struct social_scene s; social_scene_init(&s, seeds[fam], SOCIAL_SOLO, 1);
    /* find an epoch that runs this family, start after the crossfade */
    int64_t epoch=1; struct social_scene_plan plan;
    for(;;epoch++){ social_scene_plan_at(&s,(float)epoch*60.f+30.f,&plan); if(plan.family==fam) break; }
    for(int pal=0;pal<3;pal++){
    if(pal){ epoch+=3; social_scene_plan_at(&s,(float)epoch*60.f+30.f,&plan); }
    float t0=(float)epoch*60.f+6.f;
    uint32_t applied=256; double load_sum=0; uint32_t load_max=0; int capped=0;
    printf("%s\"%s%dp%d\":{\"name\":\"%s\",\"period\":%.1f,\"hue\":%.0f,\"frames\":[", (fam||pal)?",":"", TAG, fam, pal,
           social_scene_family_name(SOCIAL_SOLO,fam), (double)plan.period_s, (double)fmodf(plan.hue[0]+3600.f,360.f));
    for(int f=0;f<FPS*SECS;f++){
      float t=t0+(float)f/FPS; uint8_t px[LEDS][3]; uint32_t load=0;
      for(int k=0;k<LEDS;k++){
        struct frgb c=social_scene_render(&s,t,k,LEDS);
        c=social_scene_vivid(c,plan.hue[0]);
        float m=c.r>c.g?c.r:c.g; m=m>c.b?m:c.b; if(m>1.f){c.r/=m;c.g/=m;c.b/=m;}
        float out=255.f*BRIGHT;
        px[k][0]=(uint8_t)(c.r*out); px[k][1]=(uint8_t)(c.g*out); px[k][2]=(uint8_t)(c.b*out);
        load+=px[k][0]+px[k][1]+px[k][2];
      }
      uint32_t target=led_budget_scale_q8(load,budget);
      applied=led_budget_slew_q8(applied,target,LED_BUDGET_RELEASE_STEP_Q8);
      if(applied<256){ capped++; for(int k=0;k<LEDS;k++) for(int c=0;c<3;c++) px[k][c]=(uint8_t)((px[k][c]*applied)>>8); }
      load_sum+=load; if(load>load_max) load_max=load;
      printf("%s[",f?",":"");
      for(int k=0;k<LEDS;k++) printf("%s%u,%u,%u",k?",":"",px[k][0],px[k][1],px[k][2]);
      printf("]");
    }
    printf("],\"load_led\":%.1f,\"load_max_led\":%.1f,\"capped_pct\":%.1f}",
           load_sum/(FPS*SECS)/LEDS,(double)load_max/LEDS,100.0*capped/(FPS*SECS));
    }
  }
  printf("}\n");
  return 0;
}
