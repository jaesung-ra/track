#ifndef CHANNEL_TYPES_H
#define CHANNEL_TYPES_H

#include <string>
#include "../../utils/config_manager.h"

/**
 * @brief Redis 채널 타입 열거형
 * 
 * 로컬 Redis의 8개 채널을 정의
 */
enum ChannelType {
    CHANNEL_VEHICLE_2K = 0,         // detection:vehicle:2k
    CHANNEL_VEHICLE_4K = 1,         // detection:vehicle:4k
    CHANNEL_PEDESTRIAN = 2,         // detection:person
    CHANNEL_STATS = 3,              // statistics
    CHANNEL_QUEUE = 4,              // wait_queue
    CHANNEL_INCIDENT = 5,           // incident_event
    CHANNEL_VEHICLE_PRESENCE = 6,   // presence:vehicle
    CHANNEL_PED_WAITING = 7,        // presence:person:waiting_area
    CHANNEL_PED_CROSSING = 8        // presence:person:crosswalk
};

/**
 * @brief 채널 타입을 채널명으로 변환
 * @param type 채널 타입
 * @return 채널명 문자열
 */
inline std::string getChannelName(int type) {
    auto& config = ConfigManager::getInstance();
    
    switch (type) {
        case CHANNEL_VEHICLE_2K:    
            return config.getRedisChannel("vehicle_2k");
        case CHANNEL_VEHICLE_4K:    
            return config.getRedisChannel("vehicle_4k");
        case CHANNEL_PEDESTRIAN:    
            return config.getRedisChannel("pedestrian");
        case CHANNEL_STATS: 
            return config.getRedisChannel("stats");
        case CHANNEL_QUEUE:   
            return config.getRedisChannel("queue");
        case CHANNEL_INCIDENT:       
            return config.getRedisChannel("incident");
        case CHANNEL_VEHICLE_PRESENCE:       
            return config.getRedisChannel("vehicle_presence");
        case CHANNEL_PED_WAITING:    
            return config.getRedisChannel("ped_waiting");
        case CHANNEL_PED_CROSSING:   
            return config.getRedisChannel("ped_crossing");
        default:                     
            return "unknown_channel";
    }
}

/**
 * @brief 채널명을 채널 타입으로 변환
 * @param name 채널명
 * @return 채널 타입 (-1: 알 수 없는 채널)
 */
inline int getChannelType(const std::string& name) {
    auto& config = ConfigManager::getInstance();
    
    if (name == config.getRedisChannel("vehicle_2k")) return CHANNEL_VEHICLE_2K;
    if (name == config.getRedisChannel("vehicle_4k")) return CHANNEL_VEHICLE_4K;
    if (name == config.getRedisChannel("pedestrian")) return CHANNEL_PEDESTRIAN;
    if (name == config.getRedisChannel("stats")) return CHANNEL_STATS;
    if (name == config.getRedisChannel("queue")) return CHANNEL_QUEUE;
    if (name == config.getRedisChannel("incident")) return CHANNEL_INCIDENT;
    if (name == config.getRedisChannel("vehicle_presence")) return CHANNEL_VEHICLE_PRESENCE;
    if (name == config.getRedisChannel("ped_waiting")) return CHANNEL_PED_WAITING;
    if (name == config.getRedisChannel("ped_crossing")) return CHANNEL_PED_CROSSING;
    return -1;
}

#endif // CHANNEL_TYPES_H