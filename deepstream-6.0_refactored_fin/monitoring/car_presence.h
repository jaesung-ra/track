#ifndef CAR_PRESENCE_H
#define CAR_PRESENCE_H

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
 * @brief 차량 존재 여부 체크 클래스
 * 
 * 차선 ROI 내 차량 존재 여부를 감지하고 상태 변화시 Redis로 전송
 * 하이브리드 적응형 전략 사용:
 * - 빠른 검출 (detect_frames)
 * - 안정적 해제 (absence_frames)
 * - Anti-flicker 지원
 */
class CarPresence {
public:
    /**
     * @brief 생성자
     * @param roi ROI 핸들러 참조
     * @param redis Redis 클라이언트 참조
     */
    CarPresence(ROIHandler& roi, RedisClient& redis);
    
    /**
     * @brief 소멸자
     */
    ~CarPresence();
    
    /**
     * @brief 초기화 함수
     * @return 성공시 true
     */
    bool initialize();
    
    /**
     * @brief 차량 업데이트 - 매 프레임 호출
     * @param vehicle_positions 현재 프레임의 차량 위치 맵 (id -> position)
     * @param current_time 현재 시간 (Unix timestamp)
     */
    void updateVehicles(const std::map<int, ObjPoint>& vehicle_positions, int current_time);
    
    /**
     * @brief 현재 presence 상태 조회
     * @return 차량 존재시 true
     */
    bool isPresent() const { return current_state_; }
    
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
     * @brief 상태 전이 처리
     * @param has_vehicles 현재 프레임에 차량 존재 여부
     * @param current_time 현재 시간
     */
    void processStateTransition(bool has_vehicles, int current_time);
    
    /**
     * @brief Anti-flicker 체크
     * @param current_time 현재 시간
     * @return 상태 변경 허용시 true
     */
    bool checkAntiFlicker(int current_time);
    
    /**
     * @brief Redis로 상태 전송
     * @param state 전송할 상태 (0 또는 1)
     * @param current_time 현재 시간
     */
    void sendPresenceState(int state, int current_time);
    
    // 의존성
    ROIHandler& roi_handler_;
    RedisClient& redis_client_;
    
    // 로거
    std::shared_ptr<spdlog::logger> logger;
    
    // 설정값 (config.json에서 로드)
    struct Config {
        bool enabled = false;           // 활성화 여부
        int detect_frames = 1;          // 검출 필요 프레임 수
        int absence_frames = 3;         // 미검출 필요 프레임 수  
        bool anti_flicker = true;       // Anti-flicker 활성화
        int max_toggles_per_sec = 2;   // 초당 최대 토글 횟수
        int min_stable_ms = 300;       // 최소 안정 시간 (ms)
    } config_;
    
    // 상태 관리
    bool current_state_ = false;        // 현재 presence 상태
    int detection_counter_ = 0;         // 연속 검출 카운터
    int absence_counter_ = 0;           // 연속 미검출 카운터
    
    // Anti-flicker 관리
    struct FlickerControl {
        std::chrono::steady_clock::time_point last_change_time;
        std::vector<std::chrono::steady_clock::time_point> recent_toggles;
        int toggle_count = 0;
    } flicker_;
    
    // 통계
    struct Statistics {
        int total_state_changes = 0;    // 총 상태 변경 횟수
        int flicker_prevented = 0;      // Anti-flicker로 방지된 횟수
        int messages_sent = 0;          // Redis 전송 횟수
        std::chrono::steady_clock::time_point start_time;
    } stats_;
    
    // 활성화 상태
    bool enabled_ = false;
    bool initialized_ = false;
    
    // 차선별 차량 수 추적 (디버깅용)
    std::map<int, int> lane_vehicle_count_;
    
    // 주기적 통계 출력용
    std::chrono::steady_clock::time_point last_stats_log_time_;
    static constexpr int STATS_LOG_INTERVAL_SEC = 300;  // 5분
};

#endif // CAR_PRESENCE_H