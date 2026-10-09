// ★★★ THE EXTERNAL ANTENNA SWITCH OVER MQTT — vibe_mqtt.h + vibe_antswitch.h (2026-10-09).
//
// No switch exists here, so a FAKE BROKER on loopback plays one: it accepts the client, answers CONNACK, records the
// subscriptions, announces a Tasmota relay board on the discovery topic, executes POWERn commands like Tasmota does (with
// its Interlock: one relay on at a time) and reports the state back. Checked:
//   1. the codec against the spec's own bytes (remaining length, CONNECT, SUBSCRIBE, PUBLISH);
//   2. discovery: a Tasmota board and a Home-Assistant board found with their names and relays;
//   3. selection is BREAK BEFORE MAKE — every other relay OFF before the chosen one ON — and the state read back names it;
//   4. two relays reported ON at once is said so (-2), never guessed;
//   5. the client reconnects by itself when the broker drops it.
// ★ SILENT, loopback only.
#include "vibe_mqtt.h"
#include "vibe_antswitch.h"

#include <cstdio>
#include <deque>
#include <string>

using namespace vibe;
static int fails = 0;
static void check(bool ok, const std::string& what) { std::printf("   %s   %s\n", ok ? "ok" : "FAIL", what.c_str()); if (!ok) fails++; }
static std::string hex(const std::string& s) { std::string o; char b[4]; for (unsigned char c : s) { std::snprintf(b, sizeof b, "%02x", c); o += b; } return o; }

/** A one-client MQTT broker that behaves like a Tasmota board with Interlock. */
struct FakeBroker {
    int lfd = -1, port = 0, cfd = -1;
    std::thread th;
    std::atomic<bool> run{true};
    std::mutex m;
    std::vector<std::string> subs;          // filters the client asked for
    std::vector<antswitch::Publish> fromClient;  // in order
    bool relay[4] = {false, false, false, false};
    std::atomic<int> sessions{0};
    std::atomic<bool> dropNext{false};

    void start() {
        lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
        ::bind(lfd, (sockaddr*)&a, sizeof a); ::listen(lfd, 4);
        socklen_t l = sizeof a; getsockname(lfd, (sockaddr*)&a, &l); port = ntohs(a.sin_port);
        th = std::thread([this] { loop(); });
    }
    void stop() { run = false; ::shutdown(lfd, SHUT_RDWR); ::close(lfd); if (cfd >= 0) ::shutdown(cfd, SHUT_RDWR); if (th.joinable()) th.join(); }
    void send(const std::string& s) { if (cfd >= 0) mqtt::sendAll(cfd, s); }
    void state(int i) { send(mqtt::publishPacket("stat/antsw/POWER" + std::to_string(i + 1), relay[i] ? "ON" : "OFF")); }
    void loop() {
        while (run) {
            pollfd p{lfd, POLLIN, 0};
            if (::poll(&p, 1, 200) != 1) continue;
            cfd = ::accept(lfd, nullptr, nullptr);
            if (cfd < 0) continue;
            sessions++;
            std::string buf; char tmp[2048];
            while (run) {
                if (dropNext.exchange(false)) break;
                pollfd q{cfd, POLLIN, 0};
                if (::poll(&q, 1, 100) != 1) continue;
                const ssize_t n = ::recv(cfd, tmp, sizeof tmp, 0);
                if (n <= 0) break;
                buf.append(tmp, size_t(n));
                mqtt::Packet pk; bool bad = false;
                while (mqtt::takePacket(buf, pk, bad)) {
                    if (pk.type == 1) send(std::string("\x20\x02\x00\x00", 4));         // CONNACK accepted
                    else if (pk.type == 8) {                                           // SUBSCRIBE → SUBACK, then retained
                        const std::string& b = pk.body;
                        const size_t tl = (size_t(uint8_t(b[2])) << 8) | uint8_t(b[3]);
                        const std::string f = b.substr(4, tl);
                        { std::lock_guard<std::mutex> lk(m); subs.push_back(f); }
                        std::string ack; ack.push_back(b[0]); ack.push_back(b[1]); ack.push_back(0);
                        send(mqtt::packet(0x90, ack));
                        if (mqtt::topicMatches(f, "tasmota/discovery/A0B1C2D3E4F5/config"))
                            send(mqtt::publishPacket("tasmota/discovery/A0B1C2D3E4F5/config",
                                 R"({"ip":"192.168.86.77","dn":"Generic Antenna Switch Example","fn":["Sky Loop","VHF Vertical",null,null],)"
                                 R"("hn":"antsw","mac":"A0B1C2D3E4F5","t":"antsw","ft":"%prefix%/%topic%/","tp":["cmnd","stat","tele"],"rl":[1,1,1,0,0,0,0,0]})", true));
                        if (mqtt::topicMatches(f, "homeassistant/switch/esp32ant/ant1/config"))
                            send(mqtt::publishPacket("homeassistant/switch/esp32ant/ant1/config",
                                 R"({"name":"Long Wire","~":"esp32ant/ant1","cmd_t":"~/set","stat_t":"~/state","device":{"name":"ESP32 Antenna Box","ids":["esp32ant"]}})", true));
                    } else if (pk.type == 3) {
                        { std::lock_guard<std::mutex> lk(m); fromClient.push_back({pk.topic, pk.payload}); }
                        const std::string pre = "cmnd/antsw/POWER";
                        if (pk.topic.compare(0, pre.size(), pre) == 0) {
                            const int i = std::atoi(pk.topic.c_str() + pre.size()) - 1;
                            if (i >= 0 && i < 4) {
                                const bool on = pk.payload == "ON";
                                if (on) for (int k = 0; k < 4; ++k) if (k != i && relay[k]) { relay[k] = false; state(k); }  // Interlock
                                relay[i] = on; state(i);
                            }
                        }
                    } else if (pk.type == 12) send(std::string("\xD0\x00", 2));        // PINGRESP
                }
            }
            ::close(cfd); cfd = -1;
        }
    }
};

template <class F> static bool waitFor(F f, int ms = 3000) {
    for (int i = 0; i < ms / 20; ++i) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    return f();
}

int main() {
    // ── 1. codec, against the spec ──
    { std::string s; mqtt::putLength(s, 0); mqtt::putLength(s, 127); mqtt::putLength(s, 128); mqtt::putLength(s, 16383); mqtt::putLength(s, 16384);
      check(hex(s) == "007f8001ff7f808001", "remaining length: 0, 127, 128, 16383, 16384 (spec §2.2.3 table) — " + hex(s)); }
    check(hex(mqtt::connectPacket("vs", 30)) == "100e00044d5154540402001e00027673", "CONNECT: MQTT level 4, clean session, keepalive 30");
    check(hex(mqtt::subscribePacket(1, "a/b")) == "820800010003612f6200", "SUBSCRIBE: id 1, filter a/b, QoS 0");
    check(hex(mqtt::publishPacket("a/b", "ON")) == "3007" "0003612f62" "4f4e", "PUBLISH QoS 0: topic a/b, payload ON");
    { std::string buf = mqtt::publishPacket("x/y", "hello") + mqtt::pingPacket(); mqtt::Packet p; bool bad;
      check(mqtt::takePacket(buf, p, bad) && p.type == 3 && p.topic == "x/y" && p.payload == "hello", "a PUBLISH decodes to its topic and payload");
      check(mqtt::takePacket(buf, p, bad) && p.type == 12 && buf.empty(), "…and the packet after it is still there"); }
    check(mqtt::topicMatches("tasmota/discovery/+/config", "tasmota/discovery/ABC/config")
          && !mqtt::topicMatches("tasmota/discovery/+/config", "tasmota/discovery/ABC/sensors")
          && mqtt::topicMatches("stat/#", "stat/antsw/POWER1"), "topic filters: + is one level, # the rest");

    // ── 2–5. against the fake switch ──
    FakeBroker br; br.start();
    std::mutex fm; std::vector<antswitch::Device> found;
    antswitch::StateTracker st;
    std::atomic<int> up{0};
    mqtt::Client cl;
    auto filters = antswitch::discoveryFilters();
    filters.push_back("stat/antsw/+");
    cl.start("127.0.0.1", br.port, "vibeserver-test", "", "", filters,
        [&](const std::string& t, const std::string& p) {
            std::lock_guard<std::mutex> lk(fm);
            antswitch::noteDiscovery(found, t, p);
            st.feed(t, p);
        },
        [&](bool on, const std::string&) { if (on) up++; });
    check(waitFor([&] { return cl.connected(); }), "the client connects to the broker");
    check(waitFor([&] { std::lock_guard<std::mutex> lk(fm); return found.size() >= 2; }), "the search finds both switches");
    antswitch::Device tas;
    { std::lock_guard<std::mutex> lk(fm); for (auto& d : found) if (d.kind == "tasmota") tas = d; }
    check(tas.name == "Generic Antenna Switch Example" && tas.relays.size() == 3,
          "Tasmota: \"" + tas.name + "\" with " + std::to_string(tas.relays.size()) + " relays");
    check(tas.relays.size() == 3 && tas.relays[0].label == "Sky Loop" && tas.relays[2].label == "Relay 3"
          && tas.relays[1].cmdTopic == "cmnd/antsw/POWER2" && tas.relays[1].stateTopic == "stat/antsw/POWER2",
          "…its relays' names and topics (friendly name where set, \"Relay n\" where not)");
    { std::lock_guard<std::mutex> lk(fm); bool ha = false;
      for (auto& d : found) if (d.kind == "homeassistant" && d.name == "ESP32 Antenna Box" && d.relays.size() == 1
                                  && d.relays[0].cmdTopic == "esp32ant/ant1/set" && d.relays[0].label == "Long Wire") ha = true;
      check(ha, "Home-Assistant discovery: \"ESP32 Antenna Box\", relay \"Long Wire\", ~ expanded in its topics"); }

    st.setRelays(tas.relays);
    { std::lock_guard<std::mutex> lk(br.m); br.fromClient.clear(); }
    for (auto& p : antswitch::selectCommands(tas.relays, 1)) cl.publish(p.topic, p.payload);
    check(waitFor([&] { std::lock_guard<std::mutex> lk(fm); return st.connected() == 1; }), "selecting antenna 2: the switch reports relay 2 on");
    { std::lock_guard<std::mutex> lk(br.m);
      bool order = br.fromClient.size() == 3 && br.fromClient[0].payload == "OFF" && br.fromClient[1].payload == "OFF"
                   && br.fromClient[2].topic == "cmnd/antsw/POWER2" && br.fromClient[2].payload == "ON";
      check(order, "…sent BREAK BEFORE MAKE: relays 1 and 3 OFF, then 2 ON"); }
    for (auto& p : antswitch::selectCommands(tas.relays, 0)) cl.publish(p.topic, p.payload);
    check(waitFor([&] { std::lock_guard<std::mutex> lk(fm); return st.connected() == 0; }), "switching to antenna 1 is read back too");

    { antswitch::StateTracker t2; t2.setRelays(tas.relays);
      t2.feed("stat/antsw/POWER1", "ON"); t2.feed("stat/antsw/POWER3", "ON");
      check(t2.connected() == -2, "two relays reported ON at once: said so (-2), not guessed");
      antswitch::StateTracker t3; t3.setRelays(tas.relays);
      t3.feed("stat/antsw/RESULT", R"({"POWER3":"ON"})");
      check(t3.connected() == 2, "Tasmota's stat/<t>/RESULT JSON is read as well");
      check(antswitch::selectCommands(tas.relays, 7).empty(), "an antenna that does not exist sends nothing"); }

    br.dropNext = true;
    check(waitFor([&] { return br.sessions.load() >= 2 && cl.connected(); }, 8000), "the broker drops the client: it reconnects by itself");
    cl.stop(); br.stop();
    std::printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}
