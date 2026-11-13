#include "TClientPacketDispatch.h"
#include "TClientProtocolIO.h"
#include "IEnums.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string jesc(const std::string& value) {
    std::string out;
    out.reserve(value.size());
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
    return out;
}

std::string hex_of(const uint8_t* data, size_t count) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0f]);
    }
    return out;
}

class Js {
public:
    Js& i(const char* name, long long value) { key(name); s_ += std::to_string(value); return *this; }
    Js& d(const char* name, double value) {
        key(name);
        std::ostringstream o;
        o << value;
        s_ += o.str();
        return *this;
    }
    Js& t(const char* name, const std::string& value) { key(name); s_ += "\"" + jesc(value) + "\""; return *this; }
    Js& b(const char* name, bool value) { key(name); s_ += value ? "true" : "false"; return *this; }
    Js& raw(const char* name, const std::string& json) { key(name); s_ += json; return *this; }
    Js& list(const char* name, const std::vector<std::string>& values) {
        key(name);
        s_ += "[";
        for (size_t n = 0; n < values.size(); ++n) {
            if (n) s_ += ",";
            s_ += "\"" + jesc(values[n]) + "\"";
        }
        s_ += "]";
        return *this;
    }
    Js& ints(const char* name, const std::vector<int>& values) {
        key(name);
        s_ += "[";
        for (size_t n = 0; n < values.size(); ++n) {
            if (n) s_ += ",";
            s_ += std::to_string(values[n]);
        }
        s_ += "]";
        return *this;
    }
    Js& hex(const char* name, const std::vector<uint8_t>& bytes) { return t(name, hex_of(bytes.data(), bytes.size())); }
    std::string str() const { return s_ + "}"; }

private:
    void key(const char* name) {
        if (!first_) s_ += ",";
        first_ = false;
        s_ += "\"";
        s_ += name;
        s_ += "\":";
    }
    std::string s_ = "{";
    bool first_ = true;
};

struct Rd {
    explicit Rd(const std::vector<uint8_t>& bytes) : r(bytes) {}
    TPacketReader r;
    bool has(size_t n = 1) const { return r.has(n); }
    int gc() { return r.has() ? r.gchar() : 0; }
    int sb() { return r.has() ? static_cast<int8_t>(static_cast<uint8_t>(r.byte() - 32)) : 0; }
    int gs() { return r.has(2) ? r.gshort() : 0; }
    int g3() { return r.has(3) ? r.gint3() : 0; }
    long long g4() {
        if (!r.has(4)) return 0;
        long long v = 0;
        for (int n = 0; n < 4; ++n) v = (v << 7) | (static_cast<int>(r.byte()) - 32);
        return v;
    }
    long long g5() {
        if (!r.has(5)) return 0;
        long long v = 0;
        for (int n = 0; n < 5; ++n) v = (v << 7) | (static_cast<int>(r.byte()) - 32);
        return v;
    }
    std::string gstr() { return r.has() ? r.gstring() : std::string(); }
    std::string gstr16() {
        int n = gs();
        return r.str(static_cast<size_t>(std::max(0, n)));
    }
    std::string rest() {
        std::vector<uint8_t> bytes = r.remaining();
        if (!bytes.empty() && bytes.back() == '\n') bytes.pop_back();
        return std::string(bytes.begin(), bytes.end());
    }
    std::vector<uint8_t> rest_bytes() { return r.remaining(); }
    std::vector<std::string> gstrings() {
        std::vector<std::string> out;
        while (r.has()) out.push_back(r.gstring());
        return out;
    }
};

std::string as_text(const std::vector<uint8_t>& payload) {
    std::string text(payload.begin(), payload.end());
    if (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

std::vector<std::string> csv_tokens(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    bool any = false;
    for (size_t n = 0; n < text.size(); ++n) {
        char ch = text[n];
        if (quoted) {
            if (ch == '"') {
                if (n + 1 < text.size() && text[n + 1] == '"') { cur += '"'; ++n; }
                else quoted = false;
            } else {
                cur += ch;
            }
        } else if (ch == '"') {
            quoted = true;
            any = true;
        } else if (ch == ',') {
            out.push_back(cur);
            cur.clear();
            any = false;
        } else {
            cur += ch;
            any = true;
        }
    }
    if (any || !cur.empty() || !text.empty()) out.push_back(cur);
    return out;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : text) {
        if (ch == '\n') { out.push_back(cur); cur.clear(); }
        else if (ch != '\r') cur += ch;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string pos_key(double x, double y) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g,%g", x, y);
    return buf;
}

} // namespace

void tclient_world_reset_level(TClient* client) {
    std::lock_guard<std::mutex> lock(client->state_mutex);
    client->world.horses.clear();
    client->world.baddies.clear();
    client->world.items.clear();
    client->world.bombs.clear();
    client->world.chests.clear();
    client->world.signs.clear();
    client->world.links.clear();
    client->world.npcs.clear();
}

void tclient_world_flag(TClient* client, const std::string& name, const std::string& value, bool set) {
    std::lock_guard<std::mutex> lock(client->state_mutex);
    if (set) client->world.flags[name] = value;
    else client->world.flags.erase(name);
}

void tclient_world_npc(TClient* client, int id, const std::string& props_json, bool remove) {
    std::lock_guard<std::mutex> lock(client->state_mutex);
    if (remove) client->world.npcs.erase(id);
    else client->world.npcs[id] = props_json;
}

void tclient_world_weapon(TClient* client, const std::string& name, bool add) {
    std::lock_guard<std::mutex> lock(client->state_mutex);
    auto& list = client->world.weapons;
    auto it = std::find(list.begin(), list.end(), name);
    if (add) {
        if (it == list.end()) list.push_back(name);
    } else if (it != list.end()) {
        list.erase(it);
    }
}

void tclient_world_sign(TClient* client, double x, double y, const std::string& text) {
    std::lock_guard<std::mutex> lock(client->state_mutex);
    client->world.signs[pos_key(x, y)] = text;
}

void tclient_world_clear_weapons(TClient* client) {
    std::lock_guard<std::mutex> lock(client->state_mutex);
    client->world.weapons.clear();
}

namespace {

void remote_kind_event(TClient* client, const TClientExtCallbacks& ext, int packet_id, const char* kind, const std::string& json) {
    tclient_emit_event(client, packet_id, json);
    if (ext.remote_event.cb) ext.remote_event.cb(kind, json.c_str(), ext.remote_event.ud);
}

std::string file_browser_dir_json(Rd& rd) {
    std::string dir = rd.gstr();
    std::string out = "[";
    bool first = true;
    while (rd.has(2)) {
        rd.r.byte();
        int len = rd.gc();
        if (!rd.has(static_cast<size_t>(len))) break;
        std::vector<uint8_t> sub(rd.r.data + rd.r.pos, rd.r.data + rd.r.pos + len);
        rd.r.pos += static_cast<size_t>(len);
        Rd in(sub);
        std::string name = in.gstr();
        std::string rights = in.gstr();
        long long size = in.g5();
        long long modified = in.g5();
        if (!first) out += ",";
        first = false;
        out += Js().t("name", name).t("rights", rights).i("size", size).i("modified", modified).str();
    }
    out += "]";
    return Js().t("directory", dir).raw("files", out).str();
}

std::string player_property_data_json(Rd& rd) {
    Js j;
    j.t("world", rd.gstr());
    int plen = rd.gc();
    std::vector<uint8_t> props;
    for (int n = 0; n < plen && rd.has(); ++n) props.push_back(rd.r.byte());
    j.hex("properties_hex", props);
    int flag_count = rd.gs();
    std::vector<std::string> flags;
    for (int n = 0; n < flag_count && rd.has(); ++n) {
        int len = rd.gc();
        flags.push_back(rd.r.str(static_cast<size_t>(len)));
    }
    j.list("flags", flags);
    int chest_count = rd.gs();
    std::string chests = "[";
    for (int n = 0; n < chest_count && rd.has(); ++n) {
        int len = rd.gc();
        std::vector<uint8_t> sub(rd.r.data + rd.r.pos, rd.r.data + std::min(rd.r.len, rd.r.pos + static_cast<size_t>(len)));
        rd.r.pos = std::min(rd.r.len, rd.r.pos + static_cast<size_t>(len));
        Rd in(sub);
        int cx = in.gc();
        int cy = in.gc();
        std::string level = in.rest();
        if (n) chests += ",";
        chests += Js().i("x", cx).i("y", cy).t("level", level).str();
    }
    chests += "]";
    j.raw("chests", chests);
    int weapon_count = rd.gc();
    std::vector<std::string> weapons;
    for (int n = 0; n < weapon_count && rd.has(); ++n) {
        int len = rd.gc();
        weapons.push_back(rd.r.str(static_cast<size_t>(len)));
    }
    j.list("weapons", weapons);
    return j.str();
}

} // namespace

bool tclient_decode_json(int packet_id, const std::vector<uint8_t>& payload, std::string& out) {
    Rd rd(payload);
    switch (packet_id) {
    case PLO_RAWDATA:
        out = Js().i("length", rd.g3()).str();
        return true;
    case PLO_UNKNOWN111:
        out = Js().t("cookie", as_text(payload)).str();
        return true;
    case PLO_UNKNOWN185: {
        int map_x = rd.gc(), map_y = rd.gc(), bx = rd.gc(), by = rd.gc(), bw = rd.gc(), bh = rd.gc();
        std::vector<int> heights;
        while (rd.has()) heights.push_back(rd.gc());
        out = Js().i("map_x", map_x).i("map_y", map_y).i("block_x", bx).i("block_y", by)
                  .i("block_width", bw).i("block_height", bh).ints("heights", heights).str();
        return true;
    }
    case PLO_UNKNOWN186: {
        int map_x = rd.gc(), map_y = rd.gc(), tx = rd.gc(), ty = rd.gc(), w = rd.gc(), h = rd.gc();
        std::vector<int> tiles;
        while (rd.has(2)) {
            int lo = rd.r.byte();
            int hi = rd.r.byte();
            tiles.push_back((lo | (hi << 8)) & 0x0fff);
        }
        out = Js().i("map_x", map_x).i("map_y", map_y).i("tile_x", tx).i("tile_y", ty)
                  .i("width", w).i("height", h).ints("tiles", tiles).str();
        return true;
    }
    case PLO_UNKNOWN183:
        out = Js().t("processes", as_text(payload)).list("names", csv_tokens(as_text(payload))).str();
        return true;
    case PLO_UNKNOWN184:
        out = Js().t("filename", as_text(payload)).str();
        return true;
    case PLO_UNKNOWN195: {
        std::string name = rd.gstr();
        out = Js().t("gani", name).t("command", rd.rest()).str();
        return true;
    }
    case PLO_UNKNOWN198:
        out = Js().t("options", as_text(payload)).list("lines", split_lines(as_text(payload))).str();
        return true;
    case PLO_UNKNOWN166: {
        long long npc = rd.g3();
        int image = rd.gc();
        out = Js().i("npc_id", npc).i("image_id", image).hex("properties_hex", rd.rest_bytes()).str();
        return true;
    }
    case PLO_UNKNOWN190:
        out = "{\"connected\":true}";
        return true;
    case PLO_HORSEADD: {
        double x = rd.gc() / 2.0, y = rd.gc() / 2.0;
        int dir_bushes = rd.gc();
        out = Js().d("x", x).d("y", y).i("direction", dir_bushes & 3).i("bushes", dir_bushes >> 2).t("image", rd.rest()).str();
        return true;
    }
    case PLO_HORSEDEL: {
        double x = rd.gc() / 2.0, y = rd.gc() / 2.0;
        out = Js().d("x", x).d("y", y).str();
        return true;
    }
    case PLO_NPCACTION:
        out = Js().t("category", "npc-action").hex("properties_hex", payload).t("text", as_text(payload)).str();
        return true;
    case PLO_BADDYPROPS: {
        int id = rd.gc();
        out = Js().i("baddy_id", id).hex("properties_hex", rd.rest_bytes()).str();
        return true;
    }
    case PLO_BADDYHURT: {
        int id = rd.gc(), hx = rd.sb(), hy = rd.sb(), dmg = rd.gc();
        out = Js().i("baddy_id", id).i("hurt_x", hx).i("hurt_y", hy).i("damage", dmg).str();
        return true;
    }
    case PLO_SHOOT: {
        int player = rd.gs();
        long long source = rd.g3();
        int x = rd.sb(), y = rd.sb(), z = rd.sb();
        int angle = rd.gc(), z_angle = rd.gc(), power = rd.gc();
        std::string gani = rd.gstr();
        int plen = rd.gc();
        out = Js().i("player_id", player).i("source", source).i("x", x).i("y", y).i("z", z)
                  .i("angle", angle).i("z_angle", z_angle).i("power", power).t("gani", gani)
                  .i("parameter_length", plen).t("parameters", rd.rest()).str();
        return true;
    }
    case PLO_SHOOT2: {
        int player = rd.gs();
        int px = rd.gs(), py = rd.gs(), pz = rd.gs();
        int ox = rd.sb(), oy = rd.sb();
        int angle = rd.gc(), z_angle = rd.gc(), power = rd.gc(), gravity = rd.gc();
        std::string gani = rd.gstr16();
        int plen = rd.gc();
        out = Js().i("player_id", player).i("pixel_x", px).i("pixel_y", py).i("pixel_z", pz)
                  .i("offset_x", ox).i("offset_y", oy).i("angle", angle).i("z_angle", z_angle)
                  .i("power", power).i("gravity", gravity).t("gani", gani)
                  .i("parameter_length", plen).t("parameters", rd.rest()).str();
        return true;
    }
    case PLO_LISTPROCESSES:
        out = Js().list("processes", csv_tokens(as_text(payload))).str();
        return true;
    case PLO_UPDATEPACKAGEISUPDATED:
        out = Js().t("filename", as_text(payload)).str();
        return true;
    case PLO_SAY2:
        out = Js().t("text", as_text(payload)).str();
        return true;
    case PLO_RPGWINDOW:
        out = Js().list("messages", csv_tokens(as_text(payload))).str();
        return true;
    case PLO_STATUSLIST:
        out = Js().list("statuses", csv_tokens(as_text(payload))).str();
        return true;
    case PLO_PROFILE: {
        int prop = rd.gc();
        std::string account = rd.gstr();
        out = Js().i("property_id", prop).t("account", account).list("fields", rd.gstrings()).str();
        return true;
    }
    case PLO_RC_CHAT:
        out = Js().t("message", as_text(payload)).str();
        return true;
    case PLO_RC_ADMINMESSAGE:
        out = Js().t("message", as_text(payload)).str();
        return true;
    case PLO_RC_FILEBROWSER_DIRLIST:
        out = Js().list("directories", csv_tokens(as_text(payload))).str();
        return true;
    case PLO_RC_FILEBROWSER_DIR:
        out = file_browser_dir_json(rd);
        return true;
    case PLO_RC_FILEBROWSER_MESSAGE:
        out = Js().t("message", as_text(payload)).str();
        return true;
    case PLO_RC_ACCOUNTLISTGET:
        out = Js().list("accounts", rd.gstrings()).str();
        return true;
    case PLO_RC_PLAYERPROPSGET: {
        int player = rd.gs();
        std::string account = rd.gstr();
        out = Js().i("player_id", player).t("account", account).raw("data", player_property_data_json(rd)).str();
        return true;
    }
    case PLO_RC_ACCOUNTGET: {
        std::string account = rd.gstr(), password = rd.gstr(), email = rd.gstr();
        bool banned = rd.gc() == 1;
        bool load_only = rd.gc() == 1;
        int level = rd.gc();
        std::string worlds = rd.gstr(), ban_time = rd.gstr(), ban_reason = rd.gstr();
        out = Js().t("account", account).t("password", password).t("email", email).b("banned", banned)
                  .b("load_only", load_only).i("admin_level", level).t("admin_worlds", worlds)
                  .t("ban_time", ban_time).t("ban_reason", ban_reason).str();
        return true;
    }
    case PLO_RC_SERVERFLAGSGET: {
        int count = rd.gs();
        std::vector<std::string> flags;
        for (int n = 0; n < count && rd.has(); ++n) flags.push_back(rd.gstr());
        out = Js().list("flags", flags).str();
        return true;
    }
    case PLO_RC_PLAYERRIGHTSGET: {
        std::string account = rd.gstr();
        long long rights = rd.g5();
        std::string ip = rd.gstr();
        std::string folders = rd.gstr16();
        out = Js().t("account", account).i("rights", rights).t("admin_ip", ip).t("folders", folders).str();
        return true;
    }
    case PLO_RC_PLAYERCOMMENTSGET: {
        std::string account = rd.gstr();
        out = Js().t("account", account).t("comments", rd.rest()).str();
        return true;
    }
    case PLO_RC_PLAYERBANGET: {
        std::string account = rd.gstr();
        bool banned = rd.gc() == 1;
        out = Js().t("account", account).b("banned", banned).t("reason", rd.rest()).str();
        return true;
    }
    case PLO_RC_SERVEROPTIONSGET:
    case PLO_RC_FOLDERCONFIGGET:
        out = Js().t("text", as_text(payload)).list("lines", split_lines(as_text(payload))).str();
        return true;
    case PLO_RC_MAXUPLOADFILESIZE:
        out = Js().i("max_upload_file_size", rd.g5()).str();
        return true;
    case PLO_NC_CONTROL:
        out = Js().hex("properties_hex", payload).t("text", as_text(payload)).str();
        return true;
    case PLO_NC_LEVELLIST:
        out = Js().list("levels", rd.gstrings()).str();
        return true;
    case PLO_LARGEFILESTART:
    case PLO_LARGEFILEEND:
        out = Js().t("filename", as_text(payload)).str();
        return true;
    case PLO_LARGEFILESIZE:
        out = Js().i("size", rd.g5()).str();
        return true;
    case PLO_UPDATEPACKAGESIZE: {
        std::string name = rd.gstr();
        out = Js().t("package", name).i("size", rd.g5()).str();
        return true;
    }
    case PLO_UPDATEPACKAGEDONE:
        out = Js().t("package", as_text(payload)).str();
        return true;
    case PLO_UNKNOWN197: {
        std::string text = as_text(payload);
        size_t cut = 0;
        int commas = 0;
        while (cut < text.size() && commas < 3) {
            if (text[cut] == ',') ++commas;
            ++cut;
        }
        std::string header = text.substr(0, commas == 3 ? cut : text.size());
        std::vector<std::string> parts = csv_tokens(header);
        Js j;
        j.t("type", parts.size() > 0 ? parts[0] : "").t("name", parts.size() > 1 ? parts[1] : "").t("modified", parts.size() > 2 ? parts[2] : "");
        j.t("checksum_hex", hex_of(payload.data() + std::min(payload.size(), cut), payload.size() - std::min(payload.size(), cut)));
        out = j.str();
        return true;
    }
    case PLO_SERVERWARP: {
        std::vector<std::string> p = csv_tokens(as_text(payload));
        out = Js().t("identifier", p.size() > 0 ? p[0] : "").t("name", p.size() > 1 ? p[1] : "")
                  .t("host", p.size() > 2 ? p[2] : "").t("port", p.size() > 3 ? p[3] : "")
                  .t("server", as_text(payload)).str();
        return true;
    }
    default:
        return false;
    }
}

bool tclient_dispatch_extended(TClient* client, int packet_id, const std::vector<uint8_t>& payload) {
    std::string json;
    switch (packet_id) {
    case PLO_UNKNOWN111: case PLO_UNKNOWN185: case PLO_UNKNOWN186: case PLO_UNKNOWN183: case PLO_UNKNOWN184:
    case PLO_UNKNOWN195: case PLO_UNKNOWN198: case PLO_UNKNOWN166: case PLO_UNKNOWN190:
    case PLO_HORSEADD: case PLO_HORSEDEL: case PLO_NPCACTION: case PLO_BADDYPROPS: case PLO_BADDYHURT:
    case PLO_SHOOT: case PLO_SHOOT2: case PLO_LISTPROCESSES: case PLO_UPDATEPACKAGEISUPDATED:
    case PLO_SAY2: case PLO_RPGWINDOW: case PLO_STATUSLIST: case PLO_PROFILE:
    case PLO_RC_CHAT: case PLO_RC_ADMINMESSAGE: case PLO_RC_FILEBROWSER_DIRLIST: case PLO_RC_FILEBROWSER_DIR:
    case PLO_RC_FILEBROWSER_MESSAGE: case PLO_RC_ACCOUNTLISTGET: case PLO_RC_PLAYERPROPSGET: case PLO_RC_ACCOUNTGET:
    case PLO_RC_SERVERFLAGSGET: case PLO_RC_PLAYERRIGHTSGET: case PLO_RC_PLAYERCOMMENTSGET: case PLO_RC_PLAYERBANGET:
    case PLO_RC_SERVEROPTIONSGET: case PLO_RC_FOLDERCONFIGGET: case PLO_RC_MAXUPLOADFILESIZE:
    case PLO_NC_CONTROL: case PLO_NC_LEVELLIST: case PLO_LARGEFILESTART: case PLO_LARGEFILEEND: case PLO_LARGEFILESIZE:
    case PLO_UPDATEPACKAGESIZE: case PLO_UPDATEPACKAGEDONE: case PLO_UNKNOWN197: case PLO_SERVERWARP:
    case PLO_ITEMADD: case PLO_ITEMDEL: case PLO_BOMBADD: case PLO_BOMBDEL: case PLO_ARROWADD:
    case PLO_LEVELCHEST: case PLO_LEVELLINK: case PLO_TRIGGERACTION:
    case PLO_FREEZEPLAYER2: case PLO_UNFREEZEPLAYER: case PLO_SETACTIVELEVEL:
    case PLO_ISLEADER: case PLO_CLEARWEAPONS: case PLO_RAWDATA:
        break;
    default:
        return false;
    }

    TClientExtCallbacks ext;
    {
        std::lock_guard<std::mutex> lock(client->cb_mutex);
        ext = client->ext;
    }
    Rd rd(payload);

    switch (packet_id) {
    case PLO_ITEMADD:
    case PLO_ITEMDEL: {
        double x = rd.gc() / 2.0, y = rd.gc() / 2.0;
        int item = packet_id == PLO_ITEMADD ? rd.gc() : -1;
        Js j;
        j.d("x", x).d("y", y);
        if (packet_id == PLO_ITEMADD) j.i("item", item);
        {
            std::lock_guard<std::mutex> lock(client->state_mutex);
            if (packet_id == PLO_ITEMADD) client->world.items[pos_key(x, y)] = j.str();
            else client->world.items.erase(pos_key(x, y));
        }
        tclient_emit_event(client, packet_id, j.str());
        if (ext.item.cb) ext.item.cb(packet_id == PLO_ITEMADD, static_cast<float>(x), static_cast<float>(y), item, ext.item.ud);
        return true;
    }
    case PLO_BOMBADD: {
        int player = rd.gs();
        double x = rd.gc() / 2.0, y = rd.gc() / 2.0;
        int power_byte = rd.gc();
        int time = rd.gc();
        Js j;
        j.i("player_id", player).d("x", x).d("y", y).i("power", power_byte).i("time_to_explode", time);
        {
            std::lock_guard<std::mutex> lock(client->state_mutex);
            client->world.bombs[pos_key(x, y)] = j.str();
        }
        tclient_emit_event(client, packet_id, j.str());
        if (ext.bomb.cb) ext.bomb.cb(1, player, static_cast<float>(x), static_cast<float>(y), power_byte, time, ext.bomb.ud);
        return true;
    }
    case PLO_BOMBDEL: {
        double x = rd.gc() / 2.0, y = rd.gc() / 2.0;
        {
            std::lock_guard<std::mutex> lock(client->state_mutex);
            client->world.bombs.erase(pos_key(x, y));
        }
        tclient_emit_event(client, packet_id, Js().d("x", x).d("y", y).str());
        if (ext.bomb.cb) ext.bomb.cb(0, 0, static_cast<float>(x), static_cast<float>(y), 0, 0, ext.bomb.ud);
        return true;
    }
    case PLO_ARROWADD: {
        int player = rd.gs();
        double x = rd.gc() / 2.0, y = rd.gc() / 2.0;
        int flags = rd.gc(), sprite = rd.gc(), power = rd.sb();
        tclient_emit_event(client, packet_id, Js().i("player_id", player).d("x", x).d("y", y).i("flags", flags).i("sprite", sprite).i("power", power).str());
        if (ext.arrow.cb) ext.arrow.cb(player, static_cast<float>(x), static_cast<float>(y), flags, sprite, power, ext.arrow.ud);
        return true;
    }
    case PLO_LEVELCHEST: {
        int visible = rd.gc(), x = rd.gc(), y = rd.gc();
        int item = rd.has() ? rd.gc() : -1;
        int sign = rd.has() ? rd.gc() : -1;
        Js j;
        j.i("visible", visible).i("x", x).i("y", y).i("item_type", item).i("sign_index", sign);
        {
            std::lock_guard<std::mutex> lock(client->state_mutex);
            client->world.chests[pos_key(x, y)] = j.str();
        }
        tclient_emit_event(client, packet_id, j.str());
        if (ext.chest.cb) ext.chest.cb(visible, x, y, item, sign, ext.chest.ud);
        return true;
    }
    case PLO_LEVELLINK: {
        std::string text = as_text(payload);
        std::istringstream in(text);
        std::string dest, new_x, new_y;
        int x = 0, y = 0, w = 0, h = 0;
        in >> dest >> x >> y >> w >> h >> new_x >> new_y;
        Js j;
        j.t("destination", dest).i("x", x).i("y", y).i("width", w).i("height", h).t("new_x", new_x).t("new_y", new_y);
        {
            std::lock_guard<std::mutex> lock(client->state_mutex);
            client->world.links.push_back(j.str());
        }
        tclient_emit_event(client, packet_id, j.str());
        if (ext.level_link.cb) ext.level_link.cb(dest.c_str(), x, y, w, h, new_x.c_str(), new_y.c_str(), ext.level_link.ud);
        return true;
    }
    case PLO_TRIGGERACTION: {
        int player = rd.gs();
        int npc = rd.g3();
        double x = rd.gc() / 2.0, y = rd.gc() / 2.0;
        std::string text = rd.rest();
        std::string action = text, params;
        size_t comma = text.find(',');
        if (comma != std::string::npos) {
            action = text.substr(0, comma);
            params = text.substr(comma + 1);
        }
        tclient_emit_event(client, packet_id, Js().i("client_id", player).i("npc_id", npc).d("x", x).d("y", y).t("action", action).t("params", params).str());
        if (ext.trigger_action.cb) ext.trigger_action.cb(player, npc, static_cast<float>(x), static_cast<float>(y), action.c_str(), params.c_str(), ext.trigger_action.ud);
        return true;
    }
    case PLO_FREEZEPLAYER2:
    case PLO_UNFREEZEPLAYER: {
        bool frozen = packet_id == PLO_FREEZEPLAYER2;
        {
            std::lock_guard<std::mutex> lock(client->state_mutex);
            client->world.frozen = frozen;
        }
        tclient_emit_event(client, packet_id, Js().b("frozen", frozen).str());
        if (ext.freeze.cb) ext.freeze.cb(frozen ? 1 : 0, ext.freeze.ud);
        return true;
    }
    case PLO_SETACTIVELEVEL: {
        std::string level = as_text(payload);
        {
            std::lock_guard<std::mutex> lock(client->state_mutex);
            client->world.active_level = level;
        }
        tclient_emit_event(client, packet_id, Js().t("level", level).str());
        if (ext.active_level.cb) ext.active_level.cb(level.c_str(), ext.active_level.ud);
        return true;
    }
    case PLO_ISLEADER: {
        {
            std::lock_guard<std::mutex> lock(client->state_mutex);
            client->world.leader = true;
        }
        tclient_emit_event(client, packet_id, "{\"enabled\":true}");
        return true;
    }
    case PLO_CLEARWEAPONS: {
        tclient_world_clear_weapons(client);
        tclient_emit_event(client, packet_id, "{\"clear\":true}");
        return true;
    }
    default:
        break;
    }

    if (!tclient_decode_json(packet_id, payload, json)) return false;

    switch (packet_id) {
    case PLO_UNKNOWN111: {
        std::string cookie = as_text(payload);
        {
            std::lock_guard<std::mutex> lock(client->state_mutex);
            client->world.net_cookie = cookie;
        }
        tclient_emit_event(client, packet_id, json);
        if (ext.net_cookie.cb) ext.net_cookie.cb(cookie.c_str(), ext.net_cookie.ud);
        return true;
    }
    case PLO_UNKNOWN198: {
        std::string options = as_text(payload);
        {
            std::lock_guard<std::mutex> lock(client->state_mutex);
            client->world.server_options = options;
        }
        tclient_emit_event(client, packet_id, json);
        if (ext.server_options.cb) ext.server_options.cb(options.c_str(), ext.server_options.ud);
        return true;
    }
    case PLO_UNKNOWN183:
    case PLO_LISTPROCESSES:
    case PLO_UNKNOWN184: {
        const char* kind = packet_id == PLO_UNKNOWN183 ? "has_process_running" : (packet_id == PLO_LISTPROCESSES ? "list_processes" : "take_screenshot");
        tclient_emit_event(client, packet_id, json);
        std::string data = as_text(payload);
        if (ext.process_query.cb) ext.process_query.cb(kind, data.c_str(), ext.process_query.ud);
        if (client->auto_process_replies) {
            if (packet_id == PLO_UNKNOWN183) {
                uint8_t no = 32;
                gc_send_packet(client, 45, &no, 1);
            } else if (packet_id == PLO_LISTPROCESSES) {
                gc_send_packet(client, PLI_PROCESSLIST, nullptr, 0);
            }
        }
        return true;
    }
    case PLO_HORSEADD: {
        double x = rd.gc() / 2.0, y = rd.gc() / 2.0;
        int db = rd.gc();
        std::string image = rd.rest();
        {
            std::lock_guard<std::mutex> lock(client->state_mutex);
            client->world.horses[pos_key(x, y)] = json;
        }
        tclient_emit_event(client, packet_id, json);
        if (ext.horse.cb) ext.horse.cb(1, static_cast<float>(x), static_cast<float>(y), db & 3, db >> 2, image.c_str(), ext.horse.ud);
        return true;
    }
    case PLO_HORSEDEL: {
        double x = rd.gc() / 2.0, y = rd.gc() / 2.0;
        {
            std::lock_guard<std::mutex> lock(client->state_mutex);
            client->world.horses.erase(pos_key(x, y));
        }
        tclient_emit_event(client, packet_id, json);
        if (ext.horse.cb) ext.horse.cb(0, static_cast<float>(x), static_cast<float>(y), 0, 0, "", ext.horse.ud);
        return true;
    }
    case PLO_BADDYPROPS: {
        int id = rd.gc();
        {
            std::lock_guard<std::mutex> lock(client->state_mutex);
            client->world.baddies[id] = json;
        }
        tclient_emit_event(client, packet_id, json);
        if (ext.baddy_props.cb) ext.baddy_props.cb(id, json.c_str(), ext.baddy_props.ud);
        return true;
    }
    case PLO_BADDYHURT: {
        int id = rd.gc(), hx = rd.sb(), hy = rd.sb(), dmg = rd.gc();
        tclient_emit_event(client, packet_id, json);
        if (ext.baddy_hurt.cb) ext.baddy_hurt.cb(id, hx, hy, dmg, ext.baddy_hurt.ud);
        return true;
    }
    case PLO_SAY2: {
        std::string text = as_text(payload);
        tclient_emit_event(client, packet_id, json);
        if (ext.say2.cb) ext.say2.cb(text.c_str(), ext.say2.ud);
        return true;
    }
    case PLO_SERVERWARP: {
        std::vector<std::string> p = csv_tokens(as_text(payload));
        p.resize(std::max<size_t>(p.size(), 4));
        tclient_emit_event(client, packet_id, json);
        if (ext.server_warp.cb) ext.server_warp.cb(p[0].c_str(), p[1].c_str(), p[2].c_str(), p[3].c_str(), ext.server_warp.ud);
        return true;
    }
    case PLO_PROFILE: {
        int prop = rd.gc();
        (void)prop;
        std::string account = rd.gstr();
        std::string fields = Js().list("fields", rd.gstrings()).str();
        tclient_emit_event(client, packet_id, json);
        if (ext.profile.cb) ext.profile.cb(account.c_str(), fields.c_str(), ext.profile.ud);
        return true;
    }
    case PLO_RC_CHAT: case PLO_RC_ADMINMESSAGE: case PLO_RC_FILEBROWSER_DIRLIST: case PLO_RC_FILEBROWSER_DIR:
    case PLO_RC_FILEBROWSER_MESSAGE: case PLO_RC_ACCOUNTLISTGET: case PLO_RC_PLAYERPROPSGET: case PLO_RC_ACCOUNTGET:
    case PLO_RC_SERVERFLAGSGET: case PLO_RC_PLAYERRIGHTSGET: case PLO_RC_PLAYERCOMMENTSGET: case PLO_RC_PLAYERBANGET:
    case PLO_RC_SERVEROPTIONSGET: case PLO_RC_FOLDERCONFIGGET: case PLO_RC_MAXUPLOADFILESIZE: case PLO_NC_CONTROL:
    case PLO_NC_LEVELLIST: {
        const char* kind = "remote";
        switch (packet_id) {
        case PLO_RC_CHAT: kind = "rc_chat"; break;
        case PLO_RC_ADMINMESSAGE: kind = "admin_message"; break;
        case PLO_RC_FILEBROWSER_DIRLIST: kind = "file_browser_directories"; break;
        case PLO_RC_FILEBROWSER_DIR: kind = "file_browser_directory"; break;
        case PLO_RC_FILEBROWSER_MESSAGE: kind = "file_browser_message"; break;
        case PLO_RC_ACCOUNTLISTGET: kind = "account_list"; break;
        case PLO_RC_PLAYERPROPSGET: kind = "player_properties"; break;
        case PLO_RC_ACCOUNTGET: kind = "account"; break;
        case PLO_RC_SERVERFLAGSGET: kind = "server_flags"; break;
        case PLO_RC_PLAYERRIGHTSGET: kind = "player_rights"; break;
        case PLO_RC_PLAYERCOMMENTSGET: kind = "player_comments"; break;
        case PLO_RC_PLAYERBANGET: kind = "player_ban"; break;
        case PLO_RC_SERVEROPTIONSGET: kind = "server_options"; break;
        case PLO_RC_FOLDERCONFIGGET: kind = "folder_config"; break;
        case PLO_RC_MAXUPLOADFILESIZE: kind = "max_upload_file_size"; break;
        case PLO_NC_CONTROL: kind = "npc_control"; break;
        case PLO_NC_LEVELLIST: kind = "npc_level_list"; break;
        default: break;
        }
        remote_kind_event(client, ext, packet_id, kind, json);
        return true;
    }
    case PLO_LARGEFILESTART:
    case PLO_LARGEFILEEND:
    case PLO_LARGEFILESIZE:
    case PLO_UPDATEPACKAGESIZE:
    case PLO_UPDATEPACKAGEDONE:
    case PLO_UPDATEPACKAGEISUPDATED:
        tclient_transfer_event(client, packet_id, payload);
        tclient_emit_event(client, packet_id, json);
        return true;
    default:
        tclient_emit_event(client, packet_id, json);
        return true;
    }
}

void tclient_transfer_event(TClient* client, int packet_id, const std::vector<uint8_t>& payload) {
    CallbackTransfer cb;
    {
        std::lock_guard<std::mutex> lock(client->cb_mutex);
        cb = client->ext.transfer;
    }
    Rd rd(payload);
    switch (packet_id) {
    case PLO_LARGEFILESTART: {
        std::lock_guard<std::mutex> lock(client->state_mutex);
        client->large_file = TClientLargeFile{};
        client->large_file.active = true;
        client->large_file.name = as_text(payload);
        if (cb.cb) cb.cb("large_file_start", client->large_file.name.c_str(), 0, 0, cb.ud);
        break;
    }
    case PLO_LARGEFILESIZE: {
        long long size = rd.g5();
        std::lock_guard<std::mutex> lock(client->state_mutex);
        client->large_file.expected = size;
        if (client->large_file.expected > 0 && client->large_file.expected < (1LL << 30)) {
            client->large_file.data.reserve(static_cast<size_t>(client->large_file.expected));
        }
        if (cb.cb) cb.cb("large_file_size", client->large_file.name.c_str(), size, 0, cb.ud);
        break;
    }
    case PLO_LARGEFILEEND: {
        std::string name = as_text(payload);
        TClientLargeFile finished;
        {
            std::lock_guard<std::mutex> lock(client->state_mutex);
            finished = std::move(client->large_file);
            client->large_file = TClientLargeFile{};
        }
        bool complete = finished.expected <= 0 || static_cast<long long>(finished.data.size()) >= finished.expected;
        CallbackFile file_cb;
        {
            std::lock_guard<std::mutex> lock(client->cb_mutex);
            file_cb = client->on_file;
        }
        if (finished.active && file_cb.cb && !finished.data.empty()) {
            file_cb.cb(name.c_str(), finished.data.data(), static_cast<int>(finished.data.size()), finished.mod_time, file_cb.ud);
        }
        if (cb.cb) cb.cb("large_file_end", name.c_str(), static_cast<long long>(finished.data.size()), complete ? 1 : 0, cb.ud);
        break;
    }
    case PLO_UPDATEPACKAGESIZE: {
        std::string name = rd.gstr();
        long long size = rd.g5();
        {
            std::lock_guard<std::mutex> lock(client->state_mutex);
            client->world.update_package = name;
            client->world.update_package_size = size;
        }
        if (cb.cb) cb.cb("update_package_size", name.c_str(), size, 0, cb.ud);
        break;
    }
    case PLO_UPDATEPACKAGEDONE: {
        std::string name = as_text(payload);
        if (cb.cb) cb.cb("update_package_done", name.c_str(), 0, 1, cb.ud);
        break;
    }
    case PLO_UPDATEPACKAGEISUPDATED: {
        std::string name = as_text(payload);
        if (cb.cb) cb.cb("update_package_updated", name.c_str(), 0, 1, cb.ud);
        break;
    }
    default:
        break;
    }
}

bool tclient_large_file_append(TClient* client, const std::string& filename, int mod_time, const std::vector<uint8_t>& chunk) {
    std::lock_guard<std::mutex> lock(client->state_mutex);
    if (!client->large_file.active || client->large_file.name != filename) return false;
    client->large_file.mod_time = mod_time;
    client->large_file.data.insert(client->large_file.data.end(), chunk.begin(), chunk.end());
    return true;
}

std::string tclient_world_json(TClient* client, const std::string& kind) {
    std::lock_guard<std::mutex> lock(client->state_mutex);
    const TClientWorldState& w = client->world;
    auto map_values = [](const std::unordered_map<std::string, std::string>& m) {
        std::string out = "[";
        bool first = true;
        for (const auto& kv : m) {
            if (!first) out += ",";
            first = false;
            out += kv.second;
        }
        return out + "]";
    };
    if (kind == "npcs") {
        std::string out = "[";
        bool first = true;
        for (const auto& kv : w.npcs) {
            if (!first) out += ",";
            first = false;
            out += "{\"id\":" + std::to_string(kv.first) + ",\"props\":" + kv.second + "}";
        }
        return out + "]";
    }
    if (kind == "flags") {
        std::string out = "{";
        bool first = true;
        for (const auto& kv : w.flags) {
            if (!first) out += ",";
            first = false;
            out += "\"" + jesc(kv.first) + "\":\"" + jesc(kv.second) + "\"";
        }
        return out + "}";
    }
    if (kind == "weapons") return Js().list("weapons", w.weapons).str();
    if (kind == "horses") return map_values(w.horses);
    if (kind == "baddies") {
        std::string out = "[";
        bool first = true;
        for (const auto& kv : w.baddies) {
            if (!first) out += ",";
            first = false;
            out += kv.second;
        }
        return out + "]";
    }
    if (kind == "items") return map_values(w.items);
    if (kind == "bombs") return map_values(w.bombs);
    if (kind == "chests") return map_values(w.chests);
    if (kind == "signs") {
        std::string out = "{";
        bool first = true;
        for (const auto& kv : w.signs) {
            if (!first) out += ",";
            first = false;
            out += "\"" + jesc(kv.first) + "\":\"" + jesc(kv.second) + "\"";
        }
        return out + "}";
    }
    if (kind == "links") {
        std::string out = "[";
        for (size_t n = 0; n < w.links.size(); ++n) {
            if (n) out += ",";
            out += w.links[n];
        }
        return out + "]";
    }
    if (kind == "classes") {
        std::string out = "{";
        bool first = true;
        for (const auto& kv : w.classes) {
            if (!first) out += ",";
            first = false;
            out += "\"" + jesc(kv.first) + "\":\"" + jesc(kv.second) + "\"";
        }
        return out + "}";
    }
    if (kind == "level") {
        return Js().t("name", w.level_name).b("has_tiles", w.level_has_tiles).i("tile_count", w.level_tile_count)
                   .i("npc_count", w.level_npc_count).str();
    }
    if (kind == "session") {
        return Js().t("active_level", w.active_level).t("current_level", client->current_level).b("frozen", w.frozen)
                   .b("leader", w.leader).i("world_time", w.world_time).t("net_cookie", w.net_cookie)
                   .t("server_options", w.server_options).t("update_package", w.update_package)
                   .i("update_package_size", w.update_package_size).i("self_player_id", client->self_player_id).str();
    }
    return "{}";
}

void tclient_publish_level_summary(TClient* client) {
    std::string name = client->level_state.name;
    bool has_tiles = client->level_state.has_tiles;
    long long tiles = static_cast<long long>(client->level_state.tiles.size());
    long long npcs = static_cast<long long>(client->level_state.npcs.size());
    std::lock_guard<std::mutex> lock(client->state_mutex);
    client->world.level_name = name;
    client->world.level_has_tiles = has_tiles;
    client->world.level_tile_count = tiles;
    client->world.level_npc_count = npcs;
}

namespace {

const char* npc_prop_name(int prop) {
    switch (prop) {
    case 0: return "image"; case 1: return "script"; case 2: return "x"; case 3: return "y"; case 4: return "power";
    case 5: return "rupees"; case 6: return "arrows"; case 7: return "bombs"; case 8: return "glove_power";
    case 9: return "bomb_power"; case 10: return "sword"; case 11: return "shield"; case 12: return "gani";
    case 13: return "visibility_flags"; case 14: return "blocking_flags"; case 15: return "message";
    case 16: return "hurt_dx_dy"; case 17: return "id"; case 18: return "sprite"; case 19: return "colors";
    case 20: return "nickname"; case 21: return "horse_image"; case 22: return "head_image"; case 33: return "alignment";
    case 34: return "image_part"; case 35: return "body_image"; case 41: return "gmap_level_x"; case 42: return "gmap_level_y";
    case 43: return "z"; case 48: return "unknown48"; case 49: return "scripter"; case 50: return "name"; case 51: return "type";
    case 52: return "level"; case 74: return "classes"; case 75: return "pixel_x"; case 76: return "pixel_y"; case 77: return "pixel_z";
    default: return nullptr;
    }
}

} // namespace

std::string tclient_npc_props_json(const std::vector<uint8_t>& payload, size_t start) {
    Rd rd(payload);
    rd.r.pos = std::min(start, payload.size());
    Js j;
    while (rd.has()) {
        int prop = rd.gc();
        char fallback[16];
        const char* name = npc_prop_name(prop);
        std::string key;
        if (name) key = name;
        else if (prop >= 23 && prop <= 32) key = "save" + std::to_string(prop - 23);
        else if (prop >= 36 && prop <= 40) key = "attr" + std::to_string(prop - 35);
        else if (prop >= 44 && prop <= 47) key = "attr" + std::to_string(prop - 38);
        else if (prop >= 53 && prop <= 73) key = "attr" + std::to_string(prop - 43);
        else { std::snprintf(fallback, sizeof(fallback), "p%d", prop); key = fallback; }
        // Keys are built per property, so the builder needs a stable C string for each one.
        static thread_local std::string held;
        held = key;
        const char* k = held.c_str();
        if (prop == 0 || prop == 12 || prop == 15 || prop == 20 || prop == 21 || prop == 35 || prop == 49 || prop == 50 ||
            prop == 51 || prop == 52 || (prop >= 36 && prop <= 40) || (prop >= 44 && prop <= 47) || (prop >= 53 && prop <= 73)) {
            j.t(k, rd.gstr());
        } else if (prop == 1 || prop == 74) {
            int n = rd.gs();
            j.t(k, rd.r.str(static_cast<size_t>(std::max(0, n))));
        } else if (prop == 2 || prop == 3) {
            j.d(k, rd.sb() / 2.0);
        } else if (prop == 4) {
            j.d(k, rd.gc() / 2.0);
        } else if (prop == 5 || prop == 17) {
            j.i(k, rd.g3());
        } else if (prop == 10 || prop == 11) {
            int v = rd.gc();
            std::string image = v > (prop == 10 ? 4 : 3) ? rd.gstr() : std::string();
            j.raw(k, Js().i("raw", v).t("image", image).str());
        } else if (prop == 19) {
            std::vector<int> colors;
            for (int n = 0; n < 5 && rd.has(); ++n) colors.push_back(rd.gc());
            j.ints(k, colors);
        } else if (prop == 22) {
            int v = rd.gc();
            j.t(k, v >= 100 ? rd.r.str(static_cast<size_t>(v - 100)) : std::to_string(v));
        } else if (prop == 34) {
            std::vector<int> parts;
            for (int n = 0; n < 6 && rd.has(); ++n) parts.push_back(rd.r.byte());
            j.ints(k, parts);
        } else if (prop == 75 || prop == 76 || prop == 77) {
            int raw = rd.gs();
            j.d(k, (raw & 1 ? -1 : 1) * (raw >> 1));
        } else if (prop == 6 || prop == 7 || prop == 8 || prop == 9 || prop == 13 || prop == 14 || prop == 18 || prop == 33 ||
                   prop == 41 || prop == 42 || prop == 43 || (prop >= 23 && prop <= 32)) {
            j.i(k, rd.gc());
        } else {
            j.hex("unhandled_remaining_hex", rd.rest_bytes());
        }
    }
    return j.str();
}
