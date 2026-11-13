#ifndef GCLIB_TCLIENT_PACKET_DISPATCH_H
#define GCLIB_TCLIENT_PACKET_DISPATCH_H

#include "TClient.h"

#include <cstdint>
#include <string>
#include <vector>

void tclient_dispatch_packet(TClient* client, int packet_id, const std::vector<uint8_t>& payload);
void tclient_flush_pending_level_capture(TClient* client, int packet_id);
bool tclient_dispatch_extended(TClient* client, int packet_id, const std::vector<uint8_t>& payload);
bool tclient_decode_json(int packet_id, const std::vector<uint8_t>& payload, std::string& out);
void tclient_emit_event(TClient* client, int packet_id, const std::string& event_json);
void tclient_transfer_event(TClient* client, int packet_id, const std::vector<uint8_t>& payload);
bool tclient_large_file_append(TClient* client, const std::string& filename, int mod_time, const std::vector<uint8_t>& chunk);
void tclient_world_reset_level(TClient* client);
void tclient_world_flag(TClient* client, const std::string& name, const std::string& value, bool set);
void tclient_world_npc(TClient* client, int id, const std::string& props_json, bool remove);
void tclient_world_weapon(TClient* client, const std::string& name, bool add);
void tclient_world_sign(TClient* client, double x, double y, const std::string& text);
void tclient_world_clear_weapons(TClient* client);
std::string tclient_world_json(TClient* client, const std::string& kind);
#ifdef GCLIB_LISTSERVER
bool tclient_dispatch_list_packet(TClient* client, int packet_id, const std::vector<uint8_t>& payload);
#endif
std::string tclient_npc_props_json(const std::vector<uint8_t>& payload, size_t start);
void tclient_publish_level_summary(TClient* client);
void tclient_start_keepalive(TClient* client);
void tclient_process_decrypted(TClient* client, const std::vector<uint8_t>& decrypted);

#endif
