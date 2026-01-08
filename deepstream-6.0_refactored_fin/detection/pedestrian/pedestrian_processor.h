#ifndef PEDESTRIAN_PROCESSOR_H
#define PEDESTRIAN_PROCESSOR_H

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <string>
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
 * @brief 보행자 검지 처리 클래스
 * 
 * 보행자 객체의 추적, 방향 판정, 메타데이터 생성 및 전송 담당
 * 
 * - process_meta에서 current_pos를 파라미터로 전달
 * - 궤적 분석을 통한 방향 판정 (좌/우)
 * - 횡단보도 ROI 기반 처리
 * 
 * === 데이터 관리 정책 ===
 * - det_obj 직접 수정하지 않음
 * - 수정된 obj_data 복사본 반환
 * - 스레드 안전성 보장
 * 
 * === 방향 판정 로직 ===
 * - FPS 기반 프레임 수만큼 궤적 수집 (15fps = 15프레임 = 1초)
 * - 외부→내부 진입만 카운트 (cross_out 체크)
 * - X좌표 패턴 분석으로 방향 결정
 */
class PedestrianProcessor {
private:
    // 의존성
    ROIHandler& roi_handler_;
    RedisClient& redis_client_;
    
    // 로거
    std::shared_ptr<spdlog::logger> logger;
    
    // FPS 기반 동적 설정
    size_t DECISION_FRAMES;                     // camera_fps와 동일 (1초 궤적)
    
    // 활성화 상태
    bool is_enabled_ = false;
    
    // ========== 내부 메서드 ==========
    void checkCrosswalkTransition(obj_data& obj, const ObjPoint& current_pos, 
                                 int current_time);
    void analyzeTrajectory(obj_data& obj, const ObjPoint& current_pos, 
                          int current_time);
    void sendMetadata(const obj_data& obj, int current_time, 
                     const std::string& direction);
    
public:
    /**
     * @brief 생성자
     * @param roi ROI 핸들러 참조
     * @param redis Redis 클라이언트 참조
     */
    PedestrianProcessor(ROIHandler& roi, RedisClient& redis);
    
    /**
     * @brief 소멸자
     */
    ~PedestrianProcessor();
    
    /**
     * @brief 활성화 상태 확인
     */
    bool isEnabled() const { return is_enabled_; }
    
    /**
     * @brief 보행자 처리 메인 함수 - obj_data를 반환
     * @param input_obj 입력 보행자 데이터 (const 참조)
     * @param obj_box 바운딩 박스
     * @param current_pos 현재 프레임의 bottom_center 위치 (process_meta에서 계산)
     * @param current_time 현재 시간
     * @param second_changed 초 변경 여부
     * @return 수정된 obj_data (복사본)
     * 
     * @note current_pos는 process_meta에서 계산하여 전달
     */
    obj_data processPedestrian(const obj_data& input_obj, const box& obj_box,
                              const ObjPoint& current_pos, int current_time, 
                              bool second_changed);
    
    /**
     * @brief 통계 정보 출력
     */
    void logStatistics() const;
};

#endif // PEDESTRIAN_PROCESSOR_H