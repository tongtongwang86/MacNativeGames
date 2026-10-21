/* Local play without the game: two processes on loopback, one hosting and one joining through the same nn::ldn and
   nn::socket replacements the game calls (runtime/coop.h). Build and run:
     xcrun clang -std=c11 -Iruntime -o /tmp/coop_loopback tests/coop_loopback.c runtime/coop_lan.c runtime/ldn_host.c runtime/socket_host.c
     TOTK_PROFILE_DIR=/tmp/pa /tmp/coop_loopback host &  TOTK_PROFILE_DIR=/tmp/pb ACNH_COOP_PORT_OFFSET=100 /tmp/coop_loopback visitor
   The visitor finds the island, joins as 10.114.0.2, sends "ping" to the host and gets "pong" back by broadcast. */
#include "coop.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
_Noreturn void runtime_fail(const char *m){fprintf(stderr,"FAIL %s\n",m);exit(1);}
uint64_t runtime_now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;}
int runtime_core_away(void){return 1;} void runtime_core_back(int *a){(void)a;}
void hle_bind_system_event(void *e,int a){(void)e;(void)a;} static int signals; void hle_signal_system_event(void *e){(void)e;signals++;} void hle_unbind_system_event(void *e){(void)e;}
void *runtime_symbol(const char *n){(void)n;return NULL;}
#define F(name) runtime_ldn_override(name)
#define S(name) runtime_socket_override(name)
typedef struct { LdnIntentId intent; uint16_t channel; uint8_t max; uint8_t r0; int16_t ver; uint8_t r1[10]; } Cfg;
typedef struct { uint16_t mode, size; uint8_t pass[0x40]; } Sec;
typedef struct { char name[33]; uint8_t r[15]; } User;
typedef struct { LdnNetworkId id; uint32_t type; uint8_t bssid[6]; LdnSsid ssid; uint8_t r[16]; uint32_t flag; } Filter;
int main(int argc,char **argv){
 int host=!strcmp(argv[1],"host"); static int ev;
 ((uint32_t(*)(void))F("_ZN2nn3ldn10InitializeEv"))();
 ((void(*)(void*))F("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE"))(&ev);
 ((uint32_t(*)(void*,size_t,size_t,int))S("_ZN2nn6socket10InitializeEPvmmi"))(0,0,0,0);
 int (*sock)(int,int,int)=S("_ZN2nn6socket6SocketEiii");
 int (*bind_)(int,const void*,uint32_t)=S("_ZN2nn6socket4BindEiPK8sockaddrj");
 long (*sendto_)(int,const void*,size_t,int,const void*,uint32_t)=S("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj");
 long (*recvfrom_)(int,void*,size_t,int,void*,uint32_t*)=S("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj");
 int (*errno_)(void)=S("_ZN2nn6socket12GetLastErrnoEv");
 User u={{0}}; snprintf(u.name,33,"%s",host?"Host":"Visitor");
 if(host){
  ((uint32_t(*)(void))F("_ZN2nn3ldn15OpenAccessPointEv"))();
  Cfg c={{0}}; c.intent.local_communication_id=0x01006F8002326000ull; c.intent.scene_id=7; c.max=8; c.ver=3; Sec s={0};
  uint8_t adv[16]="island-adv";((uint32_t(*)(const void*,size_t))F("_ZN2nn3ldn16SetAdvertiseDataEPKvm"))(adv,16);
  uint32_t r=((uint32_t(*)(Cfg*,Sec*,User*))F("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE"))(&c,&s,&u);
  printf("host create=%x state=%d\n",r,((int(*)(void))F("_ZN2nn3ldn8GetStateEv"))());
 } else {
  ((uint32_t(*)(void))F("_ZN2nn3ldn11OpenStationEv"))();
  static LdnNetworkInfo found[8]; int n=0; Filter f; memset(&f,0,sizeof f); f.flag=1; f.id.intent.local_communication_id=0x01006F8002326000ull;
  for(int t=0;t<10&&!n;t++)((uint32_t(*)(void*,int*,int,Filter*,int))F("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi"))(found,&n,8,&f,0);
  printf("visitor scan found %d, host user %s adv '%s'\n",n,n?found[0].ldn.nodes[0].user_name:"-",n?(char*)found[0].ldn.advertise_data:"-");
  if(!n)return 1;
  Sec s={0};
  uint32_t r=((uint32_t(*)(void*,Sec*,User*,int,int))F("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE"))(&found[0],&s,&u,3,0);
  printf("visitor connect=%x state=%d\n",r,((int(*)(void))F("_ZN2nn3ldn8GetStateEv"))());
 }
 uint32_t ip,mask;((uint32_t(*)(uint32_t*,uint32_t*))F("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE"))(&ip,&mask);
 printf("%s ip %08x\n",u.name,ip);
 int fd=sock(2,2,17); struct sockaddr_in a={0}; a.sin_len=16;a.sin_family=2;a.sin_port=htons(49150);
 if(bind_(fd,&a,16))printf("bind failed %d\n",errno_());
 if(host){ /* wait for the visitor, then echo */
  for(int i=0;i<200;i++){ static LdnNetworkInfo inf;((uint32_t(*)(void*))F("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE"))(&inf); if(inf.ldn.node_count>1){printf("host sees %d nodes, node1 %s\n",inf.ldn.node_count,inf.ldn.nodes[1].user_name);break;} usleep(50000);}
  char b[64];struct sockaddr_in from;uint32_t fl=16;long k=recvfrom_(fd,b,64,0,&from,&fl);
  printf("host got %ld '%.*s' from %s:%u\n",k,(int)k,b,inet_ntoa(from.sin_addr),ntohs(from.sin_port));
  struct sockaddr_in bc={0};bc.sin_len=16;bc.sin_family=2;bc.sin_port=htons(49150);bc.sin_addr.s_addr=htonl(COOP_BROADCAST);
  printf("host broadcast reply %ld\n",sendto_(fd,"pong",4,0,&bc,16));
  sleep(2);
 } else {
  struct sockaddr_in h={0};h.sin_len=16;h.sin_family=2;h.sin_port=htons(49150);h.sin_addr.s_addr=htonl(COOP_SUBNET|1);
  printf("visitor send %ld\n",sendto_(fd,"ping",4,0,&h,16));
  char b[64];struct sockaddr_in from;uint32_t fl=16;long k=recvfrom_(fd,b,64,0,&from,&fl);
  printf("visitor got %ld '%.*s' from %s:%u\n",k,(int)k,b,inet_ntoa(from.sin_addr),ntohs(from.sin_port));
  ((uint32_t(*)(void))F("_ZN2nn3ldn10DisconnectEv"))();
 }
 printf("%s signals %d\n",u.name,signals); return 0;
}
