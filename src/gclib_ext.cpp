#include "TClient.h"
#include "TClientPacketDispatch.h"
#include "TClientProtocol.h"
#include "TClientProtocolV5.h"
#include "IEnums.h"
#include "miniz.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

TClient* as_client(GCHandle handle) {
    return reinterpret_cast<TClient*>(handle);
}

char* copy_for_abi(const std::string& value) {
    char* out = new char[value.size() + 1];
    std::memcpy(out, value.c_str(), value.size() + 1);
    return out;
}

void put_g5(std::vector<uint8_t>& out, long long value) {
    if (value < 0) value = 0;
    if (value > 0xFFFFFFFFLL) value = 0xFFFFFFFFLL;
    for (int shift = 28; shift >= 0; shift -= 7) {
        out.push_back(static_cast<uint8_t>(((value >> shift) & 0x7f) + 32));
    }
}

void put_signed(std::vector<uint8_t>& out, int value) {
    out.push_back(static_cast<uint8_t>((static_cast<uint8_t>(static_cast<int8_t>(value))) + 32));
}

void put_text(std::vector<uint8_t>& out, const char* text) {
    if (!text) return;
    out.insert(out.end(), text, text + std::strlen(text));
}

void put_half(std::vector<uint8_t>& out, float tiles) {
    int v = static_cast<int>(std::floor(tiles * 2.0f + 0.5f));
    write_gchar(out, std::max(0, std::min(127, v)));
}

std::string csv_item(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else if (c != '\r' && c != '\n') out += c;
    }
    return out + "\"";
}

std::string csv_join(const std::vector<std::string>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ",";
        out += csv_item(items[i]);
    }
    return out;
}

std::string json_text(const std::string& value) {
    std::string out = "\"";
    static const char* hex = "0123456789abcdef";
    for (unsigned char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20 || c >= 0x7f) {
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0x0f]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out + "\"";
}

struct CommandSpec {
    const char* name;
    int packet_id;
    const char* spec;
};

// Spec letters: s = length-prefixed string, S = rest-of-packet string, w = string with 2-char length prefix,
// c = one-char integer, h = two-char integer, i = three-char integer, l = five-char integer, b = flag,
// B = binary blob argument, L = remaining arguments as length-prefixed strings, k = counted list of
// length-prefixed strings, T = remaining arguments as one comma-quoted token string.
const CommandSpec kRemoteCommands[] = {
    {"get_server_options", 51, ""}, {"set_server_options", 52, "T"},
    {"get_folder_config", 53, ""}, {"set_folder_config", 54, "T"},
    {"set_respawn", 55, "B"}, {"set_horse_life", 56, "B"}, {"set_ap_increment", 57, "B"}, {"set_baddy_respawn", 58, "B"},
    {"get_player_props", 59, ""}, {"set_player_props", 60, "hB"},
    {"disconnect_player", 61, "hS"}, {"update_levels", 62, "k"},
    {"admin_message", 63, "S"}, {"private_admin_message", 64, "hS"},
    {"list_rcs", 65, ""}, {"disconnect_rc", 66, ""}, {"apply_reason", 67, "B"},
    {"get_server_flags", 68, ""}, {"set_server_flags", 69, "k"},
    {"account_add", 70, "sssbbc"}, {"account_del", 71, "S"}, {"get_account_list", 72, "ss"},
    {"get_player_props_by_id", 73, "h"}, {"get_player_props_by_account", 74, "s"},
    {"reset_player_props", 75, "S"}, {"set_player_props_by_account", 76, "sB"},
    {"get_account", 77, "S"}, {"set_account", 78, "sssbbcss"},
    {"chat", 79, "S"}, {"warp_player", 82, "hccS"},
    {"get_rights", 83, "s"}, {"set_rights", 84, "slsw"},
    {"get_comments", 85, "s"}, {"set_comments", 86, "sS"},
    {"get_ban", 87, "s"}, {"set_ban", 88, "sbS"},
    {"file_browser_start", 89, ""}, {"file_browser_cd", 90, "S"}, {"file_browser_end", 91, ""},
    {"file_browser_download", 92, "S"}, {"file_browser_upload", 93, "sB"},
    {"file_browser_move", 96, "sS"}, {"file_browser_delete", 97, "S"}, {"file_browser_rename", 98, "ss"},
    {"folder_delete", 160, "S"}, {"large_file_start", 155, "S"}, {"large_file_end", 156, "S"},
    {"blank_162", 162, ""},
};

const CommandSpec kNpcCommands[] = {
    {"get_npc", 103, "i"}, {"delete_npc", 104, "i"}, {"reset_npc", 105, "i"}, {"get_npc_script", 106, "i"},
    {"warp_npc", 107, "iccS"}, {"get_npc_flags", 108, "i"}, {"set_npc_script", 109, "iT"},
    {"set_npc_flags", 110, "iL"}, {"add_npc", 111, "T"}, {"class_edit", 112, "S"}, {"class_add", 113, "sL"},
    {"local_npcs_get", 114, "S"}, {"weapon_list_get", 115, ""}, {"weapon_get", 116, "S"},
    {"weapon_add", 117, "ssS"}, {"weapon_delete", 118, "S"}, {"class_delete", 119, "S"},
    {"level_list_get", 150, ""}, {"level_list_set", 151, "L"},
};

bool build_command(const CommandSpec& cmd, const char* const* args, int arg_count, const void* blob, int blob_length, std::vector<uint8_t>& out) {
    int ai = 0;
    auto next = [&]() -> const char* { return ai < arg_count && args ? args[ai++] : ""; };
    for (const char* sp = cmd.spec; *sp; ++sp) {
        switch (*sp) {
        case 's': write_gstring(out, next()); break;
        case 'S': put_text(out, next()); break;
        case 'w': {
            std::string s = next();
            write_gshort(out, static_cast<int>(s.size()));
            out.insert(out.end(), s.begin(), s.end());
            break;
        }
        case 'c': write_gchar(out, std::atoi(next())); break;
        case 'h': write_gshort(out, std::atoi(next())); break;
        case 'i': write_gint3(out, std::atoi(next())); break;
        case 'l': put_g5(out, std::atoll(next())); break;
        case 'b': {
            const char* v = next();
            bool on = v && (*v == '1' || *v == 't' || *v == 'T' || *v == 'y' || *v == 'Y');
            write_gchar(out, on ? 1 : 0);
            break;
        }
        case 'B':
            if (blob && blob_length > 0) {
                const uint8_t* p = static_cast<const uint8_t*>(blob);
                out.insert(out.end(), p, p + blob_length);
            }
            break;
        case 'L':
            while (ai < arg_count) write_gstring(out, next());
            break;
        case 'k': {
            int remaining = std::max(0, arg_count - ai);
            write_gshort(out, remaining);
            while (ai < arg_count) write_gstring(out, next());
            break;
        }
        case 'T': {
            std::vector<std::string> items;
            while (ai < arg_count) items.push_back(next());
            put_text(out, csv_join(items).c_str());
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

int send_command(GCHandle handle, const CommandSpec* table, size_t table_size, const char* command, const char* const* args, int arg_count, const void* blob, int blob_length) {
    auto* gc = as_client(handle);
    if (!gc || !command) return 0;
    for (size_t n = 0; n < table_size; ++n) {
        if (std::strcmp(table[n].name, command) != 0) continue;
        std::vector<uint8_t> data;
        if (!build_command(table[n], args, arg_count, blob, blob_length, data)) return 0;
        return gc_send_packet(handle, table[n].packet_id, data.data(), static_cast<int>(data.size()));
    }
    std::lock_guard<std::mutex> lock(gc->cb_mutex);
    gc->last_error = std::string("Unknown command: ") + command;
    return 0;
}

#ifdef GCLIB_LISTSERVER
std::string read_entry(TPacketReader& r) {
    if (!r.has()) return "{}";
    r.gchar();
    std::string name = r.gstring();
    std::string language = r.gstring();
    std::string description = r.gstring();
    std::string url = r.gstring();
    std::string version = r.gstring();
    std::string players = r.gstring();
    std::string host = r.gstring();
    std::string port = r.gstring();
    return "{\"name\":" + json_text(name) + ",\"language\":" + json_text(language) + ",\"description\":" + json_text(description) +
           ",\"url\":" + json_text(url) + ",\"version\":" + json_text(version) + ",\"players\":" + json_text(players) +
           ",\"host\":" + json_text(host) + ",\"port\":" + json_text(port) + "}";
}

#endif

void keepalive_loop(TClient* gc) {
    int counter = 0;
    while (!gc->shutting_down && !gc->stop) {
        int waited = 0;
        while (waited < gc->keepalive_interval_ms && !gc->shutting_down && !gc->stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            waited += 50;
        }
        if (gc->shutting_down || gc->stop || gc->keepalive_interval_ms <= 0) break;
        if (!gc->connected || !gc->authenticated) continue;
        std::vector<uint8_t> data;
        write_gshort(data, counter++ & 0x3fff);
        gc_send_packet(gc, PLI_PACKETCOUNT, data.data(), static_cast<int>(data.size()));
    }
}

} // namespace

void tclient_start_keepalive(TClient* gc) {
    if (gc->keepalive_interval_ms <= 0 || gc->keepalive_thread.joinable() || gc->shutting_down) return;
    gc->keepalive_thread = std::thread(keepalive_loop, gc);
}

#ifdef GCLIB_LISTSERVER
bool tclient_dispatch_list_packet(TClient* client, int packet_id, const std::vector<uint8_t>& payload) {
    TPacketReader r(payload);
    std::string kind;
    std::string json;
    switch (packet_id) {
    case 0: {
        kind = "servers";
        int count = r.has() ? r.gchar() : 0;
        std::vector<std::string> entries;
        for (int n = 0; n < count && r.has(); ++n) entries.push_back(read_entry(r));
        json = "[";
        for (size_t n = 0; n < entries.size(); ++n) json += (n ? "," : "") + entries[n];
        json += "]";
        client->list_servers_json = entries;
        break;
    }
    case 1: {
        kind = "add_server";
        json = read_entry(r);
        client->list_servers_json.push_back(json);
        break;
    }
    case 2: kind = "status"; json = json_text(std::string(payload.begin(), payload.end())); break;
    case 3: kind = "site_url"; json = json_text(std::string(payload.begin(), payload.end())); break;
    case 4: {
        kind = "error";
        std::string text(payload.begin(), payload.end());
        client->last_login_error = text;
        json = json_text(text);
        break;
    }
    case 16: {
        kind = "disconnect_message";
        std::string text(payload.begin(), payload.end());
        client->last_login_error = text;
        json = json_text(text);
        break;
    }
    case 5: kind = "donate_url"; json = json_text(std::string(payload.begin(), payload.end())); break;
    case 6: kind = "call_us"; json = json_text(std::string(payload.begin(), payload.end())); break;
    default:
        return false;
    }
    CallbackListServer cb;
    {
        std::lock_guard<std::mutex> lock(client->cb_mutex);
        cb = client->on_list_event;
    }
    std::string wrapped = "{\"kind\":" + json_text(kind) + ",\"data\":" + json + "}";
    if (cb.cb) cb.cb(kind.c_str(), wrapped.c_str(), cb.ud);
    return true;
}

#endif

extern "C" {

#define EXT_CB(name, field, type) \
GCLIB_API void name(GCHandle handle, type callback, void* user_data) { \
    auto* gc = as_client(handle); \
    if (!gc) return; \
    std::lock_guard<std::mutex> lock(gc->cb_mutex); \
    gc->ext.field.cb = callback; \
    gc->ext.field.ud = user_data; \
}

EXT_CB(gc_on_horse, horse, GC_OnHorse)
EXT_CB(gc_on_baddy_props, baddy_props, GC_OnBaddyProps)
EXT_CB(gc_on_baddy_hurt, baddy_hurt, GC_OnBaddyHurt)
EXT_CB(gc_on_item, item, GC_OnItem)
EXT_CB(gc_on_bomb, bomb, GC_OnBomb)
EXT_CB(gc_on_arrow, arrow, GC_OnArrow)
EXT_CB(gc_on_chest, chest, GC_OnChest)
EXT_CB(gc_on_level_link, level_link, GC_OnLevelLink)
EXT_CB(gc_on_trigger_action, trigger_action, GC_OnTriggerAction)
EXT_CB(gc_on_say2, say2, GC_OnSay2)
EXT_CB(gc_on_freeze, freeze, GC_OnFreeze)
EXT_CB(gc_on_active_level, active_level, GC_OnActiveLevel)
EXT_CB(gc_on_server_warp, server_warp, GC_OnServerWarp)
EXT_CB(gc_on_net_cookie, net_cookie, GC_OnNetCookie)
EXT_CB(gc_on_server_options, server_options, GC_OnServerOptions)
EXT_CB(gc_on_process_query, process_query, GC_OnProcessQuery)
EXT_CB(gc_on_profile, profile, GC_OnProfile)
EXT_CB(gc_on_remote_event, remote_event, GC_OnRemoteEvent)
EXT_CB(gc_on_transfer, transfer, GC_OnTransfer)

GCLIB_API void gc_on_connection_state(GCHandle handle, GC_OnConnectionState callback, void* user_data) {
    auto* gc = as_client(handle);
    if (!gc) return;
    std::lock_guard<std::mutex> lock(gc->cb_mutex);
    gc->on_connection_state.cb = callback;
    gc->on_connection_state.ud = user_data;
}

GCLIB_API void gc_set_cipher_seed(GCHandle handle, unsigned int seed) {
    auto* gc = as_client(handle);
    if (!gc) return;
    std::lock_guard<std::mutex> lock(gc->send_mutex);
    gc->cipher_seed = seed;
    gc->in_iter = seed;
    gc->out_iter = seed;
}

GCLIB_API void gc_set_auto_process_replies(GCHandle handle, int enabled) {
    auto* gc = as_client(handle);
    if (gc) gc->auto_process_replies = enabled != 0;
}

GCLIB_API void gc_set_reconnect(GCHandle handle, int max_attempts, int delay_ms) {
    auto* gc = as_client(handle);
    if (!gc) return;
    gc->reconnect_max_attempts = std::max(0, max_attempts);
    gc->reconnect_delay_ms = std::max(50, delay_ms);
}

GCLIB_API void gc_set_keepalive(GCHandle handle, int interval_ms) {
    auto* gc = as_client(handle);
    if (!gc) return;
    gc->keepalive_interval_ms = std::max(0, interval_ms);
    if (gc->connected) tclient_start_keepalive(gc);
}

GCLIB_API const char* gc_get_last_login_error(GCHandle handle) {
    auto* gc = as_client(handle);
    if (!gc) return "";
    return gc->last_login_error.c_str();
}

GCLIB_API char* gc_get_state_json(GCHandle handle, const char* kind) {
    auto* gc = as_client(handle);
    if (!gc || !kind) return copy_for_abi("{}");
    std::string k = kind;
    if (k == "players") {
        std::vector<int> ids;
        {
            std::lock_guard<std::mutex> lock(gc->players_mutex);
            for (const auto& kv : gc->players) ids.push_back(kv.first);
        }
        std::sort(ids.begin(), ids.end());
        std::string out = "[";
        for (size_t n = 0; n < ids.size(); ++n) {
            char* one = gc_get_player_json(handle, ids[n]);
            if (n) out += ",";
            out += one;
            gc_free_string(one);
        }
        return copy_for_abi(out + "]");
    }
    return copy_for_abi(tclient_world_json(gc, k));
}

GCLIB_API int gc_send_shoot(GCHandle handle, int source, int x, int y, int z, int angle, int z_angle, int speed, const char* gani, const char* parameters) {
    std::vector<uint8_t> data;
    write_gint3(data, source);
    put_signed(data, x);
    put_signed(data, y);
    put_signed(data, z);
    write_gchar(data, angle);
    write_gchar(data, z_angle);
    write_gchar(data, speed);
    write_gstring(data, gani ? gani : "");
    std::string params = parameters ? parameters : "";
    write_gchar(data, static_cast<int>(params.size()));
    data.insert(data.end(), params.begin(), params.end());
    return gc_send_packet(handle, PLI_SHOOT, data.data(), static_cast<int>(data.size()));
}

GCLIB_API int gc_send_shoot2(GCHandle handle, int pixel_x, int pixel_y, int pixel_z, int offset_x, int offset_y, int angle, int z_angle, int power, int gravity, const char* gani, const char* parameters) {
    std::vector<uint8_t> data;
    write_gshort(data, pixel_x);
    write_gshort(data, pixel_y);
    write_gshort(data, pixel_z);
    put_signed(data, offset_x);
    put_signed(data, offset_y);
    write_gchar(data, angle);
    write_gchar(data, z_angle);
    write_gchar(data, power);
    write_gchar(data, gravity);
    std::string g = gani ? gani : "";
    write_gshort(data, static_cast<int>(g.size()));
    data.insert(data.end(), g.begin(), g.end());
    std::string params = parameters ? parameters : "";
    write_gchar(data, static_cast<int>(params.size()));
    data.insert(data.end(), params.begin(), params.end());
    return gc_send_packet(handle, PLI_SHOOT2, data.data(), static_cast<int>(data.size()));
}

GCLIB_API int gc_send_weapon_add(GCHandle handle, int weapon_type, int default_weapon_id, unsigned int npc_id) {
    std::vector<uint8_t> data;
    write_gchar(data, weapon_type);
    if (weapon_type == 0) {
        put_signed(data, default_weapon_id);
    } else {
        for (int shift = 21; shift >= 0; shift -= 7) data.push_back(static_cast<uint8_t>(((npc_id >> shift) & 0x7f) + 32));
    }
    return gc_send_packet(handle, PLI_WEAPONADD, data.data(), static_cast<int>(data.size()));
}

GCLIB_API int gc_request_base_files(GCHandle handle, const char* const* files, int count) {
    // The client asks for at most the first ten wanted files at login.
    int sent = 0;
    for (int n = 0; n < count && n < 10 && files; ++n) {
        if (files[n] && *files[n] && gc_request_file(handle, files[n])) ++sent;
    }
    return sent;
}

GCLIB_API int gc_send_language(GCHandle handle, const char* language) {
    std::string s = language ? language : "";
    return gc_send_packet(handle, PLI_LANGUAGE, s.data(), static_cast<int>(s.size()));
}

GCLIB_API int gc_send_tamper_check(GCHandle handle, long long hash1, long long hash2) {
    std::vector<uint8_t> data;
    put_g5(data, hash1);
    put_g5(data, hash2);
    return gc_send_packet(handle, PLI_MAPINFO, data.data(), static_cast<int>(data.size()));
}

GCLIB_API int gc_send_enter_level_state(GCHandle handle, int state) {
    std::vector<uint8_t> data;
    write_gchar(data, state);
    return gc_send_packet(handle, 46, data.data(), static_cast<int>(data.size()));
}

GCLIB_API int gc_send_baddy_props(GCHandle handle, int baddy_id, const void* props, int length) {
    std::vector<uint8_t> data;
    write_gchar(data, baddy_id);
    if (props && length > 0) {
        const uint8_t* p = static_cast<const uint8_t*>(props);
        data.insert(data.end(), p, p + length);
    }
    return gc_send_packet(handle, PLI_BADDYPROPS, data.data(), static_cast<int>(data.size()));
}

GCLIB_API int gc_send_baddy_hurt(GCHandle handle, int baddy_id, int hurt_x, int hurt_y, int damage) {
    std::vector<uint8_t> data;
    write_gchar(data, baddy_id);
    put_signed(data, hurt_x);
    put_signed(data, hurt_y);
    write_gchar(data, damage);
    return gc_send_packet(handle, PLI_BADDYHURT, data.data(), static_cast<int>(data.size()));
}

GCLIB_API int gc_send_baddy_add(GCHandle handle, float x, float y, int type, int power, const char* image) {
    std::vector<uint8_t> data;
    put_half(data, x);
    put_half(data, y);
    write_gchar(data, type);
    write_gchar(data, power);
    put_text(data, image);
    return gc_send_packet(handle, PLI_BADDYADD, data.data(), static_cast<int>(data.size()));
}

GCLIB_API int gc_send_horse_add(GCHandle handle, float x, float y, int direction, int bushes, const char* image) {
    std::vector<uint8_t> data;
    put_half(data, x);
    put_half(data, y);
    write_gchar(data, (direction & 3) | ((bushes & 0x1f) << 2));
    put_text(data, image);
    return gc_send_packet(handle, PLI_HORSEADD, data.data(), static_cast<int>(data.size()));
}

GCLIB_API int gc_send_horse_del(GCHandle handle, float x, float y) {
    std::vector<uint8_t> data;
    put_half(data, x);
    put_half(data, y);
    return gc_send_packet(handle, PLI_HORSEDEL, data.data(), static_cast<int>(data.size()));
}

GCLIB_API int gc_send_arrow_add(GCHandle handle, float x, float y, int flags, int sprite, int power) {
    std::vector<uint8_t> data;
    put_half(data, x);
    put_half(data, y);
    write_gchar(data, flags);
    write_gchar(data, sprite);
    put_signed(data, power);
    return gc_send_packet(handle, PLI_ARROWADD, data.data(), static_cast<int>(data.size()));
}

GCLIB_API int gc_send_npc_server_query(GCHandle handle, int query_id, const char* message) {
    std::vector<uint8_t> data;
    write_gshort(data, query_id);
    put_text(data, message);
    return gc_send_packet(handle, PLI_NPCSERVERQUERY, data.data(), static_cast<int>(data.size()));
}

GCLIB_API int gc_send_raw_data_header(GCHandle handle, int length) {
    std::vector<uint8_t> data;
    write_gint3(data, length);
    return gc_send_packet(handle, PLI_RAWDATA, data.data(), static_cast<int>(data.size()));
}

GCLIB_API int gc_send_profile_get(GCHandle handle, const char* account) {
    std::string s = account ? account : "";
    return gc_send_packet(handle, PLI_PROFILEGET, s.data(), static_cast<int>(s.size()));
}

GCLIB_API int gc_send_profile_set(GCHandle handle, const char* account, const char* name, const char* age, const char* sex, const char* country, const char* messenger, const char* email, const char* url, const char* hangout, const char* quote) {
    std::vector<uint8_t> data;
    for (const char* field : {account, name, age, sex, country, messenger, email, url, hangout, quote}) {
        write_gstring(data, field ? field : "");
    }
    return gc_send_packet(handle, PLI_PROFILESET, data.data(), static_cast<int>(data.size()));
}

GCLIB_API int gc_send_process_running(GCHandle handle, int running) {
    std::vector<uint8_t> data;
    write_gchar(data, running ? 1 : 0);
    return gc_send_packet(handle, 45, data.data(), static_cast<int>(data.size()));
}

GCLIB_API int gc_send_process_list(GCHandle handle, const char* comma_separated_names) {
    std::string s = comma_separated_names ? comma_separated_names : "";
    return gc_send_packet(handle, PLI_PROCESSLIST, s.data(), static_cast<int>(s.size()));
}

GCLIB_API int gc_send_mute_player(GCHandle handle, int player_id) {
    std::vector<uint8_t> data;
    write_gshort(data, player_id);
    write_gchar(data, 1);
    return gc_send_packet(handle, PLI_MUTEPLAYER, data.data(), static_cast<int>(data.size()));
}

GCLIB_API int gc_send_request_update_board(GCHandle handle, const char* level, long long mod_time, int x, int y, int width, int height) {
    std::vector<uint8_t> data;
    write_gstring(data, level ? level : "");
    put_g5(data, mod_time);
    write_gshort(data, x);
    write_gshort(data, y);
    write_gshort(data, width);
    write_gshort(data, height);
    return gc_send_packet(handle, PLI_REQUESTUPDATEBOARD, data.data(), static_cast<int>(data.size()));
}

GCLIB_API void gc_set_send_capture(GCHandle handle, GC_OnSendCapture callback, void* user_data) {
    auto* gc = as_client(handle);
    if (!gc) return;
    std::lock_guard<std::mutex> lock(gc->cb_mutex);
    gc->send_capture.cb = callback;
    gc->send_capture.ud = user_data;
}

GCLIB_API int gc_inject_packet(GCHandle handle, int packet_id, const void* data, int length) {
    auto* gc = as_client(handle);
    if (!gc) return 0;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> payload;
    if (bytes && length > 0) payload.assign(bytes, bytes + length);
    tclient_dispatch_packet(gc, packet_id, payload);
    return 1;
}

GCLIB_API char* gc_decode_packet_json(int packet_id, const void* data, int length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> payload;
    if (bytes && length > 0) payload.assign(bytes, bytes + length);
    std::string json;
    if (!tclient_decode_json(packet_id, payload, json)) return copy_for_abi("null");
    return copy_for_abi(json);
}

GCLIB_API int gc_rc_send(GCHandle handle, const char* command, const char* const* args, int arg_count, const void* blob, int blob_length) {
    return send_command(handle, kRemoteCommands, sizeof(kRemoteCommands) / sizeof(kRemoteCommands[0]), command, args, arg_count, blob, blob_length);
}

GCLIB_API int gc_nc_send(GCHandle handle, const char* command, const char* const* args, int arg_count, const void* blob, int blob_length) {
    return send_command(handle, kNpcCommands, sizeof(kNpcCommands) / sizeof(kNpcCommands[0]), command, args, arg_count, blob, blob_length);
}

#ifdef GCLIB_LISTSERVER
GCLIB_API GCHandle gc_list_create(const char* host, int port, const char* version) {
    // Only the 2.x protocol logs in through a list server; 6.x clients connect straight to a login or game server.
    std::string wanted = version ? version : "2.22";
    if (wanted != "2.22" && wanted != "222") return nullptr;
    GCHandle handle = gc_create(host, port, wanted.c_str());
    auto* gc = as_client(handle);
    if (gc) gc->list_mode = true;
    return handle;
}

GCLIB_API void gc_list_destroy(GCHandle handle) {
    gc_destroy(handle);
}

GCLIB_API void gc_list_on_event(GCHandle handle, GC_OnListServer callback, void* user_data) {
    auto* gc = as_client(handle);
    if (!gc) return;
    std::lock_guard<std::mutex> lock(gc->cb_mutex);
    gc->on_list_event.cb = callback;
    gc->on_list_event.ud = user_data;
}

GCLIB_API int gc_list_connect(GCHandle handle) {
    auto* gc = as_client(handle);
    if (!gc || !gc->list_mode) return 0;
    if (!gc_connect(handle)) return 0;
    // The list server reads its first packet as a plain zlib frame; its codec switches to the session key afterwards.
    std::vector<uint8_t> raw;
    raw.push_back(static_cast<uint8_t>(7 + 32));
    raw.push_back(static_cast<uint8_t>(gc->enc_key + 32));
    put_text(raw, gc->version.protocol);
    put_text(raw, "newmain");
    raw.push_back(10);
    mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::vector<uint8_t> z(bound);
    if (mz_compress(z.data(), &bound, raw.data(), static_cast<mz_ulong>(raw.size())) != MZ_OK) return 0;
    z.resize(bound);
    std::vector<uint8_t> frame = {static_cast<uint8_t>(z.size() >> 8), static_cast<uint8_t>(z.size() & 0xff)};
    frame.insert(frame.end(), z.begin(), z.end());
    std::lock_guard<std::mutex> lock(gc->send_mutex);
    size_t sent = 0;
    while (sent < frame.size()) {
        int n = send(gc->sock, reinterpret_cast<const char*>(frame.data()) + sent, static_cast<int>(frame.size() - sent), 0);
        if (n <= 0) return 0;
        sent += static_cast<size_t>(n);
    }
    gc->first_packet_out = false;
    return 1;
}

GCLIB_API int gc_list_request_servers(GCHandle handle, const char* account, const char* password) {
    auto* gc = as_client(handle);
    if (!gc || !gc->list_mode) return 0;
    gc->list_servers_json.clear();
    gc->last_login_error.clear();
    std::vector<uint8_t> data;
    write_gstring(data, account ? account : "");
    write_gstring(data, password ? password : "");
    return gc_send_packet(handle, 1, data.data(), static_cast<int>(data.size()));
}

GCLIB_API char* gc_list_get_servers_json(GCHandle handle) {
    auto* gc = as_client(handle);
    if (!gc) return copy_for_abi("[]");
    std::string out = "[";
    for (size_t n = 0; n < gc->list_servers_json.size(); ++n) {
        if (n) out += ",";
        out += gc->list_servers_json[n];
    }
    return copy_for_abi(out + "]");
}

#endif

} // extern "C"
