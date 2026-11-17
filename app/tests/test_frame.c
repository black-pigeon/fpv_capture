/* Packet ABI fixtures exercise real PAL/NTSC dimensions and field order. */
#include "fpv_frame.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static void put32(uint8_t *p, uint32_t n)
{ p[0]=n; p[1]=n>>8; p[2]=n>>16; p[3]=n>>24; }
static void check(unsigned ntsc)
{
    unsigned h=ntsc?480:576, y, x, row=0;
    size_t len=720*h+20;
    uint8_t *p=calloc(1,len), *gray=malloc(720*h);
    struct fpv_frame_info info;
    assert(p && gray);
    put32(p,0x46505647); put32(p+4,0x02040017 | (ntsc<<16));
    put32(p+8,(720u<<16)|h); put32(p+12,720*h/4);
    put32(p+len-4,0x454e4400);
    for(unsigned field=0;field<2;field++)
        for(y=field;y<h;y+=2,row++)
            for(x=0;x<720;x++) p[16+row*720+x]=(uint8_t)(y+3*x);
    assert(fpv_frame_parse(p,len,&info,gray,720*h)==0);
    assert(info.channel==2 && info.standard==ntsc && info.sequence==23);
    assert(info.width==720 && info.height==h);
    assert(gray[0]==0 && gray[720]==1 && gray[1440]==2);
    assert(gray[(h-1)*720+719]==(uint8_t)(h-1+3*719));
    assert(fpv_frame_parse(p,len-1,&info,gray,720*h)<0);
    assert(fpv_frame_parse(p,len,&info,gray,720*h-1)<0);
    p[len-4]=1; assert(fpv_frame_parse(p,len,&info,gray,720*h)<0); p[len-4]=0;
    p[0]^=1; assert(fpv_frame_parse(p,len,&info,gray,720*h)<0); p[0]^=1;
    p[7]=1; assert(fpv_frame_parse(p,len,&info,gray,720*h)<0); p[7]=2;
    p[6]|=8; assert(fpv_frame_parse(p,len,&info,gray,720*h)<0); p[6]&=~8;
    p[8]^=1; assert(fpv_frame_parse(p,len,&info,gray,720*h)<0); p[8]^=1;
    p[12]^=1; assert(fpv_frame_parse(p,len,&info,gray,720*h)<0); p[12]^=1;
    p[len-1]^=1; assert(fpv_frame_parse(p,len,&info,gray,720*h)<0);
    free(gray);free(p);
}
int main(void) {check(0);check(1);puts("PASS frame ABI, size, errors and field order");return 0;}
