#include "server_receiver.h"
#include <iostream>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif
static std::shared_ptr<spdlog::logger> logger = NULL;
ServerReceiver::ServerReceiver(ServerInterface* srv) : server(srv), run_thread(true) {
    if(logger == NULL){
        logger = getLogger("DS_server_log");
    }
    ip_addr = getIpAddress();
    spot_camr_id = server->getCamrID(ip_addr);
    int a, b;
    std::vector<int> directions;
    std::stringstream ss(spot_camr_id);
    std::string token;
    while (std::getline(ss, token, '_')) {
        directions.push_back(std::stoi(token));
    }
    spot_ints_id = std::to_string(directions[0]);
    a = directions[1];
    b = directions[2];

    if (b % 2 == 0 && b != 0)
        target_signal = b;
    else if (a % 2 == 1 && a != 0)
        target_signal = a;
    else
        return;
    
    if (realtime_enabled_4k)
        run_thread = false;
    std::cout << RED << "spot ints id : " << spot_ints_id << std::endl << "a, b : " << a << ", " << b << std::endl << "target signal : " << target_signal << RESET << std::endl;
    std::thread(&ServerReceiver::trafficSignalReceiveThread, this).detach();
}

ServerReceiver::~ServerReceiver() {
    run_thread = false;
}

int ServerReceiver::setCallback(std::function<int(const std::string&)> cb) {
    callbackToSendQuery = cb;
    return 0;
}

int ServerReceiver::syncServer(
    int &LC_CNT,
    std::vector<std::pair<int, int>> &intervals,
    int &cycle_duration,
    int &interval_idx,
    std::map<int, int> &residual_cars
) {
    int LC_CNT_before = LC_CNT;
    logger->info("Syncing with Server");
    std::vector<int> phase_mvmt_info;
    std::vector<int> phase_duration_info;
    int mvmt_res = server->getMvmtInfo(phase_mvmt_info, spot_ints_id);
    int phas_res = server->getPhaseInfo(phase_duration_info, spot_ints_id, LC_CNT);
    while(mvmt_res != 0 || phas_res != 0) {
        if (!intervals.empty()) {
            logger->debug("Something went wrong with Signal DB, continuing with previous Signal DB data");
            if (LC_CNT_before == LC_CNT)
                LC_CNT = intervals[0].first;
            return -1;
        }
        logger->debug("Initial sync with Signal DB failed. Retrying every 1 minute until it succeeds");
        std::this_thread::sleep_for(std::chrono::seconds(60));
        phase_mvmt_info.clear();
        phase_duration_info.clear();
        mvmt_res = server->getMvmtInfo(phase_mvmt_info, spot_ints_id);
        phas_res = server->getPhaseInfo(phase_duration_info, spot_ints_id, LC_CNT);
    }
    std::vector<int> a_mvmt(phase_mvmt_info.begin(), phase_mvmt_info.begin() + 8);
    std::vector<int> b_mvmt(phase_mvmt_info.begin() + 8, phase_mvmt_info.end());
    std::vector<int> a_duration(phase_duration_info.begin(), phase_duration_info.begin() + 8);
    std::vector<int> b_duration(phase_duration_info.begin() + 8, phase_duration_info.end());

    bool isA = std::find(a_mvmt.begin(), a_mvmt.end(), target_signal) != a_mvmt.end();
    bool isB = std::find(b_mvmt.begin(), b_mvmt.end(), target_signal) != b_mvmt.end();
    if (isA && isB) {
        logger->error("Target signal appears in both rings — invalid configuration!");
        return -1;
    }
    const std::vector<int>& selected_mvmt = isA ? a_mvmt : b_mvmt;
    const std::vector<int>& selected_duration = isA ? a_duration : b_duration;

    // logging mvmt, duration
    std::ostringstream mvmt_stream, dur_stream;
    mvmt_stream << "selected_mvmt: [";
    for (size_t i = 0; i < selected_mvmt.size(); ++i) {
        mvmt_stream << selected_mvmt[i];
        if (i != selected_mvmt.size() - 1)
            mvmt_stream << ", ";
    }
    mvmt_stream << "]";

    dur_stream << "selected_duration: [";
    for (size_t i = 0; i < selected_duration.size(); ++i) {
        dur_stream << selected_duration[i];
        if (i != selected_duration.size() - 1)
            dur_stream << ", ";
    }
    dur_stream << "]";
    logger->info("LC_CNT: "+std::to_string(LC_CNT));
    logger->info(mvmt_stream.str());
    logger->info(dur_stream.str());

    intervals.clear();
    int cur_time = 0;
    for (int i=0; i<8; i++) {
        if (selected_mvmt[i] == target_signal) {
            int start = cur_time;
            int duration = selected_duration[i];
            int end = start + duration;
            if (!intervals.empty() && intervals.back().second == start)
                intervals.back().second = end;
            else   
                intervals.emplace_back(start, end);
        }
        cur_time += selected_duration[i];
    }
    cycle_duration = cur_time;

    int sleep_sec = 0;
    interval_idx = 0;
    for (size_t i=0; i<intervals.size(); ++i) {
        int on = intervals[i].first;
        int off = intervals[i].second;
        if (LC_CNT >= on && LC_CNT < off) {
            sleep_sec = off - LC_CNT;
            return sleep_sec;
        }
        if (LC_CNT < on) {
            interval_idx = i;
            sleep_sec = on - LC_CNT;
            return sleep_sec;
        }
    }
    sleep_sec = cycle_duration - LC_CNT + intervals[0].first;
    interval_idx = 0;
    return sleep_sec;
}

void ServerReceiver::trafficSignalReceiveThread() {
    if (!run_thread)
        return;

    int prev_on_time = getCurTime();
    std::map<int, int> residual_cars;
    int LC_CNT;
    std::vector<std::pair<int, int>> intervals;
    int cycle_duration;
    int interval_idx;
    
    int sleep_sec = syncServer(LC_CNT, intervals, cycle_duration, interval_idx, residual_cars);
    std::this_thread::sleep_for(std::chrono::seconds(sleep_sec));
    if (LC_CNT >= intervals[interval_idx].first && LC_CNT < intervals[interval_idx].second) {
        signalTurnedRed(residual_cars);
        if (intervals.size() == 1) {
            std::this_thread::sleep_for(std::chrono::seconds(cycle_duration - intervals[0].second + intervals[0].first));
        }
        else {
            std::this_thread::sleep_for(std::chrono::seconds(intervals[1].first - intervals[0].second));
            interval_idx++;
        }
    }
    int cycle_count = 0;
    while (run_thread) {
        if (cycle_count == 3) {
            sleep_sec = syncServer(LC_CNT, intervals, cycle_duration, interval_idx, residual_cars);
            if (sleep_sec != -1) {
                if (LC_CNT >= intervals[interval_idx].first && LC_CNT < intervals[interval_idx].second)
                    signalTurnedGreen(prev_on_time, residual_cars);
                else if (LC_CNT >= intervals[interval_idx].second) {
                    signalTurnedGreen(prev_on_time, residual_cars);
                    signalTurnedRed(residual_cars);
                }
                std::this_thread::sleep_for(std::chrono::seconds(sleep_sec));
                if (LC_CNT >= intervals[interval_idx].first && LC_CNT < intervals[interval_idx].second) {
                    signalTurnedRed(residual_cars);
                    if (intervals.size() == 1) {
                        std::this_thread::sleep_for(std::chrono::seconds(cycle_duration - intervals[0].second + intervals[0].first));
                    }
                    else {
                        std::this_thread::sleep_for(std::chrono::seconds(intervals[1].first - intervals[0].second));
                        interval_idx++;
                    }
                }
            }
            cycle_count = 0;
        }
        int on_time = intervals[interval_idx].first;
        int off_time = intervals[interval_idx].second;
        // on-logic
        signalTurnedGreen(prev_on_time, residual_cars);
        // on-logic
        std::this_thread::sleep_for(std::chrono::seconds(off_time - on_time + prev_on_time - getCurTime()));
        // off-logic
        signalTurnedRed(residual_cars);
        // off-logic
        int next_idx = (interval_idx + 1) % intervals.size();
        if (intervals[next_idx].first > off_time)
            std::this_thread::sleep_for(std::chrono::seconds(intervals[next_idx].first - off_time));
        else
            std::this_thread::sleep_for(std::chrono::seconds(cycle_duration - off_time + intervals[next_idx].first));
        interval_idx = next_idx;
        if (interval_idx == 0) {
            cycle_count++;
            current_cycle.fetch_add(1);
        }
    }
}

int ServerReceiver::signalTurnedGreen(int &prev_on_time, std::map<int, int> &residual_cars) {
    
    time_t frame_time = residual_timestamp.load();
    struct tm *tm;
    tm = localtime(&frame_time);
    std::string img_path_nm = "/data/smtints/queue/" + spot_camr_id + "/" +std::to_string(tm->tm_year+1900) + "/" + std::to_string(tm->tm_mon+1) + "/" + std::to_string(tm->tm_mday);
    std::string img_file_nm = std::to_string(frame_time) + ".jpg";

    std::cout << timestamp() << "Signal Turned " << GRN << "GREEN" << RESET << std::endl;
    logger->info("Signal Turned GREEN");
    current_phase.store(true);
    int now = getCurTime();
    std::cout << timestamp() << ": calling getPerLaneDensity() " << std::endl; 
    std::vector<std::map<int, int>> result = getPerLaneDensity();
    std::cout << timestamp() << ": getPerLaneDensity() returned" << std::endl;
    std::map<int, int>& max = result[0];
    std::map<int, int>& min = result[1];
    std::map<int, int>& total = result[2];

    std::map<int ,std::vector<int>> density;
    int total_lane = max.size();
    for (int i=1; i<=total_lane; ++i) {
        density[i].push_back(total[i] / ((now - prev_on_time) * 15));
        density[i].push_back(min[i]);
        density[i].push_back(max[i]);
    }
    std::cout << timestamp() << ": calling fetchStats() " << std::endl; 
    fetchStats(density, 1, now - prev_on_time);
    std::cout << timestamp() << ": fetchStats() returned" << std::endl;
    if (callbackToSendQuery){
        std::cout << timestamp() << RED << "Residual cars per lane" << RESET << std::endl;
        for (const std::pair<const int, int>& pair : residual_cars) {
            int lane = pair.first;
            int count = pair.second;
            std::cout << timestamp() << lane << " : " << count << " " << std::endl;
        }
        std::cout << timestamp() << GRN << "Max cars per lane" << RESET << std::endl;
        for (const std::pair<const int, int>& pair : max) {
            int lane = pair.first;
            int count = pair.second;
            std::cout << timestamp() << lane << " : " << count << " " << std::endl;
        }
        int remain_queue = 0;
        int max_queue = 0;
        for (int lane=1; lane<= total_lane; ++lane) {
            remain_queue += residual_cars[lane];
            max_queue += max[lane];
            // callbackToSendQuery("Lane Queue Query");
            callbackToSendQuery(QueryMaker::laneQueueQuery(spot_camr_id, lane, prev_on_time, now, residual_cars[lane], max[lane], img_path_nm, img_file_nm));
        }
        // callbackToSendQuery("Approach Queue Query");
        callbackToSendQuery(QueryMaker::approachQueueQuery(spot_camr_id, prev_on_time, now, remain_queue, max_queue, img_path_nm, img_file_nm));     // waiting cars per lane data
    }
    prev_on_time = now;
    return 0;
}

int ServerReceiver::signalTurnedRed(std::map<int, int> &residual_cars) {
    waiting_image_save.store(true);
    std::cout << timestamp() << "Signal Turned " << RED << "RED" << RESET << std::endl;
    logger->info("Signal Turned RED");
    current_phase.store(false);
    residual_cars = getPerLaneCount();
    std::cout << timestamp() << RED << "Residual cars per lane" << RESET << std::endl;
    for (const std::pair<const int, int>& pair : residual_cars) {
        int lane = pair.first;
        int count = pair.second;
        std::cout << timestamp() << lane << " :" << count << " " << std::endl;
    }
    return 0;
}

std::string ServerReceiver::getIpAddress() {
    struct ifreq ifr;
    struct sockaddr_in* ipaddr;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return "0.0.0.0";

    strncpy(ifr.ifr_name, "eth0", IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFADDR, &ifr) < 0) {
        close(s);
        return "0.0.0.0";
    }

    ipaddr = (struct sockaddr_in*)&ifr.ifr_addr;
    static char ipstr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ipaddr->sin_addr, ipstr, INET_ADDRSTRLEN);
    close(s);
    return std::string(ipstr);
}