#ifndef BOZ_API_H
#define BOZ_API_H

/* The mod Lua API's number (client.api in Lua): one more each time the client adds a global or a
 * function to one. A mod's mod.toml records the number it was built for ([sdk] client_api), so
 * the launcher can say when a mod may not work. 1 is the API of client 0.4.0. */
#define BOZ_LUA_API 1

#endif
