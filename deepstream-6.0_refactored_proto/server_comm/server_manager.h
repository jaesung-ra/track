#ifndef SERVER_MANAGER_H
#define SERVER_MANAGER_H

#include "server_interface.h"
#include "voltdb.h"
#include <memory>

class ServerManager {
public:
    static std::unique_ptr<ServerInterface> createServer(const std::string& type);
};

#endif 