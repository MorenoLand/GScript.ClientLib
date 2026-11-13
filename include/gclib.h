#ifndef GCLIB_H
#define GCLIB_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#ifdef GCLIB_EXPORTS
#define GCLIB_API __declspec(dllexport)
#else
#define GCLIB_API __declspec(dllimport)
#endif
#else
#define GCLIB_API
#endif

typedef void* GCHandle;

typedef enum {
    GC_VERSION_222 = 0,
    GC_VERSION_6037 = 1,
    GC_VERSION_6037_LINUX = 2,
    GC_VERSION_62 = 3
} GCVersion;

typedef void (*GC_OnConnected)(void* user_data);
typedef void (*GC_OnDisconnected)(const char* reason, void* user_data);
typedef void (*GC_OnAuthenticated)(void* user_data);
typedef void (*GC_OnRawPacket)(int packet_id, const void* data, int length, void* user_data);
typedef void (*GC_OnPacketEvent)(int packet_id, const char* packet_name, const char* event_json, void* user_data);
typedef void (*GC_OnChat)(int player_id, const char* message, void* user_data);
typedef void (*GC_OnChatEx)(int player_id, const char* account, const char* nickname, const char* community, const char* level, const char* message, void* user_data);
typedef void (*GC_OnPrivateMessage)(int player_id, const char* message_type, const char* message, void* user_data);
typedef void (*GC_OnLevelName)(const char* level, void* user_data);
typedef void (*GC_OnPlayerWarp)(float x, float y, const char* level, void* user_data);
typedef void (*GC_OnPlayerWarp2)(float x, float y, int z, int gmap_x, int gmap_y, const char* level, void* user_data);
typedef void (*GC_OnPlayerProps)(const char* props_json, void* user_data);
typedef void (*GC_OnOtherPlayerProps)(int player_id, const char* props_json, void* user_data);
typedef void (*GC_OnPlayerLeft)(int player_id, void* user_data);
typedef void (*GC_OnBoardPacket)(const unsigned short* tiles, int count, const void* raw, int raw_length, void* user_data);
typedef void (*GC_OnFile)(const char* filename, const void* data, int length, int mod_time, void* user_data);
typedef void (*GC_OnFileFailed)(const char* filename, void* user_data);
typedef void (*GC_OnWorldTime)(int world_time, void* user_data);
typedef void (*GC_OnNpcProps)(int npc_id, const char* props_json, void* user_data);
typedef void (*GC_OnNpcDeleted)(int npc_id, void* user_data);
typedef void (*GC_OnSign)(float x, float y, const char* text, void* user_data);
typedef void (*GC_OnExplosion)(float x, float y, int radius, int power, void* user_data);
typedef void (*GC_OnHitObjects)(float x, float y, int power, int player_id, void* user_data);
typedef void (*GC_OnServerText)(const char* text, void* user_data);
typedef void (*GC_OnFlagSet)(const char* name, const char* value, void* user_data);
typedef void (*GC_OnFlagDel)(const char* name, void* user_data);
typedef void (*GC_OnWeaponScript)(const char* script_type, const char* script_name, const void* bytecode, int length, void* user_data);
typedef void (*GC_OnResource)(const char* resource_type, const char* name, const void* data, int length, int packet_id, void* user_data);

typedef void (*GC_OnHorse)(int added, float x, float y, int direction, int bushes, const char* image, void* user_data);
typedef void (*GC_OnBaddyProps)(int baddy_id, const char* props_json, void* user_data);
typedef void (*GC_OnBaddyHurt)(int baddy_id, int hurt_x, int hurt_y, int damage, void* user_data);
typedef void (*GC_OnItem)(int added, float x, float y, int item_id, void* user_data);
typedef void (*GC_OnBomb)(int added, int player_id, float x, float y, int power, int time_to_explode, void* user_data);
typedef void (*GC_OnArrow)(int player_id, float x, float y, int flags, int sprite, int power, void* user_data);
typedef void (*GC_OnChest)(int visible, int x, int y, int item_type, int sign_index, void* user_data);
typedef void (*GC_OnLevelLink)(const char* destination_level, int x, int y, int width, int height, const char* new_x, const char* new_y, void* user_data);
typedef void (*GC_OnTriggerAction)(int player_id, int npc_id, float x, float y, const char* action, const char* params, void* user_data);
typedef void (*GC_OnSay2)(const char* text, void* user_data);
typedef void (*GC_OnFreeze)(int frozen, void* user_data);
typedef void (*GC_OnActiveLevel)(const char* level, void* user_data);
typedef void (*GC_OnServerWarp)(const char* identifier, const char* name, const char* host, const char* port, void* user_data);
typedef void (*GC_OnNetCookie)(const char* cookie, void* user_data);
typedef void (*GC_OnServerOptions)(const char* options, void* user_data);
typedef void (*GC_OnProcessQuery)(const char* kind, const char* data, void* user_data);
typedef void (*GC_OnProfile)(const char* account, const char* fields_json, void* user_data);
typedef void (*GC_OnRemoteEvent)(const char* kind, const char* event_json, void* user_data);
typedef void (*GC_OnListServer)(const char* kind, const char* event_json, void* user_data);
typedef void (*GC_OnTransfer)(const char* kind, const char* name, long long size, int complete, void* user_data);
typedef void (*GC_OnSendCapture)(int packet_id, const void* data, int length, void* user_data);
typedef void (*GC_OnConnectionState)(const char* state, int attempt, const char* reason, void* user_data);

GCLIB_API GCHandle gc_create(const char* host, int port, const char* version);
GCLIB_API GCHandle gc_create_version(const char* host, int port, GCVersion version);
GCLIB_API void gc_destroy(GCHandle handle);
GCLIB_API void gc_set_client_info(GCHandle handle, const char* client_info);
GCLIB_API void gc_set_client_identity(GCHandle handle, const char* platform, const char* id1, const char* id2, const char* id3, const char* id4);
GCLIB_API void gc_set_handshake(GCHandle handle, const char* handshake);
GCLIB_API void gc_set_encryption_certificate_pem(GCHandle handle, const char* certificate_pem);
GCLIB_API int gc_set_encryption_certificate_file(GCHandle handle, const char* path);
GCLIB_API int gc_connect(GCHandle handle);
GCLIB_API int gc_login(GCHandle handle, const char* account, const char* password);
GCLIB_API void gc_disconnect(GCHandle handle);
GCLIB_API int gc_is_connected(GCHandle handle);
GCLIB_API int gc_is_authenticated(GCHandle handle);
GCLIB_API int gc_poll(GCHandle handle);
GCLIB_API const char* gc_get_last_error(GCHandle handle);
GCLIB_API char* gc_get_player_json(GCHandle handle, int player_id);
GCLIB_API char* gc_get_self_player_json(GCHandle handle);
GCLIB_API int gc_set_resource_dump_directory(GCHandle handle, const char* directory);
GCLIB_API int gc_set_resource_dump_types(GCHandle handle, const char* comma_separated_types);

GCLIB_API void gc_on_connected(GCHandle handle, GC_OnConnected callback, void* user_data);
GCLIB_API void gc_on_disconnected(GCHandle handle, GC_OnDisconnected callback, void* user_data);
GCLIB_API void gc_on_authenticated(GCHandle handle, GC_OnAuthenticated callback, void* user_data);
GCLIB_API void gc_on_raw_packet(GCHandle handle, GC_OnRawPacket callback, void* user_data);
GCLIB_API void gc_on_packet_event(GCHandle handle, GC_OnPacketEvent callback, void* user_data);
GCLIB_API void gc_on_chat(GCHandle handle, GC_OnChat callback, void* user_data);
GCLIB_API void gc_on_chat_ex(GCHandle handle, GC_OnChatEx callback, void* user_data);
GCLIB_API void gc_on_private_message(GCHandle handle, GC_OnPrivateMessage callback, void* user_data);
GCLIB_API void gc_on_level_name(GCHandle handle, GC_OnLevelName callback, void* user_data);
GCLIB_API void gc_on_player_warp(GCHandle handle, GC_OnPlayerWarp callback, void* user_data);
GCLIB_API void gc_on_player_warp2(GCHandle handle, GC_OnPlayerWarp2 callback, void* user_data);
GCLIB_API void gc_on_player_props(GCHandle handle, GC_OnPlayerProps callback, void* user_data);
GCLIB_API void gc_on_other_player_props(GCHandle handle, GC_OnOtherPlayerProps callback, void* user_data);
GCLIB_API void gc_on_player_left(GCHandle handle, GC_OnPlayerLeft callback, void* user_data);
GCLIB_API void gc_on_board_packet(GCHandle handle, GC_OnBoardPacket callback, void* user_data);
GCLIB_API void gc_on_file(GCHandle handle, GC_OnFile callback, void* user_data);
GCLIB_API void gc_on_file_failed(GCHandle handle, GC_OnFileFailed callback, void* user_data);
GCLIB_API void gc_on_world_time(GCHandle handle, GC_OnWorldTime callback, void* user_data);
GCLIB_API void gc_on_npc_props(GCHandle handle, GC_OnNpcProps callback, void* user_data);
GCLIB_API void gc_on_npc_deleted(GCHandle handle, GC_OnNpcDeleted callback, void* user_data);
GCLIB_API void gc_on_sign(GCHandle handle, GC_OnSign callback, void* user_data);
GCLIB_API void gc_on_explosion(GCHandle handle, GC_OnExplosion callback, void* user_data);
GCLIB_API void gc_on_hit_objects(GCHandle handle, GC_OnHitObjects callback, void* user_data);
GCLIB_API void gc_on_server_text(GCHandle handle, GC_OnServerText callback, void* user_data);
GCLIB_API void gc_on_flag_set(GCHandle handle, GC_OnFlagSet callback, void* user_data);
GCLIB_API void gc_on_flag_del(GCHandle handle, GC_OnFlagDel callback, void* user_data);
GCLIB_API void gc_on_weapon_script(GCHandle handle, GC_OnWeaponScript callback, void* user_data);
GCLIB_API void gc_on_resource(GCHandle handle, GC_OnResource callback, void* user_data);

GCLIB_API int gc_send_packet(GCHandle handle, int packet_id, const void* data, int length);
GCLIB_API int gc_send_chat(GCHandle handle, const char* message);
GCLIB_API int gc_send_level_chat(GCHandle handle, const char* message);
GCLIB_API int gc_send_toall_chat(GCHandle handle, const char* message);
GCLIB_API int gc_set_player_prop_string(GCHandle handle, int prop_id, const char* value);
GCLIB_API int gc_set_player_prop_byte(GCHandle handle, int prop_id, int value);
GCLIB_API int gc_set_nickname(GCHandle handle, const char* nickname);
GCLIB_API int gc_set_head_image(GCHandle handle, const char* image);
GCLIB_API int gc_set_body_image(GCHandle handle, const char* image);
GCLIB_API int gc_set_player_attribute(GCHandle handle, int attribute_index, const char* value);
GCLIB_API int gc_set_player_status(GCHandle handle, int status);
GCLIB_API int gc_set_player_colors(GCHandle handle, int skin, int coat, int sleeves, int shoes, int belt);
GCLIB_API int gc_move(GCHandle handle, float x, float y, int direction, int use_high_precision);
GCLIB_API int gc_walk_step(GCHandle handle, float x, float y, int direction, const char* gani, int use_high_precision);
GCLIB_API int gc_level_warp(GCHandle handle, float x, float y, const char* level);
GCLIB_API int gc_set_animation(GCHandle handle, const char* gani, float x, float y, int direction);
GCLIB_API int gc_request_file(GCHandle handle, const char* filename);
GCLIB_API int gc_request_level_board(GCHandle handle, const char* level, int mod_time, int x, int y, int width, int height);
GCLIB_API int gc_send_private_message(GCHandle handle, const int* player_ids, int count, const char* message);
GCLIB_API int gc_set_flag(GCHandle handle, const char* flag);
GCLIB_API int gc_unset_flag(GCHandle handle, const char* flag);
GCLIB_API int gc_request_text(GCHandle handle, const char* type, const char* key, const char* default_value);
GCLIB_API int gc_send_text(GCHandle handle, const char* type, const char* key, const char* metadata, const char* value);
GCLIB_API int gc_request_weapon_script(GCHandle handle, const char* name);
GCLIB_API int gc_request_class_script(GCHandle handle, const char* name, int mod_time);
GCLIB_API int gc_request_gani_script(GCHandle handle, const char* name, int mod_time);
GCLIB_API int gc_send_trigger_action(GCHandle handle, float x, float y, const char* action, const char* params);
GCLIB_API int gc_send_trigger_action_npc(GCHandle handle, int npc_id, float x, float y, const char* action, const char* params);
GCLIB_API int gc_send_hit_objects(GCHandle handle, float x, float y, int power, int npc_id);
GCLIB_API int gc_send_player_hurt(GCHandle handle, int target_player_id, float from_x, float from_y, float target_x, float target_y, int power);
GCLIB_API int gc_send_add_bomb(GCHandle handle, float x, float y, int power, int fuse_ticks, const char* image);
GCLIB_API int gc_send_del_bomb(GCHandle handle, float x, float y);
GCLIB_API int gc_send_item_add(GCHandle handle, float x, float y, int item_id);
GCLIB_API int gc_send_item_take(GCHandle handle, float x, float y, int item_id);
GCLIB_API int gc_send_item_del(GCHandle handle, float x, float y);
GCLIB_API int gc_send_open_chest(GCHandle handle, int x, int y);
GCLIB_API int gc_send_shot(GCHandle handle, float x, float y, int direction, int sprite, int power, int mirrored, int from_player);
GCLIB_API int gc_send_verify_file_crc(GCHandle handle, const char* filename, unsigned int crc32);
GCLIB_API int gc_send_level_warp_modtime(GCHandle handle, float x, float y, const char* level, unsigned int mod_time);
GCLIB_API int gc_send_adjacent_level(GCHandle handle, const char* level);
GCLIB_API int gc_send_delete_npc(GCHandle handle, int npc_id);
GCLIB_API int gc_send_put_npc(GCHandle handle, float x, float y, const char* image, const char* script);
GCLIB_API int gc_send_rc_chat(GCHandle handle, const char* message);
GCLIB_API int gc_send_server_warp(GCHandle handle, const char* destination);
GCLIB_API int gc_send_board_modify(GCHandle handle, int layer, int x, int y, int width, int height, const unsigned short* tiles);
GCLIB_API int gc_send_fire_spy(GCHandle handle, int x, int y);
GCLIB_API int gc_send_carry_throw(GCHandle handle);
GCLIB_API int gc_send_enter_level(GCHandle handle);
GCLIB_API int gc_send_delete_weapon(GCHandle handle, const char* name);
GCLIB_API int gc_send_explosion(GCHandle handle, float x, float y, int power, int radius);
GCLIB_API int gc_send_update_file(GCHandle handle, const char* filename, long long mod_time);
GCLIB_API int gc_send_have_window(GCHandle handle, int have, const char* name);
GCLIB_API int gc_send_ping_answer(GCHandle handle, int ping_value);
GCLIB_API int gc_send_is_pker(GCHandle handle, int player_id);
GCLIB_API int gc_send_upload_start(GCHandle handle, const char* filename);
GCLIB_API int gc_send_save_file(GCHandle handle, const char* filename, const void* data, int length);
GCLIB_API int gc_send_upload_end(GCHandle handle, const char* filename);
GCLIB_API int gc_send_request_file_deletion(GCHandle handle, const char* filename);
GCLIB_API int gc_send_request_folder_deletion(GCHandle handle, const char* path);
GCLIB_API int gc_send_request_file_rename(GCHandle handle, const char* source, const char* destination);
GCLIB_API int gc_send_request_files_move(GCHandle handle, const char* source_pattern, const char* destination_folder);
GCLIB_API int gc_send_weapon_img(GCHandle handle, const char* image);
GCLIB_API int gc_send_npc_props(GCHandle handle, const char* props);
GCLIB_API int gc_send_window_list(GCHandle handle, const char* window_list);
GCLIB_API int gc_send_preload_level(GCHandle handle, const char* level, unsigned int mod_time);
GCLIB_API int gc_send_board_modify_rect(GCHandle handle, const char* level, int layer, int x, int y, int width, int height, const unsigned short* tiles);
GCLIB_API int gc_send_shoot_projectile(GCHandle handle, double x, double y, double z, double angle, double z_angle, double power, double gravity, const char* gani, const char* gani_data, const char* options);
GCLIB_API int gc_send_download_file(GCHandle handle, const char* url, const char* filename, const char* destination_path);
GCLIB_API int gc_send_request_update_package(GCHandle handle, const char* package_name, int blocking);
GCLIB_API int gc_set_encryption_out(GCHandle handle, const char* cipher_type, const char* key, const char* iv);


GCLIB_API void gc_on_horse(GCHandle handle, GC_OnHorse callback, void* user_data);
GCLIB_API void gc_on_baddy_props(GCHandle handle, GC_OnBaddyProps callback, void* user_data);
GCLIB_API void gc_on_baddy_hurt(GCHandle handle, GC_OnBaddyHurt callback, void* user_data);
GCLIB_API void gc_on_item(GCHandle handle, GC_OnItem callback, void* user_data);
GCLIB_API void gc_on_bomb(GCHandle handle, GC_OnBomb callback, void* user_data);
GCLIB_API void gc_on_arrow(GCHandle handle, GC_OnArrow callback, void* user_data);
GCLIB_API void gc_on_chest(GCHandle handle, GC_OnChest callback, void* user_data);
GCLIB_API void gc_on_level_link(GCHandle handle, GC_OnLevelLink callback, void* user_data);
GCLIB_API void gc_on_trigger_action(GCHandle handle, GC_OnTriggerAction callback, void* user_data);
GCLIB_API void gc_on_say2(GCHandle handle, GC_OnSay2 callback, void* user_data);
GCLIB_API void gc_on_freeze(GCHandle handle, GC_OnFreeze callback, void* user_data);
GCLIB_API void gc_on_active_level(GCHandle handle, GC_OnActiveLevel callback, void* user_data);
GCLIB_API void gc_on_server_warp(GCHandle handle, GC_OnServerWarp callback, void* user_data);
GCLIB_API void gc_on_net_cookie(GCHandle handle, GC_OnNetCookie callback, void* user_data);
GCLIB_API void gc_on_server_options(GCHandle handle, GC_OnServerOptions callback, void* user_data);
GCLIB_API void gc_on_process_query(GCHandle handle, GC_OnProcessQuery callback, void* user_data);
GCLIB_API void gc_on_profile(GCHandle handle, GC_OnProfile callback, void* user_data);
GCLIB_API void gc_on_remote_event(GCHandle handle, GC_OnRemoteEvent callback, void* user_data);
GCLIB_API void gc_on_transfer(GCHandle handle, GC_OnTransfer callback, void* user_data);
GCLIB_API void gc_on_connection_state(GCHandle handle, GC_OnConnectionState callback, void* user_data);

GCLIB_API void gc_set_cipher_seed(GCHandle handle, unsigned int seed);
GCLIB_API void gc_set_auto_process_replies(GCHandle handle, int enabled);
GCLIB_API void gc_set_reconnect(GCHandle handle, int max_attempts, int delay_ms);
GCLIB_API void gc_set_keepalive(GCHandle handle, int interval_ms);
GCLIB_API char* gc_get_state_json(GCHandle handle, const char* kind);
GCLIB_API const char* gc_get_last_login_error(GCHandle handle);

GCLIB_API GCHandle gc_list_create(const char* host, int port, const char* version);
GCLIB_API void gc_list_destroy(GCHandle handle);
GCLIB_API void gc_list_on_event(GCHandle handle, GC_OnListServer callback, void* user_data);
GCLIB_API int gc_list_connect(GCHandle handle);
GCLIB_API int gc_list_request_servers(GCHandle handle, const char* account, const char* password);
GCLIB_API char* gc_list_get_servers_json(GCHandle handle);

GCLIB_API int gc_send_shoot(GCHandle handle, int source, int x, int y, int z, int angle, int z_angle, int speed, const char* gani, const char* parameters);
GCLIB_API int gc_send_shoot2(GCHandle handle, int pixel_x, int pixel_y, int pixel_z, int offset_x, int offset_y, int angle, int z_angle, int power, int gravity, const char* gani, const char* parameters);
GCLIB_API int gc_send_adjacent_level_modtime(GCHandle handle, const char* level, long long mod_time);
GCLIB_API int gc_send_weapon_add(GCHandle handle, int weapon_type, int default_weapon_id, unsigned int npc_id);
GCLIB_API int gc_send_language(GCHandle handle, const char* language);
GCLIB_API int gc_send_tamper_check(GCHandle handle, long long hash1, long long hash2);
GCLIB_API int gc_send_enter_level_state(GCHandle handle, int state);
GCLIB_API int gc_send_baddy_props(GCHandle handle, int baddy_id, const void* props, int length);
GCLIB_API int gc_send_baddy_hurt(GCHandle handle, int baddy_id, int hurt_x, int hurt_y, int damage);
GCLIB_API int gc_send_baddy_add(GCHandle handle, float x, float y, int type, int power, const char* image);
GCLIB_API int gc_send_horse_add(GCHandle handle, float x, float y, int direction, int bushes, const char* image);
GCLIB_API int gc_send_horse_del(GCHandle handle, float x, float y);
GCLIB_API int gc_send_arrow_add(GCHandle handle, float x, float y, int flags, int sprite, int power);
GCLIB_API int gc_send_npc_server_query(GCHandle handle, int query_id, const char* message);
GCLIB_API int gc_send_raw_data_header(GCHandle handle, int length);
GCLIB_API int gc_send_profile_get(GCHandle handle, const char* account);
GCLIB_API int gc_send_profile_set(GCHandle handle, const char* account, const char* name, const char* age, const char* sex, const char* country, const char* messenger, const char* email, const char* url, const char* hangout, const char* quote);
GCLIB_API int gc_send_process_running(GCHandle handle, int running);
GCLIB_API int gc_send_process_list(GCHandle handle, const char* comma_separated_names);
GCLIB_API int gc_send_mute_player(GCHandle handle, int player_id);
GCLIB_API int gc_send_request_update_board(GCHandle handle, const char* level, long long mod_time, int x, int y, int width, int height);

GCLIB_API int gc_rc_send(GCHandle handle, const char* command, const char* const* args, int arg_count, const void* blob, int blob_length);
GCLIB_API int gc_nc_send(GCHandle handle, const char* command, const char* const* args, int arg_count, const void* blob, int blob_length);

GCLIB_API void gc_set_send_capture(GCHandle handle, GC_OnSendCapture callback, void* user_data);
GCLIB_API int gc_inject_packet(GCHandle handle, int packet_id, const void* data, int length);
GCLIB_API char* gc_decode_packet_json(int packet_id, const void* data, int length);

GCLIB_API void gc_free_string(char* value);

#ifdef __cplusplus
}
#endif

#endif
