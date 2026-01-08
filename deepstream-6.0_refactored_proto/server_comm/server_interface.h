#ifndef SERVER_INTERFACE_H
#define SERVER_INTERFACE_H

#include <string>
#include <tuple>
#include "process_meta.h"

class ServerInterface {
private:
    virtual std::string executeQuery(const std::string &query) = 0;

public:
    virtual ~ServerInterface() {}  

    virtual std::string getCamrID(const std::string &ip) = 0;
    virtual int getMvmtInfo(std::vector<int> &result, const std::string &spot_ints_i) = 0;
    virtual int getPhaseInfo(std::vector<int> &result, const std::string &spot_ints_id, int &LC_CNT) = 0;
};

#endif 