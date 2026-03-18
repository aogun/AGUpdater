#ifndef AG_ADMIN_API_H
#define AG_ADMIN_API_H

#include "http_server.h"

void register_admin_routes(httplib::Server &svr, AppContext &ctx);

#endif /* AG_ADMIN_API_H */
