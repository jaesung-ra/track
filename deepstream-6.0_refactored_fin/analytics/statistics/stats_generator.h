#ifndef STATS_GENERATOR_H
#define STATS_GENERATOR_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "stats_query_helper.h"
#include "stats_types.h"
#include "../../common/common_types.h"
#include "../../data/redis/channel_types.h"
#include "../../data/redis/redis_client.h"
#include "../../data/sqlite/sqlite_handler.h"
#include "../../server/core/signal_types.h"
#include "../../roi_module/roi_handler.h"

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

/**
 * @brief 통계 데이터 생성 및 관리 클래스
 * 
 * 인터벌 통계와 신호현시 통계를 모두 처리
 * - 5분 인터벌 타이머 내장
 * - 신호 변경 이벤트 처리
 * - Redis 전송 통합
 * - StatsQueryHelper를 통한 DB 접근
 * - 거리 기반 교통밀도 계산 (대/km)
 * - 차선별 실제 길이를 활용한 밀도 계산
 * 
 * 24/7 안정성:
 * - 모든 예외 상황 처리
 * - 거리값 유효성 검사 및 기본값 설정
 * - 상세 로깅으로 문제 추적 가능
 */
class StatsGenerator {
private:
    // 설정
    int total_lanes_ = 0;
    int interval_minutes_ = 5;
    int camera_fps_ = 15;                // 카메라 FPS 캐싱
    double roi_distance_m_ = 100.0;      // ROI 실제 거리 (미터) - 기본값 100m (폴백용)
    static constexpr double DEFAULT_ROI_DISTANCE = 100.0;  // 기본 ROI 거리
    
    // 차선별 실제 길이 (미터)
    std::map<int, double> lane_lengths_;  // 차선번호(1-based) -> 길이
    
    // 외부 의존성 (포인터로 참조)
    RedisClient* redis_client_ = nullptr;
    SQLiteHandler* sqlite_handler_ = nullptr;
    ROIHandler* roi_handler_ = nullptr;
    
    // 통계 쿼리 헬퍼
    std::unique_ptr<StatsQueryHelper> query_helper_;
    
    // 스레드 관련
    std::thread interval_thread_;
    std::atomic<bool> running_{false};
    
    // 뮤텍스
    mutable std::mutex stats_mutex_;
    mutable std::mutex frame_mutex_;  // 프레임 데이터 보호용
    
    // 조건 변수 (종료 시 빠른 응답을 위해)
    std::condition_variable cv_;
    std::mutex cv_mutex_;
    
    // 신호현시 통계용 시간 추적
    int last_signal_stats_time_ = 0;  // 이전 신호현시 통계 생성 시각
    
    // 프레임 기반 밀도 계산용 데이터
    int frame_count_ = 0;                           // 총 프레임 수
    std::map<int, int> per_lane_count_;             // 현재 프레임의 차로별 차량 수
    std::map<int, int> per_lane_total_;             // 차로별 누적 차량 수
    std::map<int, int> per_lane_max_;               // 차로별 최대 차량 수
    std::map<int, int> per_lane_min_;               // 차로별 최소 차량 수
    
    // 로거
    std::shared_ptr<spdlog::logger> logger = nullptr;
    
    // 내부 메서드
    // 통계 생성 헬퍼 메서드들
    ApproachStats generateApproachStats(StatsType type, int start_time, int end_time,
                                       const std::map<int, DensityInfo>& density) const;
    std::vector<TurnTypeStats> generateTurnTypeStats(StatsType type, int start_time, int end_time) const;
    std::vector<VehicleTypeStats> generateVehicleTypeStats(StatsType type, int start_time, int end_time) const;
    std::vector<LaneStats> generateLaneStats(StatsType type, int start_time, int end_time,
                                           const std::map<int, DensityInfo>& density) const;
    
    /**
     * @brief 거리 기반 교통밀도 계산
     * 
     * 프레임당 차량 수를 km당 밀도로 변환
     * 각 차선의 실제 길이를 사용하여 밀도 계산
     * 
     * @param time_window_sec 통계 시간 창 (초)
     * @return 차로별 밀도 정보 (대/km)
     */
    std::map<int, DensityInfo> calculateDensity(int time_window_sec) const;
    
    // 인터벌 타이머 스레드
    void intervalTimerThread();
    
    // 통합 통계 생성 메서드
    StatsDataPacket generateStatistics(StatsType type, int start_time, int end_time) const;
    
    // 통계 검증 및 로깅
    bool validateStats(const StatsDataPacket& stats) const;
    void logStats(const StatsDataPacket& stats) const;
    
    // Redis 전송
    bool sendToRedis(const StatsDataPacket& stats) const;
    
    // 프레임 데이터 리셋
    void resetFrameData();
    
    // 인터벌 통계 생성 (내부용)
    bool generateIntervalStats();

    /**
     * @brief 다음 인터벌 통계 시간 계산
     * 
     * 정각 기준으로 다음 통계 생성 시간 계산
     * 예: 현재 13:03, 인터벌 5분 → 다음 통계는 13:05
     * 
     * @param current_time 현재 시간 (Unix timestamp)
     * @return 다음 통계 생성 시간 (Unix timestamp)
     */
    int calculateNextIntervalTime(int current_time) const;
    
    /**
     * @brief ROI 거리 초기화 및 유효성 검사
     * 
     * ROIHandler에서 차선별 실제 길이 로드
     * 유효하지 않으면 캘리브레이션 거리 또는 기본값 사용
     * 24/7 안정성을 위한 예외 처리
     */
    void initializeROIDistance();

public:
    /**
     * @brief 생성자
     */
    StatsGenerator();
    
    /**
     * @brief 소멸자
     */
    ~StatsGenerator();
    
    /**
     * @brief 초기화
     * @param redis_client Redis 클라이언트 포인터
     * @param sqlite_handler SQLite 핸들러 포인터
     * @param roi_handler ROI 핸들러 포인터 (차선 길이 정보용)
     * @param total_lanes 총 차로 수
     * @param interval_minutes 인터벌 통계 주기 (분)
     */
    void initialize(RedisClient* redis_client, SQLiteHandler* sqlite_handler,
                   ROIHandler* roi_handler, int total_lanes, int interval_minutes = 5);
    
    /**
     * @brief 통계 생성 시작
     * 인터벌 타이머 스레드 시작
     */
    void start();
    
    /**
     * @brief 통계 생성 중지
     * 모든 스레드 종료
     */
    void stop();
    
    /**
     * @brief 프레임별 차로 데이터 업데이트
     * process_meta에서 매 프레임마다 호출
     * @param lane_counts 차로별 차량 수 맵
     */
    void updateFrameData(const std::map<int, int>& lane_counts);
    
    // === 외부 이벤트 핸들러 ===
    
    /**
     * @brief 신호 변경 이벤트 처리
     * 녹색 신호가 켜질 때 신호현시 통계 생성
     * @param event 신호 변경 이벤트
     */
    void onSignalChange(const SignalChangeEvent& event);
    
    // === 상태 조회 ===
    
    /**
     * @brief 실행 상태 확인
     * @return 실행 중이면 true
     */
    bool isRunning() const { return running_.load(); }
    
    /**
     * @brief 현재 프레임 수 조회 (디버깅용)
     * @return 현재까지 처리된 프레임 수
     */
    int getFrameCount() const { 
        std::lock_guard<std::mutex> lock(frame_mutex_);
        return frame_count_; 
    }
    
    /**
     * @brief 현재 ROI 거리 조회 (디버깅용)
     * @return ROI 거리 (미터)
     */
    double getROIDistance() const { return roi_distance_m_; }
    
    /**
     * @brief 차선별 길이 맵 조회 (디버깅용)
     * @return 차선별 길이 맵
     */
    const std::map<int, double>& getLaneLengths() const { return lane_lengths_; }
};

#endif // STATS_GENERATOR_H