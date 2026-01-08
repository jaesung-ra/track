#include "pedestrian_presence.h"
#include "../../data/redis/channel_types.h"
#include "../../data/redis/redis_client.h"
#include "../../roi_module/roi_handler.h"
#include "../../roi_module/roi_utils.h"
#include "../../utils/config_manager.h"
#include <algorithm>
#include <sstream>

/**
 * @brief 생성자
 */
PedestrianPresence::PedestrianPresence(ROIHandler& roi, RedisClient& redis)
    : roi_handler_(roi)
    , redis_client_(redis) {
    
    // 로거 초기화
    logger = getLogger("DS_PedestrianPresence");
    logger->info("보행자 Presence 모듈 생성");
}

/**
 * @brief 소멸자
 */
PedestrianPresence::~PedestrianPresence() {
    if (logger && initialized_) {
        logStatistics();
        logger->info("보행자 Presence 모듈 종료");
    }
}

/**
 * @brief 초기화 함수
 */
bool PedestrianPresence::initialize() {
    try {
        auto& config = ConfigManager::getInstance();
        
        // ConfigManager의 캐싱된 메서드 사용
        config_.enabled = config.isPedestrianPresenceEnabled();
        
        if (!config_.enabled) {
            logger->info("보행자 Presence 체크 비활성화됨");
            return false;
        }
        
        // 캐싱된 설정값 사용 - 횡단보도와 대기구역 모두 공통 적용
        config_.detect_frames = config.getPedestrianPresenceDetectFrames();
        config_.absence_frames = config.getPedestrianPresenceAbsenceFrames();
        config_.anti_flicker = config.getPedestrianPresenceAntiFlicker();
        
        // Anti-flicker 기본값 설정
        config_.max_toggles_per_sec = 3;
        config_.min_stable_ms = 200;
        
        enabled_ = config_.enabled;
        
        // ROI 체크
        crosswalk_enabled_ = !roi_handler_.crosswalk_roi.empty();
        waiting_enabled_ = !roi_handler_.waiting_area_roi.empty();
        
        if (!crosswalk_enabled_ && !waiting_enabled_) {
            logger->error("횡단보도/대기구역 ROI 모두 없음 - 보행자 Presence 비활성화");
            enabled_ = false;
            return false;
        }
        
        // 통계 시작 시간 기록
        global_stats_.start_time = std::chrono::steady_clock::now();
        
        // 구역별 초기화
        auto now = global_stats_.start_time;
        crosswalk_state_.last_change_time = now;
        waiting_state_.last_change_time = now;
        
        initialized_ = true;
        
        logger->info("보행자 Presence 초기화 완료:");
        logger->info("  - 횡단보도 ROI: {}", crosswalk_enabled_ ? "활성" : "비활성");
        if (crosswalk_enabled_) {
            logger->info("    - ROI 좌표 수: {}", roi_handler_.crosswalk_roi.size());
        }
        logger->info("  - 대기구역 ROI: {}", waiting_enabled_ ? "활성" : "비활성");
        if (waiting_enabled_) {
            logger->info("    - ROI 좌표 수: {}", roi_handler_.waiting_area_roi.size());
        }
        logger->info("  - 검출 프레임: {}", config_.detect_frames);
        logger->info("  - 미검출 프레임: {}", config_.absence_frames);
        logger->info("  - Anti-flicker: {}", config_.anti_flicker ? "활성" : "비활성");
        if (config_.anti_flicker) {
            logger->info("    - 초당 최대 토글: {}회", config_.max_toggles_per_sec);
            logger->info("    - 최소 안정 시간: {}ms", config_.min_stable_ms);
        }
        
        return true;
        
    } catch (const std::exception& e) {
        logger->error("보행자 Presence 초기화 실패: {}", e.what());
        enabled_ = false;
        return false;
    }
}

/**
 * @brief 보행자 업데이트 - 매 프레임 호출
 */
void PedestrianPresence::updatePedestrians(const std::map<int, ObjPoint>& pedestrian_positions, 
                                          int current_time) {
    if (!enabled_ || !initialized_) return;
    
    try {
        global_stats_.total_frames_processed++;
        
        // 횡단보도 체크
        if (crosswalk_enabled_) {
            int crosswalk_count = 0;
            bool has_crosswalk_peds = false;
            
            for (const auto& [id, pos] : pedestrian_positions) {
                if (isInCrosswalk(pos)) {
                    crosswalk_count++;
                    has_crosswalk_peds = true;
                }
            }
            
            crosswalk_state_.pedestrian_count = crosswalk_count;
            
            // 공통 설정 적용
            processAreaTransition(crosswalk_state_, has_crosswalk_peds, 
                                CHANNEL_PED_CROSSING, "횡단보도", current_time);
        }
        
        // 대기구역 체크
        if (waiting_enabled_) {
            int waiting_count = 0;
            bool has_waiting_peds = false;
            
            for (const auto& [id, pos] : pedestrian_positions) {
                if (isInWaitingArea(pos)) {
                    waiting_count++;
                    has_waiting_peds = true;
                }
            }
            
            waiting_state_.pedestrian_count = waiting_count;
            
            // 공통 설정 적용
            processAreaTransition(waiting_state_, has_waiting_peds, 
                                CHANNEL_PED_WAITING, "대기구역", current_time);
        }
        
        // 주기적 통계 출력 (5분마다)
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_stats_log_time_).count();
        
        if (elapsed >= STATS_LOG_INTERVAL_SEC) {
            logStatistics();
            last_stats_log_time_ = now;
        }
        
    } catch (const std::exception& e) {
        logger->error("보행자 업데이트 중 오류: {}", e.what());
    }
}

/**
 * @brief 구역별 상태 전이 처리
 */
void PedestrianPresence::processAreaTransition(AreaState& state, bool has_pedestrians,
                                              int channel_type, const std::string& area_name,
                                              int current_time) {
    if (has_pedestrians) {
        // 보행자 검출됨
        state.absence_counter = 0;
        state.detection_counter++;
        
        // 검출 임계값 도달
        if (!state.current && state.detection_counter >= config_.detect_frames) {
            // 0 -> 1 전이 시도
            if (!config_.anti_flicker || checkAntiFlicker(state, current_time)) {
                state.current = true;
                state.detection_counter = 0;
                
                sendPresenceState(channel_type, 1, area_name, current_time);
                state.total_changes++;
                state.messages_sent++;
                
                logger->debug("{} 보행자 진입 - presence: 0 -> 1 ({}명)", 
                            area_name, state.pedestrian_count);
            } else {
                logger->trace("Anti-flicker: {} 진입 신호 억제", area_name);
                state.flicker_prevented++;
            }
        }
    } else {
        // 보행자 미검출
        state.detection_counter = 0;
        state.absence_counter++;
        state.pedestrian_count = 0;
        
        // 미검출 임계값 도달
        if (state.current && state.absence_counter >= config_.absence_frames) {
            // 1 -> 0 전이 시도
            if (!config_.anti_flicker || checkAntiFlicker(state, current_time)) {
                state.current = false;
                state.absence_counter = 0;
                
                sendPresenceState(channel_type, 0, area_name, current_time);
                state.total_changes++;
                state.messages_sent++;
                
                logger->debug("{} 보행자 이탈 - presence: 1 -> 0", area_name);
            } else {
                logger->trace("Anti-flicker: {} 이탈 신호 억제", area_name);
                state.flicker_prevented++;
            }
        }
    }
}

/**
 * @brief Anti-flicker 체크
 */
bool PedestrianPresence::checkAntiFlicker(AreaState& state, int current_time) {
    if (!config_.anti_flicker) return true;
    
    auto now = std::chrono::steady_clock::now();
    
    // 최소 안정 시간 체크
    auto time_since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state.last_change_time).count();
    
    if (time_since_last < config_.min_stable_ms) {
        return false;
    }
    
    // 초당 토글 횟수 체크
    state.recent_toggles.erase(
        std::remove_if(state.recent_toggles.begin(), state.recent_toggles.end(),
            [now](const auto& time_point) {
                return std::chrono::duration_cast<std::chrono::seconds>(
                    now - time_point).count() >= 1;
            }),
        state.recent_toggles.end()
    );
    
    if (state.recent_toggles.size() >= static_cast<size_t>(config_.max_toggles_per_sec)) {
        return false;
    }
    
    // 상태 변경 허용
    state.last_change_time = now;
    state.recent_toggles.push_back(now);
    
    return true;
}

/**
 * @brief Redis로 상태 전송
 */
void PedestrianPresence::sendPresenceState(int channel_type, int state_value,
                                          const std::string& area_name, int current_time) {
    try {
        // 단순 문자열 형태로 전송 ("0" 또는 "1")
        std::string data = std::to_string(state_value);
        
        int result = redis_client_.sendData(channel_type, data);
        
        if (result == 0) {
            logger->info("{} Presence 상태 전송: {} (시간: {})", 
                        area_name, state_value, current_time);
        } else {
            logger->error("Redis 전송 실패 - 구역: {}, 상태: {}", area_name, state_value);
        }
        
    } catch (const std::exception& e) {
        logger->error("Redis 전송 중 오류: {}", e.what());
    }
}

/**
 * @brief 횡단보도 내 보행자 체크
 */
bool PedestrianPresence::isInCrosswalk(const ObjPoint& position) {
    return roi_handler_.isInCrossWalk(position);
}

/**
 * @brief 대기구역 내 보행자 체크
 */
bool PedestrianPresence::isInWaitingArea(const ObjPoint& position) {
    return roi_handler_.isInWaitingArea(position);
}

/**
 * @brief 통계 정보 로깅
 */
void PedestrianPresence::logStatistics() const {
    if (!initialized_) return;
    
    auto now = std::chrono::steady_clock::now();
    auto runtime = std::chrono::duration_cast<std::chrono::seconds>(
        now - global_stats_.start_time).count();
    
    logger->info("=== 보행자 Presence 통계 ===");
    logger->info("  실행 시간: {}초", runtime);
    logger->info("  처리 프레임: {}", global_stats_.total_frames_processed);
    
    if (crosswalk_enabled_) {
        logger->info("  [횡단보도]");
        logger->info("    - 상태 변경: {}회", crosswalk_state_.total_changes);
        logger->info("    - Anti-flicker 차단: {}회", crosswalk_state_.flicker_prevented);
        logger->info("    - Redis 전송: {}회", crosswalk_state_.messages_sent);
        logger->info("    - 현재 상태: {} ({}명)", 
                    crosswalk_state_.current ? "보행자 있음" : "보행자 없음",
                    crosswalk_state_.pedestrian_count);
    }
    
    if (waiting_enabled_) {
        logger->info("  [대기구역]");
        logger->info("    - 상태 변경: {}회", waiting_state_.total_changes);
        logger->info("    - Anti-flicker 차단: {}회", waiting_state_.flicker_prevented);
        logger->info("    - Redis 전송: {}회", waiting_state_.messages_sent);
        logger->info("    - 현재 상태: {} ({}명)", 
                    waiting_state_.current ? "보행자 있음" : "보행자 없음",
                    waiting_state_.pedestrian_count);
    }
}