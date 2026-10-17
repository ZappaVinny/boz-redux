#include "s3e_host_internal.h"
#include "game_patches.h"
#include "s3e_image.h"

#include <assert.h>

char g_root[1024];

static const char embedded_config[] = "[Demonware]\n"
                                      "LSGServer=codboh-iphone-lobby.prod.demonware.net:3074\n"
                                      "AuthServer=codboh-iphone-auth.prod.demonware.net:3074\n"
                                      "[STUN]\n"
                                      "STUNServer=codboh-iphone.stun.demonware.net\n"
                                      "STUNServerPort=3478\n"
                                      "[GAME]\n"
                                      "OnlineAccount=GC\n"
                                      "GameVersion=1.0.9\n"
                                      "MatchmakingSearchAndPublishMode=1\n"
                                      "[ONLINE]\n"
                                      "dispatcher=/isonline/dispatcher.php\n";

static void assert_config_string(const char *section, const char *key, const char *expected) {
    char value[256];
    memset(value, 0, sizeof(value));
    assert(s3eConfigGetString(section, key, value) == 0);
    assert(strcmp(value, expected) == 0);
}

static void test_offline_defaults(void) {
    s3e_host_set_config((const uint8_t *)embedded_config, (uint32_t)strlen(embedded_config));
    assert_config_string("GAME", "OnlineAccount", "NONE");
    assert_config_string("GAME", "MatchmakingSearchAndPublishMode", "0");
    assert_config_string("Demonware", "AuthServer", "");
    assert_config_string("Demonware", "LSGServer", "");
    assert_config_string("ONLINE", "dispatcher", "");
    assert_config_string("GAME", "VoiceChatEnabled", "0");
}

static void test_direct_multiplayer_server(void) {
    setenv("BOZ_MULTIPLAYER_SERVER", "192.0.2.10", 1);
    s3e_host_set_config((const uint8_t *)embedded_config, (uint32_t)strlen(embedded_config));

    assert_config_string("GAME", "OnlineAccount", "GENERIC");
    assert_config_string("GAME", "GameVersion", "1.0.11");
    assert_config_string("GAME", "MatchmakingSearchAndPublishMode", "1");
    assert_config_string("Demonware", "AuthServer", "codboh-iphone-auth.prod.demonware.net:3074");
    assert_config_string("Demonware", "LSGServer", "codboh-iphone-lobby.prod.demonware.net:3074");
    assert_config_string("ONLINE", "dispatcher", "/isonline/dispatcher.php");

    assert(strcmp(s3e_multiplayer_resolve_hostname("codboh-iphone-auth.prod.demonware.net"),
                  "192.0.2.10") == 0);
    assert(strcmp(s3e_multiplayer_resolve_hostname(".demonware.net"), "192.0.2.10") == 0);
    assert(strcmp(s3e_multiplayer_resolve_hostname("example.net"), "example.net") == 0);
    assert(strcmp(s3e_multiplayer_resolve_hostname("host.Demonware.net"), "192.0.2.10") == 0);
    assert(s3e_multiplayer_resolve_hostname(NULL) == NULL);

    unsetenv("BOZ_MULTIPLAYER_SERVER");
}

static void test_voice_chat_opt_in(void) {
    setenv("BOZ_VOICE_CHAT", "1", 1);
    s3e_host_set_config((const uint8_t *)embedded_config, (uint32_t)strlen(embedded_config));
    assert_config_string("GAME", "VoiceChatEnabled", "1");

    setenv("BOZ_VOICE_CHAT", "invalid", 1);
    s3e_host_set_config((const uint8_t *)embedded_config, (uint32_t)strlen(embedded_config));
    assert_config_string("GAME", "VoiceChatEnabled", "0");
    unsetenv("BOZ_VOICE_CHAT");
}

static void test_player_name(void) {
    setenv("BOZ_PLAYER_NAME", "Producdevity", 1);
    s3e_host_set_config((const uint8_t *)embedded_config, (uint32_t)strlen(embedded_config));
    assert(strcmp(s3e_host_player_name(), "Producdevity") == 0);

    setenv("BOZ_PLAYER_NAME", "too-long-player-name", 1);
    s3e_host_set_config((const uint8_t *)embedded_config, (uint32_t)strlen(embedded_config));
    assert(strcmp(s3e_host_player_name(), "Player") == 0);
    unsetenv("BOZ_PLAYER_NAME");
}

static void test_player_name_patch(void) {
    enum {
        reference_offset = 0x18f74c,
        reference_pc_offset = 0x18f60a,
        format_offset = 0x3af131,
    };
    size_t size = format_offset + sizeof("Player-%d");
    uint8_t *memory = calloc(1, size);
    assert(memory);
    memcpy(memory + format_offset, "Player-%d", sizeof("Player-%d"));
    uint32_t original = format_offset - reference_pc_offset;
    memcpy(memory + reference_offset, &original, sizeof(original));

    struct s3e_loaded_image loaded = {.base = memory, .map_size = size};
    uint32_t name_address = 0x12345678;

    memory[format_offset] = 'X';
    assert(!player_name_install(&loaded, name_address));
    memory[format_offset] = 'P';

    assert(player_name_install(&loaded, name_address));
    uint32_t patched;
    memcpy(&patched, memory + reference_offset, sizeof(patched));
    assert(patched == name_address - ((uint32_t)(uintptr_t)memory + reference_pc_offset));
    free(memory);
}

int main(void) {
    char template[] = "/tmp/codboz-config.XXXXXX";
    char *directory = mkdtemp(template);
    assert(directory);
    snprintf(g_root, sizeof(g_root), "%s", directory);

    test_offline_defaults();
    test_direct_multiplayer_server();
    test_voice_chat_opt_in();
    test_player_name();
    test_player_name_patch();

    int result = rmdir(g_root);
    assert(result == 0);
    puts("s3e config tests passed");
    return 0;
}
