
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint64_t u64;
#define ETH_ALEN 6
#define ETH_P_8021Q 0x8100
#define ETH_P_PPP_SES 0x8864
#define OCT_VLAN_HLEN 4
#define OCT_CACHE_LINE 128
#define OCT_PPPOE_HLEN 8
#define OCT_PPP_IPV4 0x0021
#define OCT_PPP_IPV6 0x0057
struct cvmx_wqe { struct { struct { u64 addr; int back; int size; } s; } packet_ptr; struct { int len; } word1; };
struct octeon_ft_flow { int n_eg; u16 eg_vid[2]; bool eg_ppp; u16 eg_sid; };
static u8 mem[4096];
#define BASE 1000
/* @@FUNCTION@@ */
/* build a frame at mem+BASE+off: MACs, tags, [pppoe], IP payload of iplen bytes */
static int build(u8 *p, int ntag, const u16 *vid, bool ppp, u16 sid, bool v6, int iplen) {
  int o=0; for(int i=0;i<12;i++) p[o++]=0xA0+i;
  for(int k=0;k<ntag;k++){p[o++]=0x81;p[o++]=0;p[o++]=vid[k]>>8;p[o++]=vid[k]&0xff;}
  if(ppp){p[o++]=0x88;p[o++]=0x64;p[o++]=0x11;p[o++]=0;p[o++]=sid>>8;p[o++]=sid&0xff;
          p[o++]=(iplen+2)>>8;p[o++]=(iplen+2)&0xff;p[o++]=0;p[o++]=v6?0x57:0x21;}
  else {p[o++]=v6?0x86:0x08;p[o++]=v6?0xdd:0x00;}
  for(int i=0;i<iplen;i++) p[o++]=(u8)(0x40+i);
  return o;
}
static int fails;
static void t(const char*name,int nin,const u16*vin,bool pin,u16 sin,int nout,const u16*vout,bool pout,u16 sout,bool v6){
  u8 exp[256]; int iplen=60; int hl_in=12+4*nin+2+(pin?8:0);
  memset(mem,0xEE,sizeof mem);
  /* place input so that L3 starts at fixed offset BASE+64 */
  int l3=BASE+64; u8*l2=mem+l3-hl_in;
  int flen=build(l2,nin,vin,pin,sin,v6,iplen);
  struct cvmx_wqe w; w.packet_ptr.s.addr=(u64)(l2-mem); w.packet_ptr.s.back=5; w.packet_ptr.s.size=flen; w.word1.len=flen;
  struct octeon_ft_flow f={nout,{vout?vout[0]:0,vout?vout[1]:0},pout,sout};
  octeon_ft_l2_xform_t(&w,l2,nin,pin,&f,v6,iplen);
  int elen=build(exp,nout,vout,pout,sout,v6,iplen);
  u8*got=mem+w.packet_ptr.s.addr;
  int ok = w.word1.len==elen && w.packet_ptr.s.size==elen && !memcmp(got,exp,elen) && (got+ (elen-iplen))==mem+l3;
  printf("%-36s %s (len %d->%d)\n",name,ok?"OK":"FAIL",flen,w.word1.len);
  if(!ok){fails++; for(int i=0;i<elen;i++)printf("%02x%s",got[i],i==elen-1?"\n":" ");for(int i=0;i<elen;i++)printf("%02x%s",exp[i],i==elen-1?"\n":" ");}
}
int main(void){
  u16 v7[2]={7,0}, v40[2]={40,0}, q[2]={100,200}, v36[2]={36,0};
  t("plain->plain",0,0,false,0,0,0,false,0,false);
  t("plain->pppoe",0,0,false,0,0,0,true,0x1234,false);
  t("plain->vlan40+pppoe",0,0,false,0,1,v40,true,0x1234,false);
  t("vlan36->vlan40+pppoe",1,v36,false,0,1,v40,true,0xbeef,false);
  t("pppoe->plain",0,0,true,0x1234,0,0,false,0,false);
  t("vlan40+pppoe->plain",1,v40,true,0x1234,0,0,false,0,false);
  t("vlan40+pppoe->vlan36",1,v40,true,0x1234,1,v36,false,0,false);
  t("qinq+pppoe->qinq",2,q,true,1,2,q,false,0,false);
  t("plain->qinq+pppoe v6",0,0,false,0,2,q,true,9,true);
  t("vlan7+pppoe->plain v6",1,v7,true,0x55,0,0,false,0,true);
  t("vlan7->vlan7 (no ppp)",1,v7,false,0,1,v7,false,0,false);
  t("pppoe->pppoe (sid change)",0,0,true,1,0,0,true,2,false);
  printf("%s\n",fails?"SOME FAILED":"ALL PASSED"); return fails;
}
