/*
 * pedestrian_processor.cpp
 * 
 * 보행자 감지 처리 클래스 구현
 * - obj_data 복사본 반환 방식으로 스레드 안전성 보장
 * - process_meta에서 전달받은 current_pos 활용
 */

#include "pedestrian_processor.h"
#include "../../data/redis/channel_types.h"
#include "../../data/redis/redis_client.h"
#include "../../roi_module/roi_handler.h"
#include "../../utils/config_manager.h"
#include <algorithm>
#include <sstream>

/**
 * @brief 생성자
 */
PedestrianProcessor::PedestrianProcessor(ROIHandler& roi, RedisClient& redis)
    : roi_handler_(roi)
    , redis_client_(redis) {
    
    // 로거 초기화
    logger = getLogger("DS_PedestrianProcessor");
    logger->info("PedestrianProcessor 초기화");
    
    // ConfigManager에서 FPS 가져오기
    auto& config = ConfigManager::getInstance();
    DECISION_FRAMES = static_cast<size_t>(config.getCameraFPS());  // camera_fps 값 사용 (15, 30 등)
    
    // 횡단보도 ROI 체크
    if (roi_handler_.crosswalk_roi.empty()) {
        logger->warn("횡단보도 ROI 없음 - 보행자 프로세서 비활성화");
        is_enabled_ = false;
    } else {
        is_enabled_ = true;
        logger->info("보행자 프로세서 초기화 완료 ({}프레임 모드) - 횡단보도 ROI: {}개 좌표", 
                    DECISION_FRAMES, roi_handler_.crosswalk_roi.size());
    }
}

/**
 * @brief 소멸자
 */
PedestrianProcessor::~PedestrianProcessor() {
    if (logger) {
        logger->info("보행자 프로세서 종료");
    }
}

/**
 * @brief 보행자 처리 메인 함수
 */
obj_data PedestrianProcessor::processPedestrian(const obj_data& input_obj, const box& obj_box,
                                               const ObjPoint& current_pos, int current_time, 
                                               bool second_changed) {
    // 입력 데이터 복사
    obj_data obj = input_obj;
    
    // ROI가 없으면 처리 안함
    if (!is_enabled_) {
        return obj;
    }
    
    // 차량 필터링 (안전장치)
    if (!isPedestrianClass(obj.class_id)) {
        logger->warn("Non-pedestrian object passed to PedestrianProcessor: ID={}, class_id={}, label={}", 
                    obj.object_id, obj.class_id, obj.label);
        return obj;  // 수정 없이 반환
    }
    
    try {
        // 새 보행자 체크 (첫 프레임)
        if (obj.first_detected_time == current_time) {
            // 궤적 초기화
            obj.prev_ped.clear();
            obj.cross_out = false;
            obj.ped_pass = false;
            obj.ped_dir = 0;
            logger->debug("새 보행자 감지: ID={}", obj.object_id);
        }
        
        // 이미 처리 완료된 보행자는 스킵
        if (!obj.ped_pass) {
            // 횡단보도 전이 체크 및 궤적 분석
            // process_meta에서 계산된 current_pos 사용
            checkCrosswalkTransition(obj, current_pos, current_time);
        }
        
    } catch (const std::exception& e) {
        logger->error("보행자 {} 처리 중 오류: {}", obj.object_id, e.what());
    }
    
    // 수정된 obj_data 반환
    return obj;
}

/**
 * @brief 횡단보도 전이 체크 및 궤적 분석
 */
void PedestrianProcessor::checkCrosswalkTransition(obj_data& obj, const ObjPoint& current_pos,
                                                  int current_time) {
    // 횡단보도 내부 체크
    bool in_crosswalk = roi_handler_.isInCrossWalk(current_pos);
    
    if (in_crosswalk) {
        // 외부에서 진입한 경우만 처리 (cross_out 체크)
        if (obj.cross_out) {
            // 궤적 분석
            analyzeTrajectory(obj, current_pos, current_time);
        }
        // 횡단보도 내부 상태 유지
    }
    else {
        // 횡단보도 밖
        obj.cross_out = true;
        
        // 이미 처리 완료된 경우 궤적 초기화 (다음 진입을 위해)
        if (obj.ped_pass) {
            obj.prev_ped.clear();
            // 다음 진입을 위해 플래그 리셋은 하지 않음 (중복 전송 방지)
        }
    }
}

/**
 * @brief 궤적 분석 및 방향 판정
 */
void PedestrianProcessor::analyzeTrajectory(obj_data& obj, const ObjPoint& current_pos,
                                          int current_time) {
    // 설정된 프레임 수만큼 궤적 수집 (FPS 기반)
    if (obj.prev_ped.size() == DECISION_FRAMES) {
        // 패턴 분석: 전체 X좌표가 오름차순 또는 내림차순인지 확인
        bool ascending = true, descending = true;
        
        for (size_t i = 0; i < DECISION_FRAMES - 1; i++) {
            if (obj.prev_ped[i].x > obj.prev_ped[i+1].x) {
                ascending = false;
            }
            if (obj.prev_ped[i].x < obj.prev_ped[i+1].x) {
                descending = false;
            }
        }
        
        if (ascending) {
            // 오른쪽 방향
            obj.ped_pass = true;
            obj.ped_dir = 1;
            
            // 메타데이터 전송
            sendMetadata(obj, current_time, "R");
            logger->info("오른쪽 방향 보행자: ID={}, {}프레임 패턴 확인 완료", 
                       obj.object_id, DECISION_FRAMES);
        }
        else if (descending) {
            // 왼쪽 방향
            obj.ped_pass = true;
            obj.ped_dir = -1;
            
            // 메타데이터 전송
            sendMetadata(obj, current_time, "L");
            logger->info("왼쪽 방향 보행자: ID={}, {}프레임 패턴 확인 완료", 
                       obj.object_id, DECISION_FRAMES);
        }
        else {
            // 패턴이 명확하지 않으면 가장 오래된 프레임 제거하고 계속
            obj.prev_ped.pop_front();
            obj.prev_ped.push_back(current_pos);
            logger->trace("보행자 {} 패턴 불명확 - 궤적 갱신", obj.object_id);
        }
    }
    else {
        // 설정 프레임 미만이면 궤적 추가
        obj.prev_ped.push_back(current_pos);
        logger->trace("보행자 {} 프레임 수집 중: {}/{}", 
                    obj.object_id, obj.prev_ped.size(), DECISION_FRAMES);
    }
}

/**
 * @brief 메타데이터 전송
 */
void PedestrianProcessor::sendMetadata(const obj_data& obj, int current_time, 
                                      const std::string& direction) {
    // CSV 형식: trce_id(트래킹ID), dttn_unix_tm(검지유닉스시각), drct_se_cd(방향구분코드)
    std::stringstream metadata;
    metadata << obj.object_id << ","     // trce_id
            << current_time << ","       // dttn_unix_tm
            << direction;                // drct_se_cd (L 또는 R)
    
    // Redis 전송
    int result = redis_client_.sendData(CHANNEL_PEDESTRIAN, metadata.str());
    
    if (result == 0) {
        logger->info("보행자 메타데이터 전송 완료: {}", metadata.str());
    } else {
        logger->error("보행자 메타데이터 전송 실패: ID={}, 결과={}", 
                     obj.object_id, result);
    }
}

/**
 * @brief 통계 정보 출력
 */
void PedestrianProcessor::logStatistics() const {
    if (is_enabled_) {
        logger->debug("보행자 프로세서 상태: 활성화 ({}프레임 모드)", DECISION_FRAMES);
    } else {
        logger->debug("보행자 프로세서 상태: 비활성화 (ROI 없음)");
    }
}