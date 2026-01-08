/*
 * special_site_adapter.h
 * 
 * Special Site 모드 처리를 위한 어댑터 클래스
 * - 진입로 차량 검출이 어려운 특수 교차로 처리
 * - 신호 정보 기반 방향 결정 (타겟신호 ON=직진, OFF=좌회전)
 * - SQLite 저장 비활성화 제어
 */

#ifndef SPECIAL_SITE_ADAPTER_H
#define SPECIAL_SITE_ADAPTER_H

#include <memory>
#include <mutex>
#include "../../common/common_types.h"
#include "../../common/object_data.h"
#include "../../utils/config_manager.h"

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

// Forward declarations
class SignalCalculator;
class ROIHandler;

/**
 * @brief Special Site 모드 설정 구조체
 */
struct SpecialSiteConfig {
    bool enabled = false;           // Special Site 모드 활성화 여부
    bool straight_left = false;     // 직진/좌회전 처리 모드
    bool right = false;             // 우회전 처리 모드
};

/**
 * @brief Special Site 모드 처리 어댑터
 * 
 * 특수 교차로에서 차량 방향 결정을 위한 어댑터 클래스
 * - 신호 정보 기반 방향 결정 (타겟신호 ON=직진, OFF=좌회전)
 * - ROI 기반 필터링
 * 
 * 동작 모드:
 * 1. straight_left 모드: 우회전 무시, ROI 밖 차량은 신호 기반 판단
 * 2. right 모드: 우회전만 처리
 */
class SpecialSiteAdapter {
private:
    // 의존성
    SignalCalculator* signal_calculator_;
    ROIHandler* roi_handler_;
    
    // 설정
    SpecialSiteConfig config_;
    bool is_active_ = false;  // 2K + Special Site 활성화 여부
    
    // 뮤텍스
    mutable std::mutex config_mutex_;
    
    // 로거
    std::shared_ptr<spdlog::logger> logger;
    
    /**
     * @brief 신호 기반 방향 결정
     * @return 타겟신호 ON=11(직진), OFF=21(좌회전)
     */
    int determineDirectionBySignal() const;

public:
    /**
     * @brief 생성자
     * @param signal_calc SignalCalculator 포인터 (nullptr 가능)
     * @param roi_handler ROIHandler 포인터
     */
    SpecialSiteAdapter(SignalCalculator* signal_calc, ROIHandler* roi_handler);
    
    /**
     * @brief 소멸자
     */
    ~SpecialSiteAdapter() = default;
    
    /**
     * @brief 초기화 및 설정 로드
     * @return 성공 시 true
     */
    bool initialize();
    
    /**
     * @brief Special Site 모드 활성화 여부
     * @return 활성화되어 있으면 true
     */
    bool isActive() const { 
        std::lock_guard<std::mutex> lock(config_mutex_);
        return is_active_; 
    }
    
    /**
     * @brief 차량 방향 결정
     * @param obj 차량 객체
     * @param in_roi ROI 내부 여부
     * @param roi_direction ROI에서 검출된 방향
     * @return 결정된 방향 코드, 무시해야 할 경우 -1
     * 
     * 동작:
     * - straight_left 모드: 
     *   - 우회전 ROI → 무시 (-1)
     *   - 직진/좌회전 ROI → 원래 방향 유지
     *   - ROI 밖 → 신호 기반 결정 (타겟신호 ON=직진, OFF=좌회전)
     * - right 모드:
     *   - 우회전 ROI → 처리
     *   - 나머지 → 무시 (-1)
     */
    int determineVehicleDirection(const obj_data& obj, bool in_roi, int roi_direction);
    
    /**
     * @brief 현재 설정 반환
     * @return Special Site 설정
     */
    SpecialSiteConfig getConfig() const {
        std::lock_guard<std::mutex> lock(config_mutex_);
        return config_;
    }
    
    /**
     * @brief SignalCalculator 설정/변경
     * @param signal_calc 새로운 SignalCalculator 포인터
     */
    void setSignalCalculator(SignalCalculator* signal_calc);
};

#endif // SPECIAL_SITE_ADAPTER_H