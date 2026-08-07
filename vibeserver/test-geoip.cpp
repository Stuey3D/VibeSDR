// ★★★ IS THE COUNTRY LOOKUP ACTUALLY RIGHT?
//
// A geo lookup that returns a plausible-looking wrong flag is worse than none: nobody can tell by
// looking, and the whole feature quietly becomes decoration. So this checks real addresses whose
// allocation is a matter of public record, in all five registries, v4 and v6.
//
// ★★ AND IT CHECKS THE NEGATIVES. Private, loopback and malformed addresses must return EMPTY,
//    never a guess — the client renders empty as "no flag", and anything else would put a
//    confident little flag next to somebody on the LAN.
//
//   build:  c++ -std=c++17 -I. -o /tmp/tgeo test-geoip.cpp geoip.cpp
//   run:    /tmp/tgeo [cache-dir]      (downloads ~50 MB the first time)
#include "geoip.h"
#include <cstdio>
#include <string>
// Verified against the registries' own published records — see the comments on the two cases
// that look wrong and are not.
int fails=0;
void ck(const char* ip, const char* want){
  std::string got = geoip::lookup(ip);
  const bool ok = got == want;
  if(!ok){ printf("   FAIL %-22s got '%s' want '%s'\n", ip, got.c_str(), want); fails++; }
  else printf("   ok   %-22s -> %s\n", ip, got.empty()?"(none)":got.c_str());
}
int main(int argc,char**argv){
  geoip::setDir(argc>1?argv[1]:"/tmp/geoiptest");
  if(!geoip::load()){
    printf("no cache — downloading (this takes a minute)...\n");
    std::string err;
    if(!geoip::refresh(err)){ printf("refresh failed: %s\n", err.c_str()); return 2; }
  }
  printf("%d ranges loaded\n\n", geoip::count());
  // Well-known allocations, checked against the registries' own records.
  ck("8.8.8.8","US");            // Google, ARIN
  // ★★ AU, NOT US — and this is the limitation, not a bug. Cloudflare operates 1.1.1.1 from
  //    everywhere, but the REGISTRY record says `apnic|AU|ipv4|1.1.1.0|256|assigned`. This data
  //    answers "which country was this block allocated to", which is all a flag can honestly
  //    claim. My first version of this test asserted US and was simply wrong about the source.
  ck("1.1.1.1","AU");
  ck("212.58.244.20","GB");      // BBC, RIPE
  ck("193.0.6.139","NL");        // RIPE NCC itself
  ck("133.11.0.1","JP");         // University of Tokyo
  ck("200.160.2.3","BR");        // NIC.br, LACNIC
  ck("196.216.2.1","ZA");        // AFRINIC
  // ★ NL: 2001:600::/29 is allocated to RIPE NCC itself, which is Dutch. Same lesson.
  ck("2001:600::1","NL");
  ck("2001:200::1","JP");        // v6, APNIC
  // Must NOT invent an answer:
  ck("10.0.0.5","");             // private
  ck("192.168.1.1","");          // private
  ck("127.0.0.1","");            // loopback
  ck("not-an-ip","");            // rubbish
  printf(fails?"\n%d FAILURES\n":"\nall passed\n",fails);
  return fails?1:0;
}
