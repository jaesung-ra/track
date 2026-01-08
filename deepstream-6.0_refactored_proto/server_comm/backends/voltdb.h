#ifndef VOLTDB_H
#define VOLTDB_H

#include "server_interface.h"
#include "config_parser.h"
#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

class VoltDB : public ServerInterface {
private:
    std::string db_host;
    int db_port;
    
    // 로거 인스턴스
    std::shared_ptr<spdlog::logger> logger = NULL;
    
    std::string executeQuery(const std::string &query) override;
public:
    VoltDB(const std::string &config_path);

    std::string getCamrID(const std::string &ip) override;
    int getMvmtInfo(std::vector<int> &result, const std::string &spot_ints_id) override;
    int getPhaseInfo(std::vector<int> &result, const std::string &spot_ints_id, int &LC_CNT) override;
};

#endif