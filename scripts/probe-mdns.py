#!/usr/bin/env python3
"""probe-mdns.py — ask one host the mDNS questions real Apple and Android resolvers ask, and say which it answered.

READ-ONLY: it sends QUERIES only (never a response, never a probe for a name), so it is safe against a live server.

    scripts/probe-mdns.py 192.168.86.158 vibeserver-portable [--instance "VibeServer Portable"]

The host argument filters replies by source address; the label is the .local name WITHOUT ".local". The service
instance defaults to the label (that is what our responder publishes: <label>._vibesdr._tcp.local).

Shapes (each is one packet, the way mDNSResponder / Android's MdnsDiscoveryManager / avahi send them):
  lone-A            one A question, uncompressed
  A+AAAA(ptr)       A, then AAAA by pointer to the first name            -> want A AND an NSEC saying "no AAAA"
  AAAA+A(ptr)       AAAA first, then A by pointer                        -> want A AND NSEC
  other+ours(ptr)   someone else's question first; ours compressed against its ".local"
  QU-A              A with the unicast-response bit                       -> unicast reply to our 5353
  legacy-A          A from a random source port (RFC 6762 §6.7)           -> reply to THAT port, id echoed, TTL <= 10
  browse            PTR _vibesdr._tcp.local                               -> PTR + SRV/TXT/A (additionals)
  browse-packed     PTR _airplay._tcp + _vibesdr._tcp(ptr _tcp.local) + _raop._tcp(ptr) — Apple's packing
  browse-KA         browse with OUR PTR as a known answer (TTL 4500)      -> SILENCE (known-answer suppression)
  browse-KA-stale   same, known-answer TTL 10 (below half)                -> answered
  browse-TC         browse with TC set, the known answers in a 2nd packet  -> silence (it waits for the rest)
  srv               SRV+TXT for the instance (what a resolve asks)
"""
import argparse, os, random, socket, struct, sys, time

GROUP = "224.0.0.251"


def enc(name):
    out = b""
    for p in name.rstrip(".").split("."):
        b = p.encode()
        out += bytes([len(b)]) + b
    return out + b"\0"


def hdr(qd, an=0, flags=0, txid=0):
    return struct.pack("!HHHHHH", txid, flags, qd, an, 0, 0)


def qt(t, qu=False):
    return struct.pack("!HH", t, 0x8001 if qu else 0x0001)


def skip_name(d, i):
    while i < len(d):
        l = d[i]
        if l == 0:
            return i + 1
        if l & 0xC0 == 0xC0:
            return i + 2
        i += 1 + l
    return len(d)


def read_name(d, i, hops=0):
    labels = []
    while i < len(d) and hops < 20:
        l = d[i]
        if l == 0:
            break
        if l & 0xC0 == 0xC0:
            ptr = ((l & 0x3F) << 8) | d[i + 1]
            labels.append(read_name(d, ptr, hops + 1))
            break
        labels.append(d[i + 1:i + 1 + l].decode(errors="replace"))
        i += 1 + l
    return ".".join(x for x in labels if x)


TYPES = {1: "A", 12: "PTR", 16: "TXT", 28: "AAAA", 33: "SRV", 47: "NSEC", 255: "ANY"}


def parse(d):
    """[(section, name, type, ttl, flush)] for every record in a response."""
    qd, an, ns, ar = struct.unpack("!HHHH", d[4:12])
    i = 12
    for _ in range(qd):
        i = skip_name(d, i) + 4
    out = []
    for sec, cnt in (("an", an), ("ns", ns), ("ar", ar)):
        for _ in range(cnt):
            name = read_name(d, i)
            i = skip_name(d, i)
            t, c, ttl, rdl = struct.unpack("!HHIH", d[i:i + 10])
            out.append((sec, name, TYPES.get(t, t), ttl, bool(c & 0x8000)))
            i += 10 + rdl
    return out


def mcast_sock():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.bind(("", 5353))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton(GROUP) + socket.inet_aton("0.0.0.0"))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
    return s


def ask(host, pkts, legacy=False, wait=1.6):
    """Send `pkts` (a list, sent ~20 ms apart) and collect every response from `host`."""
    if legacy:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("", 0))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
    else:
        s = mcast_sock()
    s.settimeout(0.1)
    for p in pkts:
        s.sendto(p, (GROUP, 5353))
        time.sleep(0.02)
    got, t0 = [], time.time()
    while time.time() - t0 < wait:
        try:
            d, a = s.recvfrom(9000)
        except socket.timeout:
            continue
        if a[0] == host and len(d) >= 12 and d[2] & 0x80:
            got.append((round(time.time() - t0, 3), d))
    s.close()
    return got


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("label")
    ap.add_argument("--instance", default=None)
    ap.add_argument("--only", default=None, help="comma list of shape names")
    a = ap.parse_args()
    host_name = a.label + ".local"
    inst = (a.instance or a.label) + "._vibesdr._tcp.local"
    H = enc(host_name)
    T = enc("_vibesdr._tcp.local")
    I = enc(inst)

    def ka_ptr(ttl):
        return T + struct.pack("!HHIH", 12, 1, ttl, len(I)) + I

    other = enc("living-room-tv.local")
    ours_ptr_local = bytes([len(a.label)]) + a.label.encode() + struct.pack("!H", 0xC000 | (12 + 1 + 14))
    air = enc("_airplay._tcp.local")
    tcp_at = 12 + 1 + len("_airplay")
    packed = (hdr(3) + air + qt(12) + b"\x08_vibesdr" + struct.pack("!H", 0xC000 | tcp_at) + qt(12)
              + b"\x05_raop" + struct.pack("!H", 0xC000 | tcp_at) + qt(12))
    txid = random.randint(1, 0xFFFF)
    shapes = [
        ("lone-A", [hdr(1) + H + qt(1)], False),
        ("A+AAAA(ptr)", [hdr(2) + H + qt(1) + b"\xC0\x0C" + qt(28)], False),
        ("AAAA+A(ptr)", [hdr(2) + H + qt(28) + b"\xC0\x0C" + qt(1)], False),
        ("other+ours(ptr)", [hdr(2) + other + qt(1) + ours_ptr_local + qt(1)], False),
        ("QU-A", [hdr(1) + H + qt(1, qu=True)], False),
        ("legacy-A", [hdr(1, txid=txid) + H + qt(1)], True),
        ("browse", [hdr(1) + T + qt(12)], False),
        ("browse-packed", [packed], False),
        ("browse-KA", [hdr(1, an=1) + T + qt(12) + ka_ptr(4500)], False),
        ("browse-KA-stale", [hdr(1, an=1) + T + qt(12) + ka_ptr(10)], False),
        ("browse-TC", [hdr(1, flags=0x0200) + T + qt(12), hdr(0, an=1) + ka_ptr(4500)], False),
        ("srv", [hdr(2) + I + qt(33) + b"\xC0\x0C" + qt(16)], False),
    ]
    only = set(a.only.split(",")) if a.only else None
    for name, pkts, legacy in shapes:
        if only and name not in only:
            continue
        got = ask(a.host, pkts, legacy=legacy)
        if not got:
            print("%-16s  NO REPLY" % name)
        for t, d in got:
            recs = parse(d)
            desc = " ".join("%s:%s%s/%d%s" % (s, TYPES.get(ty, ty) if isinstance(ty, int) else ty,
                                              "" if s == "an" else "", ttl, "!" if fl else "")
                            for s, n, ty, ttl, fl in recs)
            extra = ""
            if legacy:
                extra = " id=%s qd=%d" % ("ok" if struct.unpack("!H", d[:2])[0] == txid else "WRONG",
                                         struct.unpack("!H", d[4:6])[0])
            print("%-16s  +%.3fs  %s%s" % (name, t, desc, extra))
        time.sleep(0.3)


if __name__ == "__main__":
    main()
