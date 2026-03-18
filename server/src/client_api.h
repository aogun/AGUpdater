#ifndef AG_CLIENT_API_H
#define AG_CLIENT_API_H

#include "http_server.h"

void register_client_routes(httplib::Server &svr, AppContext &ctx);

#endif /* AG_CLIENT_API_H */
