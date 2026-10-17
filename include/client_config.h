#ifndef CLIENT_CONFIG_H
#define CLIENT_CONFIG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads <root>/client.ini (writing a default one if missing) into the BOZ_* settings. */
void client_config_load(const char *root);

/* A raw value from the [keys] section, or NULL when the file does not set it. */
const char *client_config_key_binding(const char *action);

/* The raw [mods] order and disabled lists (comma-separated mod ids); empty when unset. */
const char *client_config_mods_order(void);
const char *client_config_mods_disabled(void);

/* The [keys] actions in the order the input code numbers them: name in client.ini, the
 * launcher's label, the default keys, and the comment written above it (or NULL). */
struct client_binding {
    const char *action;
    const char *label;
    const char *defaults;
    const char *comment;
};
extern const struct client_binding CLIENT_BINDINGS[];
extern const int CLIENT_BINDING_COUNT;

/* Writes the commented default client.ini to path. */
bool client_config_write_default(const char *path);

#ifdef __cplusplus
}
#endif

#endif
