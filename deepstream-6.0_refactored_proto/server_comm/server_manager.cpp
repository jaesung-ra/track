#include "server_manager.h"

std::unique_ptr<ServerInterface> ServerManager::createServer(const std::string& type) {
    if (type == "VoltDB") {
        return std::make_unique<VoltDB>("path/to/volt/config");
    }
    return nullptr;
}