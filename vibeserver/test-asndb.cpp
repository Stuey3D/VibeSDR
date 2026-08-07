// ★★★ IS THE ASN LOOKUP RIGHT?
// Same reasoning as test-geoip.cpp: a plausible-looking wrong network name is worse than none,
// because an owner will BAN on it. These are well-known announcements anyone can verify.
// ★★ And the negatives matter more here than for country: an ASN ban acts on a whole network, so
//    a lookup that guesses would block thousands of innocent people.
//   build: c++ -std=c++17 -I. -o /tmp/tasn test-asndb.cpp asndb.cpp
#include "asndb.h"
#include <cstdio>
#include <string>
int fails = 0;
void ck(const char* ip, uint32_t wantAsn, const char* wantNamePart) {
  uint32_t a = 0; std::string n;
  const bool got = asndb::lookup(ip, a, n);
  const bool ok = wantAsn ? (got && a == wantAsn && n.find(wantNamePart) != std::string::npos)
                          : !got;
  if (!ok) { printf("   FAIL %-20s got %s AS%u '%s'\n", ip, got?"hit":"miss", a, n.c_str()); fails++; }
  else if (wantAsn) printf("   ok   %-20s AS%u %s\n", ip, a, n.c_str());
  else              printf("   ok   %-20s (unknown, as it should be)\n", ip);
}
int main(int argc, char** argv) {
  asndb::setDir(argc > 1 ? argv[1] : "/tmp/geoiptest");
  if (!asndb::load()) {
    printf("no cache — downloading (~9 MB)...\n");
    std::string err;
    if (!asndb::refresh(err)) { printf("refresh failed: %s\n", err.c_str()); return 2; }
  }
  printf("%d ranges loaded\n\n", asndb::count());
  ck("8.8.8.8",       15169, "GOOGLE");
  ck("1.1.1.1",       13335, "CLOUDFLARE");
  // ★ AS number, not the brand: 36692 is right and the NAME is now CISCO-UMBRELLA (Cisco bought
  //   OpenDNS). Assert on the number and a stable fragment — org names churn, ASNs do not.
  ck("208.67.222.222", 36692, "UMBRELLA");
  ck("2606:4700::1111", 13335, "CLOUDFLARE");
  ck("2001:4860:4860::8888", 15169, "GOOGLE");
  // Must NOT invent an answer — an ASN ban on a guess blocks a whole network.
  ck("10.0.0.5",   0, "");
  ck("192.168.1.1",0, "");
  ck("127.0.0.1",  0, "");
  ck("not-an-ip",  0, "");
  printf(fails ? "\n%d FAILURES\n" : "\nall passed\n", fails);
  return fails ? 1 : 0;
}
