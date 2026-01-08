/*
 * special_site_adapter.cpp
 * 
 * Special Site 모드 처리를 위한 어댑터 클래스 구현
 */

#include "special_site_adapter.h"
#include "../../roi_module/roi_handler.h"
#include "../../server/signal/signal_calculator.h"

SpecialSiteAdapter::SpecialSiteAdapter(SignalCalculator* signal_calc, ROIHandler* roi_handler)
    : signal_calculator_(signal_calc), roi_handler_(roi_handler) {
    
    logger = getLogger("DS_SpecialSite_log");
    logger->info("SpecialSiteAdapter 생성");
}

bool SpecialSiteAdapter::initialize() {
    std::lock_guard<std::mutex> lock(config_mutex_);
    
    try {
        auto& config_mgr = ConfigManager::getInstance();
        
        // ConfigManager의 캐시된 값 사용
        config_.enabled = config_mgr.isSpecialSiteEnabled();
        config_.straight_left = config_mgr.isSpecialSiteStraightLeft();
        config_.right = config_mgr.isSpecialSiteRight();
        
        logger->info("Special Site 설정 로드:");
        logger->info("  - enabled: {}", config_.enabled);
        logger->info("  - straight_left: {}", config_.straight_left);
        logger->info("  - right: {}", config_.right);
        
        // 2K 모드 확인
        bool is_2k_enabled = config_mgr.isVehicle2KEnabled();
        bool is_4k_enabled = config_mgr.isVehicle4KEnabled();
        
        logger->info("카메라 모드: 2K={}, 4K={}", is_2k_enabled, is_4k_enabled);
        
        // Special Site는 2K 모드에서만 유효
        if (config_.enabled) {
            if (!is_2k_enabled || is_4k_enabled) {
                logger->warn("Special Site 모드는 2K 전용 모드에서만 동작 (2K=true, 4K=false)");
                logger->warn("현재 설정: 2K={}, 4K={} - Special Site 비활성화", 
                           is_2k_enabled, is_4k_enabled);
                config_.enabled = false;
                is_active_ = false;
            } else {
                is_active_ = true;
                logger->info("========================================");
                logger->info("Special Site 모드 활성화됨");
                logger->info("  - 처리 모드: {}", 
                           config_.straight_left ? "직진/좌회전" : "우회전");
                logger->info("  - 신호 판단: 타겟신호 ON=직진, OFF=좌회전");
                logger->info("  - SQLite 저장: 비활성화");
                logger->info("  - 통계 생성: 자동 비활성화");
                logger->info("  - 대기행렬 분석: 자동 비활성화");
                logger->info("========================================");
                
                // ROI Handler 확인
                if (!roi_handler_) {
                    logger->error("ROI Handler 없음. 모든 차량이 신호 기반으로 처리됨");
                } else {
                    logger->info("ROI Handler 연결됨");
                }
                
                // 신호 계산기 확인
                if (!signal_calculator_) {
                    logger->warn("SignalCalculator가 없음 - 신호 기반 방향 결정시 기본값(직진) 사용");
                    logger->warn("ROI 기반 방향 결정만 가능");
                } else {
                    logger->info("SignalCalculator 연결됨");
                    logger->info("  - 현재 타겟 신호: {}", 
                                signal_calculator_->isGreenSignal() ? "ON(직진)" : "OFF(좌회전)");
                }
            }
        } else {
            is_active_ = false;
            logger->info("Special Site 모드 비활성화 (config.enabled=false)");
        }
        
        return true;
        
    } catch (const std::exception& e) {
        logger->error("Special Site 초기화 실패: {}", e.what());
        is_active_ = false;
        return false;
    }
}

void SpecialSiteAdapter::setSignalCalculator(SignalCalculator* signal_calc) {
    signal_calculator_ = signal_calc;
    
    if (signal_calc) {
        logger->info("SignalCalculator 연결됨");
        logger->info("  - 현재 타겟 신호: {}", 
                    signal_calc->isGreenSignal() ? "ON(직진)" : "OFF(좌회전)");
    } else {
        logger->warn("SignalCalculator 연결 해제됨");
    }
}

int SpecialSiteAdapter::determineDirectionBySignal() const {
    if (!signal_calculator_) {
        logger->debug("SignalCalculator 없음 - 기본값(직진) 반환");
        return 11;  // 기본값: 직진
    }
    
    // 타겟신호 ON=직진(11), OFF=좌회전(21)
    int direction = signal_calculator_->getDirectionForSpecialSite();
    
    logger->trace("신호 기반 방향 결정: {} (타겟신호: {})", 
                 direction == 11 ? "직진" : "좌회전",
                 signal_calculator_->isGreenSignal() ? "ON" : "OFF");
    
    return direction;
}

int SpecialSiteAdapter::determineVehicleDirection(const obj_data& obj, bool in_roi, int roi_direction) {
    if (!isActive()) {
        // Special Site 비활성화 시 원래 방향 반환
        return roi_direction;
    }
    
    std::lock_guard<std::mutex> lock(config_mutex_);
    
    // 유턴은 항상 무시
    if (roi_direction == 41) {
        logger->trace("Special Site: 유턴 차량 무시 - ID={}", obj.object_id);
        return -1;  // 무시
    }

    // 역방향 차량 무시 (-11, -21, -22, -31, -32, -41)
    if (roi_direction < -1) {  // -1은 ROI 밖이므로 제외
        logger->trace("Special Site: 역방향 차량 무시 - ID={}, 방향={}", 
                     obj.object_id, roi_direction);
        return -1;  // 무시
    }    
    
    // =============== straight_left 모드 (직진/좌회전 처리) ===============
    if (config_.straight_left) {
        logger->trace("Special Site straight_left 모드: ID={}, in_roi={}, roi_direction={}", 
                     obj.object_id, in_roi, roi_direction);
        
        // 1. 우회전 ROI 차량은 무조건 무시
        if (roi_direction >= 31 && roi_direction <= 32) {
            logger->debug("Special Site: 우회전 ROI 차량 무시 - ID={}, 방향={}", 
                        obj.object_id, roi_direction);
            return -1;  // 무시 (데이터 전송 안함)
        }
        
        // 2. 직진 ROI 차량 - 방향 유지
        if (roi_direction == 11) {
            logger->debug("Special Site: 직진 ROI 차량 검출 - ID={}, 방향 유지(11)", 
                        obj.object_id);
            return 11;
        }
        
        // 3. 좌회전 ROI 차량 - 방향 유지  
        if (roi_direction == 21 || roi_direction == 22) {
            logger->debug("Special Site: 좌회전 ROI 차량 검출 - ID={}, 방향 유지({})", 
                        obj.object_id, roi_direction);
            return roi_direction;
        }
        
        // 4. ROI 밖 차량 또는 ROI가 없는 경우 - 신호 기반 판단
        // roi_direction이 -1(ROI 밖), 0(초기값), 또는 in_roi가 false인 경우
        if (!in_roi || roi_direction <= 0) {
            int signal_direction = determineDirectionBySignal();
            
            logger->debug("Special Site: ROI 밖 차량, 신호 기반 방향 결정 - ID={}, 방향={} ({})", 
                        obj.object_id, signal_direction,
                        signal_direction == 11 ? "직진" : "좌회전");
            
            return signal_direction;
        }
        
        // 5. 예상치 못한 경우 - 원래 방향 반환
        logger->warn("Special Site straight_left: 예상치 못한 roi_direction={} - 원래 값 반환", 
                    roi_direction);
        return roi_direction;
    }
    
    // =============== right 모드 (우회전만 처리) ===============
    if (config_.right) {
        logger->trace("Special Site right 모드: ID={}, in_roi={}, roi_direction={}", 
                     obj.object_id, in_roi, roi_direction);
        
        // 우회전 ROI 차량만 처리
        if (roi_direction >= 31 && roi_direction <= 32) {
            logger->debug("Special Site: 우회전 차량 처리 - ID={}, 방향={}", 
                        obj.object_id, roi_direction);
            return roi_direction;
        }
        
        // 나머지는 모두 무시
        logger->debug("Special Site: 우회전 외 차량 무시 - ID={}, 방향={}", 
                     obj.object_id, roi_direction);
        return -1;  // 무시
    }
    
    // ConfigManager에서 Special Site 세부 설정 자동 보정이 안된 오류 상황
    logger->error("Special Site: 잘못된 설정 (straight_left={}, right={}) - 원래 방향 반환", 
                 config_.straight_left, config_.right);    
    return roi_direction;
}