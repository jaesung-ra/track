#include "car_presence.h"
#include "../../data/redis/redis_client.h"
#include "../../data/redis/channel_types.h"
#include "../../roi_module/roi_handler.h"
#include "../../utils/config_manager.h"
#include <algorithm>
#include <sstream>

/**
 * @brief 생성자
 */
CarPresence::CarPresence(ROIHandler& roi, RedisClient& redis)
    : roi_handler_(roi)
    , redis_client_(redis) {
    
    // 로거 초기화
    logger = getLogger("DS_CarPresence");
    logger->info("차량 Presence 모듈 생성");
}

/**
 * @brief 소멸자
 */
CarPresence::~CarPresence() {
    if (logger && initialized_) {
        logStatistics();
        logger->info("차량 Presence 모듈 종료");
    }
}

/**
 * @brief 초기화 함수
 */
bool CarPresence::initialize() {
    try {
        auto& config = ConfigManager::getInstance();
        
        // ConfigManager의 캐싱된 메서드 사용
        config_.enabled = config.isVehiclePresenceEnabled();
        
        if (!config_.enabled) {
            logger->info("차량 Presence 체크 비활성화됨");
            return false;
        }
        
        // 캐싱된 설정값 사용
        config_.detect_frames = config.getVehiclePresenceDetectFrames();
        config_.absence_frames = config.getVehiclePresenceAbsenceFrames();
        config_.anti_flicker = config.getVehiclePresenceAntiFlicker();
        
        // Anti-flicker 기본값 설정
        config_.max_toggles_per_sec = 2;
        config_.min_stable_ms = 300;
        
        enabled_ = config_.enabled;
        
        // 차선 ROI 체크
        if (roi_handler_.lane_roi.empty()) {
            logger->error("차선 ROI 없음 - 차량 Presence 비활성화");
            enabled_ = false;
            return false;
        }
        
        // 통계 시작 시간 기록
        stats_.start_time = std::chrono::steady_clock::now();
        flicker_.last_change_time = stats_.start_time;
        
        initialized_ = true;
        
        logger->info("차량 Presence 초기화 완료:");
        logger->info("  - 차선 ROI 수: {}", roi_handler_.lane_roi.size());
        logger->info("  - 검출 프레임: {}", config_.detect_frames);
        logger->info("  - 미검출 프레임: {}", config_.absence_frames);
        logger->info("  - Anti-flicker: {}", config_.anti_flicker ? "활성" : "비활성");
        if (config_.anti_flicker) {
            logger->info("    - 초당 최대 토글: {}회", config_.max_toggles_per_sec);
            logger->info("    - 최소 안정 시간: {}ms", config_.min_stable_ms);
        }
        
        return true;
        
    } catch (const std::exception& e) {
        logger->error("차량 Presence 초기화 실패: {}", e.what());
        enabled_ = false;
        return false;
    }
}

/**
 * @brief 차량 업데이트 - 매 프레임 호출
 */
void CarPresence::updateVehicles(const std::map<int, ObjPoint>& vehicle_positions, int current_time) {
    if (!enabled_ || !initialized_) return;
    
    try {
        // 차선별 차량 수 계산
        lane_vehicle_count_.clear();
        bool has_vehicles = false;
        
        for (const auto& [id, pos] : vehicle_positions) {
            int lane = roi_handler_.getLaneNum(pos);
            if (lane > 0) {
                lane_vehicle_count_[lane]++;
                has_vehicles = true;
            }
        }
        
        // 상태 전이 처리
        processStateTransition(has_vehicles, current_time);
        
        // 주기적 통계 출력 (5분마다)
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_stats_log_time_).count();
        
        if (elapsed >= STATS_LOG_INTERVAL_SEC) {
            logStatistics();
            last_stats_log_time_ = now;
        }
        
    } catch (const std::exception& e) {
        logger->error("차량 업데이트 중 오류: {}", e.what());
    }
}

/**
 * @brief 상태 전이 처리
 */
void CarPresence::processStateTransition(bool has_vehicles, int current_time) {
    bool prev_state = current_state_;
    
    if (has_vehicles) {
        // 차량 검출됨
        absence_counter_ = 0;
        detection_counter_++;
        
        // 검출 임계값 도달
        if (!current_state_ && detection_counter_ >= config_.detect_frames) {
            // 0 -> 1 전이 시도
            if (!config_.anti_flicker || checkAntiFlicker(current_time)) {
                current_state_ = true;
                detection_counter_ = 0;
                
                sendPresenceState(1, current_time);
                stats_.total_state_changes++;
                
                logger->debug("차량 진입 감지 - presence: 0 -> 1");
                
                // 차선별 상세 정보 (디버깅)
                if (logger->level() <= spdlog::level::debug) {
                    std::stringstream ss;
                    for (const auto& [lane, count] : lane_vehicle_count_) {
                        ss << " [차선" << lane << ":" << count << "대]";
                    }
                    logger->debug("차선별 차량:{}", ss.str());
                }
            } else {
                logger->debug("Anti-flicker: 차량 진입 신호 억제");
                stats_.flicker_prevented++;
            }
        }
    } else {
        // 차량 미검출
        detection_counter_ = 0;
        absence_counter_++;
        
        // 미검출 임계값 도달
        if (current_state_ && absence_counter_ >= config_.absence_frames) {
            // 1 -> 0 전이 시도
            if (!config_.anti_flicker || checkAntiFlicker(current_time)) {
                current_state_ = false;
                absence_counter_ = 0;
                
                sendPresenceState(0, current_time);
                stats_.total_state_changes++;
                
                logger->debug("차량 이탈 감지 - presence: 1 -> 0");
            } else {
                logger->debug("Anti-flicker: 차량 이탈 신호 억제");
                stats_.flicker_prevented++;
            }
        }
    }
}

/**
 * @brief Anti-flicker 체크
 */
bool CarPresence::checkAntiFlicker(int current_time) {
    if (!config_.anti_flicker) return true;
    
    auto now = std::chrono::steady_clock::now();
    
    // 최소 안정 시간 체크
    auto time_since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - flicker_.last_change_time).count();
    
    if (time_since_last < config_.min_stable_ms) {
        logger->trace("Anti-flicker: {}ms < {}ms (최소 안정 시간)", 
                     time_since_last, config_.min_stable_ms);
        return false;
    }
    
    // 초당 토글 횟수 체크
    // 1초 이내의 토글만 유지
    flicker_.recent_toggles.erase(
        std::remove_if(flicker_.recent_toggles.begin(), flicker_.recent_toggles.end(),
            [now](const auto& time_point) {
                return std::chrono::duration_cast<std::chrono::seconds>(
                    now - time_point).count() >= 1;
            }),
        flicker_.recent_toggles.end()
    );
    
    if (flicker_.recent_toggles.size() >= static_cast<size_t>(config_.max_toggles_per_sec)) {
        logger->trace("Anti-flicker: 초당 토글 횟수 초과 ({}/{})", 
                     flicker_.recent_toggles.size(), config_.max_toggles_per_sec);
        return false;
    }
    
    // 상태 변경 허용
    flicker_.last_change_time = now;
    flicker_.recent_toggles.push_back(now);
    
    return true;
}

/**
 * @brief Redis로 상태 전송
 */
void CarPresence::sendPresenceState(int state, int current_time) {
    try {
        // 단순 문자열 형태로 전송 ("0" 또는 "1")
        std::string data = std::to_string(state);
        
        int result = redis_client_.sendData(CHANNEL_VEHICLE_PRESENCE, data);
        
        if (result == 0) {
            stats_.messages_sent++;
            logger->info("차량 Presence 상태 전송: {} (시간: {})", state, current_time);
        } else {
            logger->error("Redis 전송 실패 - 채널: presence:vehicle, 상태: {}", state);
        }
        
    } catch (const std::exception& e) {
        logger->error("Redis 전송 중 오류: {}", e.what());
    }
}

/**
 * @brief 통계 정보 로깅
 */
void CarPresence::logStatistics() const {
    if (!initialized_) return;
    
    auto now = std::chrono::steady_clock::now();
    auto runtime = std::chrono::duration_cast<std::chrono::seconds>(
        now - stats_.start_time).count();
    
    logger->info("=== 차량 Presence 통계 ===");
    logger->info("  실행 시간: {}초", runtime);
    logger->info("  총 상태 변경: {}회", stats_.total_state_changes);
    logger->info("  Anti-flicker 차단: {}회", stats_.flicker_prevented);
    logger->info("  Redis 전송: {}회", stats_.messages_sent);
    logger->info("  현재 상태: {}", current_state_ ? "차량 있음" : "차량 없음");
}