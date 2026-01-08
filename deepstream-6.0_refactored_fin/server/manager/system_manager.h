/*
 * system_manager.h
 * 
 * ITS 전체 시스템 통합 관리 클래스
 * - 모든 모듈의 초기화 및 생명주기 관리
 * - 모듈 간 연결 설정
 * - 시스템 시작/중지
 * - 신호 변경 이벤트 중앙 처리
 * - 매 초 데이터 업데이트 처리
 * - 공용 리소스(Redis, SQLite) 통합 관리
 */

#ifndef SYSTEM_MANAGER_H
#define SYSTEM_MANAGER_H

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include "site_info_manager.h"
#include "../signal/signal_calculator.h"
#include "../../analytics/incident/incident_detector.h"
#include "../../analytics/queue/queue_analyzer.h"
#include "../../analytics/statistics/stats_generator.h"
#include "../../data/redis/redis_client.h"
#include "../../data/sqlite/sqlite_handler.h"
#include "../../detection/special/special_site_adapter.h"
#include "../../image/image_capture_handler.h"
#include "../../monitoring/car_presence.h"
#include "../../monitoring/pedestrian_presence.h"
#include "../../roi_module/roi_handler.h"

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

/**
 * @brief ITS 전체 시스템 통합 관리 클래스
 * 
 * 역할:
 * - 모든 모듈의 초기화 및 생명주기 관리
 * - 모듈 간 연결 설정
 * - 시스템 시작/중지
 * - 신호 변경 이벤트 중앙 처리
 * - 매 초 데이터 업데이트 처리
 * - 공용 리소스(Redis, SQLite) 통합 관리
 * 
 * 관리 모듈:
 * - RedisClient: 데이터 전송
 * - SQLiteHandler: 데이터베이스 관리
 * - SiteInfoManager: 사이트 정보 관리
 * - SignalCalculator: 신호 역산
 * - StatsGenerator: 통계 생성 (인터벌/신호현시)
 * - QueueAnalyzer: 대기행렬 분석
 * - IncidentDetector: 돌발상황 감지 (독립적 이미지 처리)
 * - ImageCaptureHandler: 대기행렬 이미지 캡처 전용
 * - CarPresence: 차량 존재 감지 (독립적)
 * - PedestrianPresence: 보행자 존재 감지 (독립적)
 * - SpecialSiteAdapter: Special Site 모드 처리
 */
class SystemManager {
private:
    // 핵심 모듈들
    std::unique_ptr<SiteInfoManager> site_info_mgr_;
    std::unique_ptr<SignalCalculator> signal_calc_;
    std::unique_ptr<RedisClient> redis_client_;
    std::unique_ptr<SQLiteHandler> sqlite_handler_;
    std::unique_ptr<StatsGenerator> stats_gen_;
    std::unique_ptr<QueueAnalyzer> queue_analyzer_;
    std::unique_ptr<IncidentDetector> incident_detector_;
    std::unique_ptr<ImageCaptureHandler> image_capture_handler_;
    
    // Presence 모듈들 (신호와 무관하게 독립적 운영)
    std::unique_ptr<CarPresence> car_presence_;
    std::unique_ptr<PedestrianPresence> ped_presence_;
    
    // Special Site 어댑터
    std::unique_ptr<SpecialSiteAdapter> special_site_adapter_;
    
    // ROI Handler (차로 정보 획득용)
    ROIHandler* roi_handler_ = nullptr;  // 외부에서 초기화된 것을 받음
    
    // 사이트 정보
    SiteInfo site_info_;
    
    // 상태 추적
    std::atomic<bool> running_{false};
    std::atomic<bool> last_signal_state_{false};  // 이전 신호 상태
    std::map<int, int> last_lane_counts_;         // 마지막 차로별 차량 수
    std::mutex lane_counts_mutex_;
    
    // 로거
    std::shared_ptr<spdlog::logger> logger = nullptr;
    
    // 내부 메서드
    void handleSignalChangeCallback(const SignalChangeEvent& event);

public:
    SystemManager();
    ~SystemManager();
    
    /**
     * @brief 시스템 초기화
     * @param config_path config.json 경로
     * @param roi_handler ROIHandler 포인터 (차로 정보용)
     * @param image_cropper ImageCropper 포인터 (이미지 캡처용)
     * @param image_storage ImageStorage 포인터 (이미지 저장용)
     * @return 성공 시 true
     */
    bool initialize(const std::string& config_path, 
                   ROIHandler* roi_handler = nullptr,
                   ImageCropper* image_cropper = nullptr,
                   ImageStorage* image_storage = nullptr);
    
    /**
     * @brief 시스템 시작
     */
    void start();
    
    /**
     * @brief 시스템 중지
     */
    void stop();

    /**
     * @brief Presence 모듈 업데이트 (매 프레임)
     * @param vehicle_positions 현재 프레임의 차량 위치들 (id -> position)
     * @param pedestrian_positions 현재 프레임의 보행자 위치들 (id -> position)
     * @param current_time 현재 시간
     * 
     * process_meta에서 매 프레임마다 호출
     * 신호와 무관하게 독립적으로 운영
     */
    void updatePresenceModules(const std::map<int, ObjPoint>& vehicle_positions,
                              const std::map<int, ObjPoint>& pedestrian_positions,
                              int current_time);

    /**
     * @brief 매 초마다 호출되는 데이터 업데이트
     * @param lane_counts 현재 차로별 차량 수
     * @param current_time 현재 시간
     * 
     * process_meta에서 매 초마다 한 번만 호출
     * 신호 변경 체크 및 대기행렬 업데이트 자동 처리
     */
    void updatePerSecondData(const std::map<int, int>& lane_counts, int current_time);
    
    /**
     * @brief 현재 신호 상태 조회
     * @return 녹색 신호 여부
     */
    bool isGreenSignal() const;
    
    /**
     * @brief 모듈 참조 반환 (외부 모듈에서 사용)
     */
    StatsGenerator* getStatsGenerator() { return stats_gen_.get(); }
    RedisClient* getRedisClient() { return redis_client_.get(); }
    SQLiteHandler* getSQLiteHandler() { return sqlite_handler_.get(); }
    SiteInfoManager* getSiteInfoManager() { return site_info_mgr_.get(); }
    SignalCalculator* getSignalCalculator() { return signal_calc_.get(); }
    QueueAnalyzer* getQueueAnalyzer() { return queue_analyzer_.get(); }
    IncidentDetector* getIncidentDetector() { return incident_detector_.get(); }
    ImageCaptureHandler* getImageCaptureHandler() { return image_capture_handler_.get(); }
    CarPresence* getCarPresence() { return car_presence_.get(); }
    PedestrianPresence* getPedestrianPresence() { return ped_presence_.get(); }
    SpecialSiteAdapter* getSpecialSiteAdapter() { return special_site_adapter_.get(); }
};

#endif // SYSTEM_MANAGER_H