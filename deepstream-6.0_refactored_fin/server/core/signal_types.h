#ifndef SIGNAL_TYPES_H
#define SIGNAL_TYPES_H

#include <map>
#include <string>
#include <vector>

/**
 * @brief 신호 변경 이벤트
 */
struct SignalChangeEvent {
    enum class Type {
        GREEN_ON,                      // 녹색 신호 켜짐
        GREEN_OFF,                     // 녹색 신호 꺼짐 (적색 신호 켜짐)
        CYCLE_COMPLETE
    };
    
    Type type;
    int timestamp;
    int phase;
    int duration_seconds;              // 신호 지속 시간
    std::map<int, int> residual_cars;  // 차로별 잔여 차량 수
};

/**
 * @brief 신호 정보
 */
struct SignalInfo {
    std::string intersection_id;
    int target_signal;
    std::vector<std::pair<int, int>> green_intervals;  // (시작, 종료) 시간
    int cycle_duration;
    bool is_valid = false;
};

#endif