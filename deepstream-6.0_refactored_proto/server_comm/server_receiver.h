#ifndef SERVER_RECEIVER_H
#define SERVER_RECEIVER_H

#include "server_interface.h"
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>

class ServerReceiver {
private:
    ServerInterface* server;
    // RedisClient redis_client;
    std::string ip_addr;
    std::string spot_camr_id;
    std::string spot_ints_id;
    int target_signal;
    std::atomic<bool> run_thread;
    std::function<void(const std::string&)> callbackToSendQuery;
    
    std::string getIpAddress(); 
    int syncServer(
        int &LC_CNT,
        std::vector<std::pair<int, int>> &intervals,
        int &cycle_duration,
        int &interval_idx,
        std::map<int, int> &residual_cars
    );
    int signalTurnedGreen(int &prev_on_time, std::map<int, int> &residual_cars);
    int signalTurnedRed(std::map<int, int> &residual_cars);
public:
    ServerReceiver(ServerInterface* srv);
    ~ServerReceiver();

    int setCallback(std::function<int(const std::string&)> cb);
    void trafficSignalReceiveThread();
    std::string getCamrID(){ return spot_camr_id; };
};

#endif 