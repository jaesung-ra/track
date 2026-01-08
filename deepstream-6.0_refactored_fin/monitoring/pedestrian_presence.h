#ifndef PEDESTRIAN_PRESENCE_H
#define PEDESTRIAN_PRESENCE_H

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include "../../common/common_types.h"
#include "../../common/object_data.h"

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

// Forward declarations
class ROIHandler;
class RedisClient;

/**
 * @brief 보행자 존재 여부 체크 클래스
 * 
 * 횡단보도와 대기구역의 보행자 존재 여부를 감지하고 상태 변화시 Redis로 전송
 * 두 개의 독립적인 채널로 관리:
 * - presence:person:crosswalk (횡단보도)
 * - presence:person:waiting_area (대기구역)
 */
class PedestrianPresence {
public:
    /**
     * @brief 생성자
     * @param roi ROI 핸들러 참조
     * @param redis Redis 클라이언트 참조
     */
    PedestrianPresence(ROIHandler& roi, RedisClient& redis);
    
    /**
     * @brief 소멸자
     */
    ~PedestrianPresence();
    
    /**
     * @brief 초기화 함수
     * @return 성공시 true
     */
    bool initialize();
    
    /**
     * @brief 보행자 업데이트 - 매 프레임 호출
     * @param pedestrian_positions 현재 프레임의 보행자 위치 맵 (id -> position)
     * @param current_time 현재 시간 (Unix timestamp)
     */
    void updatePedestrians(const std::map<int, ObjPoint>& pedestrian_positions, int current_time);
    
    /**
     * @brief 횡단보도 presence 상태 조회
     * @return 보행자 존재시 true
     */
    bool isCrosswalkPresent() const { return crosswalk_state_.current; }
    
    /**
     * @brief 대기구역 presence 상태 조회
     * @return 보행자 존재시 true
     */
    bool isWaitingAreaPresent() const { return waiting_state_.current; }
    
    /**
     * @brief 활성화 상태 확인
     * @return 모듈 활성화시 true
     */
    bool isEnabled() const { return enabled_; }
    
    /**
     * @brief 통계 정보 로깅
     */
    void logStatistics() const;
    
private:
    /**
     * @brief 구역별 상태 관리 구조체
     */
    struct AreaState {
        bool current = false;           // 현재 presence 상태
        int detection_counter = 0;      // 연속 검출 카운터
        int absence_counter = 0;        // 연속 미검출 카운터
        int pedestrian_count = 0;       // 현재 보행자 수
        
        // Anti-flicker 관리
        std::chrono::steady_clock::time_point last_change_time;
        std::vector<std::chrono::steady_clock::time_point> recent_toggles;
        
        // 통계
        int total_changes = 0;          // 총 상태 변경 횟수
        int flicker_prevented = 0;      // Anti-flicker로 방지된 횟수
        int messages_sent = 0;          // Redis 전송 횟수
    };
    
    /**
     * @brief 구역별 상태 전이 처리
     * @param state 구역 상태 참조
     * @param has_pedestrians 현재 프레임에 보행자 존재 여부
     * @param channel_type Redis 채널 타입
     * @param area_name 구역 이름 (로깅용)
     * @param current_time 현재 시간
     */
    void processAreaTransition(AreaState& state, bool has_pedestrians, 
                               int channel_type, const std::string& area_name, 
                               int current_time);
    
    /**
     * @brief Anti-flicker 체크
     * @param state 구역 상태 참조
     * @param current_time 현재 시간
     * @return 상태 변경 허용시 true
     */
    bool checkAntiFlicker(AreaState& state, int current_time);
    
    /**
     * @brief Redis로 상태 전송
     * @param channel_type 채널 타입
     * @param state_value 전송할 상태 (0 또는 1)
     * @param area_name 구역 이름 (로깅용)
     * @param current_time 현재 시간
     */
    void sendPresenceState(int channel_type, int state_value, 
                          const std::string& area_name, int current_time);
    
    /**
     * @brief 횡단보도 내 보행자 체크
     * @param position 보행자 위치
     * @return 횡단보도 내부시 true
     */
    bool isInCrosswalk(const ObjPoint& position);
    
    /**
     * @brief 대기구역 내 보행자 체크
     * @param position 보행자 위치
     * @return 대기구역 내부시 true
     */
    bool isInWaitingArea(const ObjPoint& position);
    
    // 의존성
    ROIHandler& roi_handler_;
    RedisClient& redis_client_;
    
    // 로거
    std::shared_ptr<spdlog::logger> logger;
    
    // 설정값 (config.json에서 로드) - 횡단보도와 대기구역 공통 적용
    struct Config {
        bool enabled = false;           // 전체 활성화 여부
        int detect_frames = 1;          // 검출 필요 프레임 수
        int absence_frames = 3;         // 미검출 필요 프레임 수
        bool anti_flicker = true;       // Anti-flicker 활성화
        int max_toggles_per_sec = 2;   // 초당 최대 토글 횟수
        int min_stable_ms = 300;       // 최소 안정 시간 (ms)
    } config_;
    
    // 구역별 상태
    AreaState crosswalk_state_;        // 횡단보도 상태
    AreaState waiting_state_;          // 대기구역 상태
    
    // 전체 통계
    struct GlobalStats {
        std::chrono::steady_clock::time_point start_time;
        int total_frames_processed = 0;
    } global_stats_;
    
    // 활성화 상태
    bool enabled_ = false;
    bool initialized_ = false;
    bool crosswalk_enabled_ = false;   // 횡단보도 ROI 존재 여부
    bool waiting_enabled_ = false;     // 대기구역 ROI 존재 여부
    
    // 주기적 통계 출력용
    std::chrono::steady_clock::time_point last_stats_log_time_;
    static constexpr int STATS_LOG_INTERVAL_SEC = 300;  // 5분
};

#endif // PEDESTRIAN_PRESENCE_H