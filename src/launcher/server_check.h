// Is a host a BOZ online server (one that speaks the game's online protocol, such as
// cod-boz-online)? Such a server has no status page, so it is recognised by how it answers:
//   TCP 3074 (sign-in and lobby): a lobby-ticket request (AccountForMMP) for an account that
//     does not exist gets the game's own AccountForMMP error reply (00 0b ...). Web servers,
//     game servers and the rest answer otherwise or not.
//   UDP 3478 (STUN, needed to start matches): a binding request with a CHANGE-REQUEST attribute,
//     as the game sends it, gets a binding response with the same transaction id.
// Nothing is signed in or created on the server.
#pragma once

#include <string>

struct ServerCheck {
    bool ok = false;
    std::string message;  // what the player sees
};

// Blocks for up to a few seconds: run it on a worker thread.
ServerCheck check_boz_server(const std::string &host);

// A server's name or address as the client accepts it (letters, digits, dots, dashes, colons).
bool valid_server_host(const std::string &host);
