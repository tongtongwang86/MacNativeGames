/* Find Murmur3-32 hash regions: u32 at o == murmur3(data[o+4 .. o+4+len)). Scans each candidate o in [from,to). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static uint32_t rotl(uint32_t x,int r){return (x<<r)|(x>>(32-r));}
static uint32_t fmix(uint32_t h){h^=h>>16;h*=0x85ebca6b;h^=h>>13;h*=0xc2b2ae35;h^=h>>16;return h;}
int main(int argc,char **argv){
 FILE *f=fopen(argv[1],"rb");fseek(f,0,2);long n=ftell(f);rewind(f);uint8_t *d=malloc(n);fread(d,1,n,f);fclose(f);
 long from=strtol(argv[2],0,0),to=strtol(argv[3],0,0);
 for(long o=from;o<to&&o+8<=n;o+=4){
  uint32_t want=*(uint32_t*)(d+o),h=0;
  for(long p=o+4;p+4<=n;p+=4){uint32_t k=*(uint32_t*)(d+p);k*=0xcc9e2d51;k=rotl(k,15);k*=0x1b873593;h^=k;h=rotl(h,13);h=h*5+0xe6546b64;
   if(fmix(h^(uint32_t)(p+4-o-4))==want){printf("region hash@0x%lx data 0x%lx len 0x%lx\n",o,o+4,p+4-o-4);fflush(stdout);o=p;break;}}
 }
}
