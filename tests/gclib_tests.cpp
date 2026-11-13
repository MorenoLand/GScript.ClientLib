#include "gclib.h"
#include "IEnums.h"
#include "miniz.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET test_socket;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int test_socket;
#endif

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

typedef std::vector<uint8_t> Bytes;

struct Builder {
    Bytes v;
    Builder& c(int n) { v.push_back(static_cast<uint8_t>((n + 32) & 0xff)); return *this; }
    Builder& s2(int n) { v.push_back(static_cast<uint8_t>(((n >> 7) & 0x7f) + 32)); v.push_back(static_cast<uint8_t>((n & 0x7f) + 32)); return *this; }
    Builder& i3(int n) { v.push_back(static_cast<uint8_t>(((n >> 14) & 0x7f) + 32)); return s2(n); }
    Builder& i5(long long n) {
        for (int shift = 28; shift >= 0; shift -= 7) v.push_back(static_cast<uint8_t>(((n >> shift) & 0x7f) + 32));
        return *this;
    }
    Builder& raw(const std::string& text) { v.insert(v.end(), text.begin(), text.end()); return *this; }
    Builder& str(const std::string& text) { c(static_cast<int>(text.size())); return raw(text); }
    Builder& str2(const std::string& text) { s2(static_cast<int>(text.size())); return raw(text); }
};

struct Captured {
    std::vector<std::pair<int, Bytes>> packets;
};

void on_capture(int packet_id, const void* data, int length, void* ud) {
    auto* cap = static_cast<Captured*>(ud);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    cap->packets.emplace_back(packet_id, Bytes(p, p + length));
}

std::string decode(int id, const Bytes& payload) {
    char* json = gc_decode_packet_json(id, payload.data(), static_cast<int>(payload.size()));
    std::string out = json;
    gc_free_string(json);
    return out;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

struct SenderFixture {
    GCHandle gc = gc_create("127.0.0.1", 1, "6.037");
    Captured cap;
    SenderFixture() { gc_set_send_capture(gc, on_capture, &cap); }
    ~SenderFixture() { gc_destroy(gc); }
    const Bytes& last() const { return cap.packets.back().second; }
    int last_id() const { return cap.packets.back().first; }
};

Bytes cat(const Bytes& a, const std::string& b) {
    Bytes out = a;
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

// Wire vectors in this block come from the packet reference tables and serializer fixtures.
void test_senders() {
    {
        SenderFixture f;
        gc_send_rc_chat(f.gc, "hello");
        CHECK(f.last_id() == 79 && f.last() == Bytes({'h', 'e', 'l', 'l', 'o'}));
    }
    {
        SenderFixture f;
        const char* args[] = {"7", "-4", "6", "level.nw"};
        CHECK(gc_rc_send(f.gc, "warp_player", args, 4, nullptr, 0) == 1);
        CHECK(f.last_id() == 82);
        CHECK(f.last() == cat(Bytes({32, 39, 28, 38}), "level.nw"));
    }
    {
        SenderFixture f;
        int ids[] = {7, 300};
        gc_send_private_message(f.gc, ids, 2, "hello");
        CHECK(f.last_id() == 28);
        CHECK(f.last() == cat(Bytes({32, 34, 32, 39, 34, 76}), "hello"));
    }
    {
        SenderFixture f;
        gc_send_verify_file_crc(f.gc, "level.nw", 0x12345678u);
        CHECK(f.last_id() == 47);
        CHECK(f.last() == cat(Bytes({33, 49, 113, 76, 152}), "level.nw"));
    }
    {
        SenderFixture f;
        gc_send_request_update_board(f.gc, "level.nw", 0x12345678LL, 1, 2, 3, 4);
        CHECK(f.last_id() == 130);
        Bytes expected = {32 + 8};
        expected = cat(expected, "level.nw");
        for (int b : {33, 49, 113, 76, 152, 32, 33, 32, 34, 32, 35, 32, 36}) expected.push_back(static_cast<uint8_t>(b));
        CHECK(f.last() == expected);
    }
    {
        SenderFixture f;
        gc_send_request_update_package(f.gc, "core.gupd", 1);
        CHECK(f.last_id() == 159);
        Bytes expected = cat(Bytes({32 + 9}), "core.gupd");
        expected.push_back(33);
        CHECK(f.last() == expected);
    }
    {
        SenderFixture f;
        gc_send_shoot2(f.gc, 320, 640, 48, -1, 1, 22, 44, 66, 32, "bow", "power");
        CHECK(f.last_id() == 48);
        Bytes expected = {34, 96, 37, 32, 32, 80, 31, 33, 54, 76, 98, 64, 32, 35};
        expected = cat(expected, "bow");
        expected.push_back(37);
        expected = cat(expected, "power");
        CHECK(f.last() == expected);
    }
    {
        SenderFixture f;
        gc_send_item_take(f.gc, 3.0f, 4.0f, 9);
        CHECK(f.last_id() == 32 && f.last() == Bytes({38, 40}));
        gc_send_enter_level(f.gc);
        CHECK(f.last_id() == 46 && f.last() == Bytes({33}));
        gc_send_explosion(f.gc, 3.0f, 4.0f, 2, 5);
        CHECK(f.last_id() == 27 && f.last() == Bytes({37, 38, 40, 34}));
        gc_send_player_hurt(f.gc, 300, 0, 0, 0, 0, 3);
        CHECK(f.last_id() == 26 && f.last().size() == 8);
        CHECK(f.last()[0] == 34 && f.last()[1] == 76);
    }
    {
        SenderFixture f;
        gc_send_language(f.gc, "English");
        CHECK(f.last_id() == 37 && f.last() == Bytes({'E', 'n', 'g', 'l', 'i', 's', 'h'}));
        gc_send_horse_add(f.gc, 2.0f, 3.0f, 1, 2, "horse.png");
        CHECK(f.last_id() == 7 && f.last()[0] == 36 && f.last()[1] == 38 && f.last()[2] == 32 + 9);
        gc_send_baddy_hurt(f.gc, 4, -1, 1, 5);
        CHECK(f.last_id() == 16 && f.last() == Bytes({36, 31, 33, 37}));
        gc_send_profile_get(f.gc, "target");
        CHECK(f.last_id() == 80 && f.last() == Bytes({'t', 'a', 'r', 'g', 'e', 't'}));
        gc_send_tamper_check(f.gc, 1, 2);
        CHECK(f.last_id() == 39 && f.last().size() == 10);
        gc_send_npc_server_query(f.gc, 7, "hi");
        CHECK(f.last_id() == 94 && f.last() == Bytes({32, 39, 'h', 'i'}));
    }
    {
        SenderFixture f;
        const char* args[] = {"lister", "serverinfo"};
        CHECK(gc_rc_send(f.gc, "set_server_options", args, 2, nullptr, 0) == 1);
        CHECK(f.last_id() == 52 && f.last() == Bytes({'l', 'i', 's', 't', 'e', 'r', ',', 's', 'e', 'r', 'v', 'e', 'r', 'i', 'n', 'f', 'o'}));
        const char* bad[] = {"x"};
        CHECK(gc_rc_send(f.gc, "no_such_command", bad, 1, nullptr, 0) == 0);
        const char* npc[] = {"1000"};
        CHECK(gc_nc_send(f.gc, "get_npc", npc, 1, nullptr, 0) == 1);
        CHECK(f.last_id() == 103 && f.last() == Bytes({32, 39, 136}));
    }
}

void test_decoders() {
    CHECK(decode(PLO_UNKNOWN111, cat({}, "cookie=1")) == "{\"cookie\":\"cookie=1\"}");
    {
        Builder b;
        b.c(1).c(2).c(3).c(4).c(5).c(6).c(10).c(20);
        std::string j = decode(PLO_UNKNOWN185, b.v);
        CHECK(contains(j, "\"map_x\":1") && contains(j, "\"block_height\":6") && contains(j, "\"heights\":[10,20]"));
    }
    {
        Builder b;
        b.c(0).c(0).c(5).c(6).c(1).c(1);
        b.v.push_back(0x34);
        b.v.push_back(0x02);
        std::string j = decode(PLO_UNKNOWN186, b.v);
        CHECK(contains(j, "\"tile_x\":5") && contains(j, "\"tiles\":[564]"));
    }
    {
        Builder b;
        b.c(3).c(4).c(9).raw("horse.png");
        std::string j = decode(PLO_HORSEADD, b.v);
        CHECK(contains(j, "\"x\":1.5") && contains(j, "\"y\":2") && contains(j, "\"direction\":1") && contains(j, "\"bushes\":2") && contains(j, "horse.png"));
    }
    {
        Builder b;
        b.c(7).c(-2).c(3).c(9);
        std::string j = decode(PLO_BADDYHURT, b.v);
        CHECK(contains(j, "\"baddy_id\":7") && contains(j, "\"hurt_x\":-2") && contains(j, "\"hurt_y\":3") && contains(j, "\"damage\":9"));
    }
    {
        Builder b;
        b.s2(300).s2(320).s2(640).s2(48).c(-1).c(1).c(22).c(44).c(66).c(32).str2("bow").c(5).raw("power");
        std::string j = decode(PLO_SHOOT2, b.v);
        CHECK(contains(j, "\"player_id\":300") && contains(j, "\"pixel_x\":320") && contains(j, "\"pixel_y\":640") &&
              contains(j, "\"offset_x\":-1") && contains(j, "\"gani\":\"bow\"") && contains(j, "\"parameters\":\"power\""));
    }
    {
        Builder b;
        b.str("john").str("secret").str("j@example.com").c(1).c(0).c(2).str("w1").str("").str("spam");
        std::string j = decode(PLO_RC_ACCOUNTGET, b.v);
        CHECK(contains(j, "\"account\":\"john\"") && contains(j, "\"banned\":true") && contains(j, "\"load_only\":false") &&
              contains(j, "\"admin_level\":2") && contains(j, "\"ban_reason\":\"spam\""));
    }
    {
        Builder b;
        b.str("john").i5(0x12345678LL).str("10.0.0.1").str2("folder1");
        std::string j = decode(PLO_RC_PLAYERRIGHTSGET, b.v);
        CHECK(contains(j, "\"rights\":305419896") && contains(j, "\"admin_ip\":\"10.0.0.1\"") && contains(j, "\"folders\":\"folder1\""));
    }
    {
        Builder b;
        b.s2(2).str("a").str("b");
        CHECK(contains(decode(PLO_RC_SERVERFLAGSGET, b.v), "\"flags\":[\"a\",\"b\"]"));
    }
    {
        std::string j = decode(PLO_SERVERWARP, cat({}, "serverid,\"Server Name\",google.com,443"));
        CHECK(contains(j, "\"name\":\"Server Name\"") && contains(j, "\"host\":\"google.com\"") && contains(j, "\"port\":\"443\""));
    }
    {
        Builder b;
        b.c(6).str("target").str("one").str("two");
        std::string j = decode(PLO_PROFILE, b.v);
        CHECK(contains(j, "\"account\":\"target\"") && contains(j, "[\"one\",\"two\"]"));
    }
    {
        Builder b;
        b.str("gui").raw("start");
        CHECK(contains(decode(PLO_UNKNOWN195, b.v), "\"gani\":\"gui\""));
    }
    {
        Builder b;
        b.i5(123456);
        CHECK(contains(decode(PLO_LARGEFILESIZE, b.v), "\"size\":123456"));
    }
    {
        Builder b;
        b.str("pack").i5(99);
        CHECK(contains(decode(PLO_UPDATEPACKAGESIZE, b.v), "\"package\":\"pack\"") );
    }
    CHECK(decode(9999, {}) == "null");
}

struct EventLog {
    std::vector<std::string> lines;
    std::vector<uint8_t> file;
    int file_calls = 0;
};

void on_horse(int added, float x, float y, int dir, int bushes, const char* image, void* ud) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "horse %d %.1f %.1f %d %d %s", added, x, y, dir, bushes, image);
    static_cast<EventLog*>(ud)->lines.push_back(buf);
}

void on_item(int added, float x, float y, int item, void* ud) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "item %d %.1f %.1f %d", added, x, y, item);
    static_cast<EventLog*>(ud)->lines.push_back(buf);
}

void on_trigger(int player, int npc, float x, float y, const char* action, const char* params, void* ud) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "trigger %d %d %.1f %.1f %s|%s", player, npc, x, y, action, params);
    static_cast<EventLog*>(ud)->lines.push_back(buf);
}

void on_file(const char*, const void* data, int length, int, void* ud) {
    auto* log = static_cast<EventLog*>(ud);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    log->file.assign(p, p + length);
    ++log->file_calls;
}

void on_explosion(float x, float y, int radius, int power, void* ud) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "explosion %.1f %.1f %d %d", x, y, radius, power);
    static_cast<EventLog*>(ud)->lines.push_back(buf);
}

void on_hit(float x, float y, int power, int player, void* ud) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "hit %.1f %.1f %d %d", x, y, power, player);
    static_cast<EventLog*>(ud)->lines.push_back(buf);
}

void on_remote(const char* kind, const char* json, void* ud) {
    static_cast<EventLog*>(ud)->lines.push_back(std::string("remote ") + kind + " " + json);
}

std::string state(GCHandle gc, const char* kind) {
    char* s = gc_get_state_json(gc, kind);
    std::string out = s;
    gc_free_string(s);
    return out;
}

void inject(GCHandle gc, int id, const Bytes& payload) {
    gc_inject_packet(gc, id, payload.data(), static_cast<int>(payload.size()));
}

void test_dispatch_and_state() {
    GCHandle gc = gc_create("127.0.0.1", 1, "6.037");
    EventLog log;
    gc_on_horse(gc, on_horse, &log);
    gc_on_item(gc, on_item, &log);
    gc_on_trigger_action(gc, on_trigger, &log);
    gc_on_file(gc, on_file, &log);
    gc_on_explosion(gc, on_explosion, &log);
    gc_on_hit_objects(gc, on_hit, &log);
    gc_on_remote_event(gc, on_remote, &log);

    inject(gc, PLO_HORSEADD, Builder().c(3).c(4).c(9).raw("h.png").v);
    CHECK(log.lines.back() == "horse 1 1.5 2.0 1 2 h.png");
    CHECK(contains(state(gc, "horses"), "h.png"));
    inject(gc, PLO_HORSEDEL, Builder().c(3).c(4).v);
    CHECK(state(gc, "horses") == "[]");

    inject(gc, PLO_ITEMADD, Builder().c(6).c(8).c(5).v);
    CHECK(log.lines.back() == "item 1 3.0 4.0 5");
    CHECK(contains(state(gc, "items"), "\"item\":5"));
    inject(gc, PLO_ITEMDEL, Builder().c(6).c(8).v);
    CHECK(state(gc, "items") == "[]");

    inject(gc, PLO_TRIGGERACTION, Builder().s2(7).i3(1234).c(4).c(6).raw("hit,a,b").v);
    CHECK(log.lines.back() == "trigger 7 1234 2.0 3.0 hit|a,b");

    inject(gc, PLO_EXPLOSION, Builder().s2(5).c(3).c(10).c(12).c(2).v);
    CHECK(log.lines.back() == "explosion 5.0 6.0 3 2");
    inject(gc, PLO_HITOBJECTS, Builder().s2(5).c(4).c(10).c(12).v);
    CHECK(log.lines.back() == "hit 5.0 6.0 4 5");

    {
        Builder b;
        b.i3(77).c(20).str("npc").c(0).str("door.png").c(2).c(8).c(3).c(-6).c(10).c(7).str("sword7.png").c(19).c(1).c(2).c(3).c(4).c(5);
        inject(gc, PLO_NPCPROPS, b.v);
        std::string j = state(gc, "npcs");
        CHECK(contains(j, "\"id\":77") && contains(j, "\"nickname\":\"npc\"") && contains(j, "\"image\":\"door.png\"") &&
              contains(j, "\"x\":4") && contains(j, "\"y\":-3") && contains(j, "sword7.png") && contains(j, "\"colors\":[1,2,3,4,5]"));
    }
    inject(gc, PLO_FLAGSET, Builder().raw("quest=done").v);
    CHECK(contains(state(gc, "flags"), "\"quest\":\"done\""));
    inject(gc, PLO_FLAGDEL, Builder().raw("quest").v);
    CHECK(state(gc, "flags") == "{}");

    inject(gc, PLO_NPCWEAPONADD, Builder().str("Bow").raw("x").v);
    CHECK(contains(state(gc, "weapons"), "Bow"));
    inject(gc, PLO_NPCWEAPONDEL, Builder().raw("Bow").v);
    CHECK(!contains(state(gc, "weapons"), "Bow"));

    inject(gc, PLO_FREEZEPLAYER2, {});
    CHECK(contains(state(gc, "session"), "\"frozen\":true"));
    inject(gc, PLO_SETACTIVELEVEL, Builder().raw("a.nw").v);
    CHECK(contains(state(gc, "session"), "\"active_level\":\"a.nw\""));
    inject(gc, PLO_UNKNOWN111, Builder().raw("ck").v);
    CHECK(contains(state(gc, "session"), "\"net_cookie\":\"ck\""));

    inject(gc, PLO_NEWWORLDTIME, Builder().c(0).c(0).c(1).c(5).v);
    CHECK(contains(state(gc, "session"), "\"world_time\":133"));
    inject(gc, PLO_NEWWORLDTIME, Bytes({0x39, 0x30, 0, 0}));
    CHECK(contains(state(gc, "session"), "\"world_time\":12345"));

    inject(gc, PLO_RC_ACCOUNTLISTGET, Builder().str("a").str("b").v);
    CHECK(log.lines.back().rfind("remote account_list ", 0) == 0 && contains(log.lines.back(), "[\"a\",\"b\"]"));

    Bytes content = {1, 2, 3, 4};
    Bytes file_head = Builder().i5(99).str("big.bin").v;
    inject(gc, PLO_LARGEFILESTART, Builder().raw("big.bin").v);
    inject(gc, PLO_LARGEFILESIZE, Builder().i5(8).v);
    inject(gc, PLO_FILE, [&] { Bytes b = file_head; b.insert(b.end(), content.begin(), content.end()); return b; }());
    CHECK(log.file_calls == 0);
    inject(gc, PLO_FILE, [&] { Bytes b = file_head; b.insert(b.end(), content.begin(), content.end()); return b; }());
    inject(gc, PLO_LARGEFILEEND, Builder().raw("big.bin").v);
    CHECK(log.file_calls == 1 && log.file.size() == 8);

    gc_destroy(gc);
}

#ifdef GCLIB_LISTSERVER
struct ListLog {
    std::vector<std::string> kinds;
    std::string servers;
};

void on_list(const char* kind, const char* json, void* ud) {
    auto* log = static_cast<ListLog*>(ud);
    log->kinds.push_back(kind);
    if (std::strcmp(kind, "servers") == 0) log->servers = json;
}

void test_list_server() {
    GCHandle gc = gc_list_create("127.0.0.1", 1, "2.22");
    ListLog log;
    gc_list_on_event(gc, on_list, &log);
    auto entry = [](Builder& b, const char* name, const char* host, const char* port) {
        b.c(8).str(name).str("English").str("desc").str("http://x").str("1.0").str("3").str(host).str(port);
    };
    Builder servers;
    servers.c(2);
    entry(servers, "One", "10.0.0.1", "14900");
    entry(servers, "Two", "10.0.0.2", "14901");
    inject(gc, 0, servers.v);
    CHECK(contains(log.servers, "\"name\":\"One\"") && contains(log.servers, "\"port\":\"14901\""));
    char* list = gc_list_get_servers_json(gc);
    CHECK(contains(list, "10.0.0.2"));
    gc_free_string(list);
    inject(gc, 4, Builder().raw("bad password").v);
    CHECK(std::string(gc_get_last_login_error(gc)) == "bad password");
    CHECK(log.kinds.back() == "error");
    gc_list_destroy(gc);
}

#endif

// -- reconnect ---------------------------------------------------------------

struct StateLog {
    std::vector<std::string> states;
};

void on_state(const char* state_name, int, const char*, void* ud) {
    static_cast<StateLog*>(ud)->states.push_back(state_name);
}

void test_reconnect() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    test_socket listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    CHECK(listen(listener, 4) == 0);
    socklen_t len = sizeof(addr);
    getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    int port = ntohs(addr.sin_port);

    std::atomic<int> accepted{0};
    std::thread server([&] {
        for (int n = 0; n < 2; ++n) {
            test_socket s = accept(listener, nullptr, nullptr);
            if (s == static_cast<test_socket>(-1)) return;
            ++accepted;
            if (n == 0) {
#ifdef _WIN32
                closesocket(s);
#else
                close(s);
#endif
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
#ifdef _WIN32
                closesocket(s);
#else
                close(s);
#endif
            }
        }
    });

    GCHandle gc = gc_create("127.0.0.1", port, "6.037");
    StateLog log;
    gc_on_connection_state(gc, on_state, &log);
    gc_set_reconnect(gc, 3, 100);
    CHECK(gc_connect(gc) == 1);
    for (int n = 0; n < 60 && accepted < 2; ++n) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(accepted == 2);
    bool saw_reconnecting = false;
    bool saw_reconnected = false;
    for (const auto& s : log.states) {
        if (s == "reconnecting") saw_reconnecting = true;
        if (s == "reconnected") saw_reconnected = true;
    }
    CHECK(saw_reconnecting && saw_reconnected);
    gc_destroy(gc);
    server.join();
#ifdef _WIN32
    closesocket(listener);
    WSACleanup();
#else
    close(listener);
#endif
}

struct EventSink {
    std::vector<std::pair<int, std::string>> events;
};

void on_any_event(int id, const char*, const char* json, void* ud) {
    static_cast<EventSink*>(ud)->events.emplace_back(id, json);
}

// Every server-to-client id the reference tables define must reach a handler; none may fall into "unhandled".
void test_packet_coverage() {
    static const int ids[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
        30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 55, 56, 60, 61, 62, 63, 64, 65,
        66, 67, 68, 69, 70, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 100, 101, 102, 103, 104, 105, 106, 107,
        108, 109, 110, 111, 124, 131, 132, 133, 134, 140, 150, 151, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162,
        163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184,
        185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 197, 198};
    GCHandle gc = gc_create("127.0.0.1", 1, "6.037");
    EventSink sink;
    gc_on_packet_event(gc, on_any_event, &sink);
    Bytes payload = Builder().c(1).c(2).c(3).c(4).c(5).c(6).c(7).c(8).raw("abc").v;
    for (int id : ids) {
        sink.events.clear();
        inject(gc, id, payload);
        for (const auto& e : sink.events) {
            if (e.first == id && contains(e.second, "\"category\":\"unhandled\"")) {
                ++g_failures;
                std::printf("FAIL packet %d reaches no handler\n", id);
            }
        }
        ++g_checks;
    }
    gc_destroy(gc);
}

// Malformed and truncated payloads must never crash the dispatcher.
void test_malformed_payloads() {
    GCHandle gc = gc_create("127.0.0.1", 1, "6.037");
    uint32_t state = 12345;
    auto next = [&state]() { state = state * 1664525u + 1013904223u; return state >> 8; };
    for (int id = 0; id < 256; ++id) {
        if (id == PLO_RAWDATA) continue;
        for (int round = 0; round < 40; ++round) {
            Bytes payload(next() % 40);
            for (auto& b : payload) b = static_cast<uint8_t>(next());
            inject(gc, id, payload);
            char* json = gc_decode_packet_json(id, payload.data(), static_cast<int>(payload.size()));
            gc_free_string(json);
        }
    }
    ++g_checks;
    gc_destroy(gc);
}


// -- full sessions over a loopback socket ---------------------------------------------------------------------------
// The scripted servers below speak the Gen5 framing independently of the library: they inflate the login frame,
// decrypt later client frames, and answer with a zlib first frame followed by an encrypted frame. The seed is a
// throwaway value used only by these tests.

uint32_t advance(uint32_t it, int key) { return it * 0x08088405u + static_cast<uint32_t>(key); }

Bytes crypt(const Bytes& input, uint32_t& it, int key, int limit_blocks) {
    Bytes out = input;
    int bytes = static_cast<int>(out.size());
    if (limit_blocks > 0) bytes = (std::min)(bytes, limit_blocks * 4);
    int blocks_left = limit_blocks;
    uint32_t stream = it;
    for (int i = 0; i < bytes; ++i) {
        if ((i & 3) == 0) {
            if (blocks_left == 0) break;
            stream = advance(stream, key);
            it = stream;
            if (blocks_left > 0) --blocks_left;
        }
        out[static_cast<size_t>(i)] ^= static_cast<uint8_t>((stream >> ((i & 3) * 8)) & 0xff);
    }
    return out;
}

bool read_exact(test_socket s, Bytes& out, size_t count) {
    out.resize(count);
    size_t got = 0;
    while (got < count) {
        int n = recv(s, reinterpret_cast<char*>(out.data()) + got, static_cast<int>(count - got), 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

Bytes inflate_bytes(const Bytes& in) {
    mz_ulong size = 65536;
    Bytes out(size);
    if (mz_uncompress(out.data(), &size, in.data(), static_cast<mz_ulong>(in.size())) != MZ_OK) return Bytes();
    out.resize(size);
    return out;
}

Bytes deflate_bytes(const Bytes& in) {
    mz_ulong size = mz_compressBound(static_cast<mz_ulong>(in.size()));
    Bytes out(size);
    mz_compress(out.data(), &size, in.data(), static_cast<mz_ulong>(in.size()));
    out.resize(size);
    return out;
}

void send_all_bytes(test_socket s, const Bytes& data) {
    send(s, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0);
}

void close_test_socket(test_socket s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

test_socket open_listener(int& port) {
    test_socket listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    CHECK(listen(listener, 1) == 0);
    socklen_t len = sizeof(addr);
    getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    port = ntohs(addr.sin_port);
    return listener;
}

Bytes first_frame(const Bytes& raw_packets) {
    Bytes z = deflate_bytes(raw_packets);
    Bytes frame = {static_cast<uint8_t>(z.size() >> 8), static_cast<uint8_t>(z.size() & 0xff)};
    frame.insert(frame.end(), z.begin(), z.end());
    return frame;
}

// Reads one post-login client frame and returns its decrypted packet (id, then body without id/key/newline).
bool read_client_packet(test_socket s, uint32_t& it, int key, bool first, int& id, Bytes& body) {
    Bytes header, frame;
    if (!read_exact(s, header, 2)) return false;
    size_t frame_len = (static_cast<size_t>(header[0]) << 8) | header[1];
    if (!read_exact(s, frame, frame_len) || frame.empty()) return false;
    int comp = frame[0];
    Bytes dec = crypt(Bytes(frame.begin() + 1, frame.end()), it, key, comp == 2 ? 12 : 4);
    if (comp == 4) dec = inflate_bytes(dec);
    if (dec.size() < 2) return false;
    id = dec[0] - 32;
    body.assign(dec.begin() + (first ? 2 : 1), dec.end() - 1);
    return true;
}

struct Session {
    std::string account, password, protocol;
    Bytes client_packet;
    int client_packet_id = -1;
    bool login_ok = false;
};

void test_session() {
    const uint32_t seed = 0x2A3B4C5Du;
    int port = 0;
    test_socket listener = open_listener(port);
    Session session;
    std::thread server([&] {
        test_socket s = accept(listener, nullptr, nullptr);
        Bytes header, body;
        if (!read_exact(s, header, 2)) return;
        size_t frame_len = (static_cast<size_t>(header[0]) << 8) | header[1];
        if (!read_exact(s, body, frame_len)) return;
        Bytes raw = inflate_bytes(body);
        if (raw.size() < 12) return;
        int key = raw[1] - 32;
        session.protocol.assign(raw.begin() + 2, raw.begin() + 10);
        size_t pos = 10;
        int alen = raw[pos++] - 32;
        session.account.assign(raw.begin() + pos, raw.begin() + pos + alen);
        pos += static_cast<size_t>(alen);
        int plen = raw[pos++] - 32;
        session.password.assign(raw.begin() + pos, raw.begin() + pos + plen);
        session.login_ok = true;

        Bytes first;
        first.push_back(static_cast<uint8_t>(PLO_SIGNATURE + 32));
        first.push_back(static_cast<uint8_t>(5 + 32));
        first.push_back('\n');
        first.push_back(static_cast<uint8_t>(PLO_FLAGSET + 32));
        for (char c : std::string("quest=done")) first.push_back(static_cast<uint8_t>(c));
        first.push_back('\n');
        send_all_bytes(s, first_frame(first));

        uint32_t in_it = seed;
        read_client_packet(s, in_it, key, true, session.client_packet_id, session.client_packet);

        Bytes second;
        second.push_back(static_cast<uint8_t>(PLO_FLAGSET + 32));
        for (char c : std::string("later=yes")) second.push_back(static_cast<uint8_t>(c));
        second.push_back('\n');
        uint32_t out_it = seed;
        Bytes sealed = crypt(second, out_it, key, 12);
        Bytes out = {static_cast<uint8_t>((sealed.size() + 1) >> 8), static_cast<uint8_t>((sealed.size() + 1) & 0xff), 2};
        out.insert(out.end(), sealed.begin(), sealed.end());
        send_all_bytes(s, out);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        close_test_socket(s);
    });

    GCHandle gc = gc_create("127.0.0.1", port, "6.037");
    gc_set_cipher_seed(gc, seed);
    CHECK(gc_connect(gc) == 1);
    CHECK(gc_login(gc, "tester", "pw123") == 1);
    for (int n = 0; n < 60 && !gc_is_authenticated(gc); ++n) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(gc_is_authenticated(gc) == 1);
    CHECK(contains(state(gc, "flags"), "\"quest\":\"done\""));
    CHECK(gc_set_flag(gc, "a=b") == 1);
    for (int n = 0; n < 60 && !contains(state(gc, "flags"), "later"); ++n) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(contains(state(gc, "flags"), "\"later\":\"yes\""));
    gc_destroy(gc);
    server.join();
    CHECK(session.login_ok);
    CHECK(session.account == "tester" && session.password == "pw123");
    CHECK(session.client_packet_id == PLI_FLAGSET && session.client_packet == Bytes({'a', '=', 'b'}));
    close_test_socket(listener);
}

#ifdef GCLIB_LISTSERVER
void test_list_session() {
    const uint32_t seed = 0x2A3B4C5Du;
    int port = 0;
    test_socket listener = open_listener(port);
    int hello_id = -1, request_id = -1;
    Bytes hello_body, request_body;
    std::thread server([&] {
        test_socket s = accept(listener, nullptr, nullptr);
        uint32_t it = seed;
        // The first list-server packet is a plain zlib frame; later frames use the session key.
        Bytes header, frame;
        if (!read_exact(s, header, 2)) return;
        size_t frame_len = (static_cast<size_t>(header[0]) << 8) | header[1];
        if (!read_exact(s, frame, frame_len)) return;
        Bytes dec = inflate_bytes(frame);
        if (dec.size() < 3) return;
        int key = dec[1] - 32;
        hello_id = dec[0] - 32;
        hello_body.assign(dec.begin() + 2, dec.end() - 1);
        if (!read_client_packet(s, it, key, false, request_id, request_body)) return;

        Bytes reply;
        auto push_packet = [&reply](int id, const Bytes& body) {
            reply.push_back(static_cast<uint8_t>(id + 32));
            reply.insert(reply.end(), body.begin(), body.end());
            reply.push_back('\n');
        };
        Builder servers;
        servers.c(1).c(8).str("One").str("English").str("desc").str("http://x").str("1.0").str("3").str("10.0.0.1").str("14900");
        push_packet(0, servers.v);
        push_packet(2, Builder().raw("Welcome").v);
        uint32_t out_it = seed;
        Bytes sealed = crypt(reply, out_it, key, 12);
        Bytes out = {static_cast<uint8_t>((sealed.size() + 1) >> 8), static_cast<uint8_t>((sealed.size() + 1) & 0xff), 2};
        out.insert(out.end(), sealed.begin(), sealed.end());
        send_all_bytes(s, out);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        close_test_socket(s);
    });

    GCHandle gc = gc_list_create("127.0.0.1", port, "2.22");
    gc_set_cipher_seed(gc, seed);
    ListLog log;
    gc_list_on_event(gc, on_list, &log);
    CHECK(gc_list_connect(gc) == 1);
    CHECK(gc_list_request_servers(gc, "acct", "pass") == 1);
    for (int n = 0; n < 60 && log.kinds.size() < 2; ++n) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(contains(log.servers, "\"name\":\"One\"") && contains(log.servers, "\"port\":\"14900\""));
    CHECK(!log.kinds.empty() && log.kinds.back() == "status");
    gc_list_destroy(gc);
    server.join();
    CHECK(hello_id == 7);
    CHECK(std::string(hello_body.begin(), hello_body.end()) == "GNW03014newmain");
    CHECK(request_id == 1);
    CHECK(request_body == Builder().str("acct").str("pass").v);
    close_test_socket(listener);
}

#endif

} // namespace

int main() {
    test_packet_coverage();
    test_malformed_payloads();
    test_senders();
    test_decoders();
    test_dispatch_and_state();
#ifdef GCLIB_LISTSERVER
    test_list_server();
#endif
    test_reconnect();
    test_session();
#ifdef GCLIB_LISTSERVER
    test_list_session();
#endif
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
