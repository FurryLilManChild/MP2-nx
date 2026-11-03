// mp2-nx: no Unix domain sockets on Horizon. Enough for Echoes Unbound's Discord
// presence code to compile; its connect() then fails and the feature stays off.
#pragma once
#include <sys/socket.h>
#ifndef AF_UNIX
#define AF_UNIX 1
#endif
struct sockaddr_un {
  sa_family_t sun_family;
  char sun_path[108];
};
