/* fbstat: classify an 800x600 BMP dump from GMBOX_FB_DUMP. Prints "<clock_white_fraction> <taskbar_luma>".
 * lock screen: fraction > 0.2 and luma < 100; desktop: luma > 200. */
#include <stdio.h>
#include <stdint.h>
int main(int c,char**v){FILE*f=fopen(v[1],"rb");if(!f)return 1;static uint8_t b[54+800*600*4];if(fread(b,1,sizeof b,f)<sizeof b)return 1;
 int w=0,n=0; for(int y=110;y<190;y++)for(int x=300;x<500;x++){uint8_t*p=b+54+(y*800+x)*4;n++;if(p[0]>200&&p[1]>200&&p[2]>200)w++;}
 long s=0,m=0;for(int y=575;y<595;y++)for(int x=200;x<600;x++){uint8_t*p=b+54+(y*800+x)*4;s+=(p[0]+p[1]+p[2])/3;m++;}
 printf("%.3f %ld\n",(double)w/n,s/m);return 0;}
