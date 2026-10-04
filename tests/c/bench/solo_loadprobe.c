#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "social_scene.h"
#define LEDS 16
static int cmp(const void*a,const void*b){double x=*(double*)a,y=*(double*)b;return x<y?-1:x>y;}
int main(void){
  for(int fam=0;fam<3;fam++){
    static double loads[200000]; int n=0;
    for(uint32_t seed=1;seed<60;seed+=2){
      struct social_scene s; social_scene_init(&s,seed,SOCIAL_SOLO,1);
      for(int64_t e=1;e<30;e++){
        struct social_scene_plan p; social_scene_plan_at(&s,(float)e*60.f+30.f,&p); if(p.family!=fam) continue;
        for(float t=(float)e*60.f+6.f;t<(float)e*60.f+60.f;t+=0.5f){
          double load=0;
          for(int k=0;k<LEDS;k++){ struct frgb c=social_scene_render(&s,t,k,LEDS); c=social_scene_vivid(c,p.hue[0]);
            float m=c.r>c.g?c.r:c.g; m=m>c.b?m:c.b; if(m>1.f){c.r/=m;c.g/=m;c.b/=m;}
            load+=floor(c.r*114.75)+floor(c.g*114.75)+floor(c.b*114.75);}
          loads[n++]=load/LEDS;
        }
      }
    }
    qsort(loads,n,sizeof(double),cmp);
    double mean=0; for(int i=0;i<n;i++) mean+=loads[i]; mean/=n;
    printf("%-13s frames=%d mean=%.1f p50=%.1f p90=%.1f max=%.1f  over45=%.0f%%\n", social_scene_family_name(SOCIAL_SOLO,fam), n, mean, loads[n/2], loads[n*9/10], loads[n-1], 100.0*({int c=0;for(int i=0;i<n;i++)if(loads[i]>45)c++;c;})/n);
  }
  return 0;
}
