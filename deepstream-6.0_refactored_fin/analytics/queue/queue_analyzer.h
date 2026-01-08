#ifndef QUEUE_ANALYZER_H
#define QUEUE_ANALYZER_H

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "queue_types.h"
#include "../../common/common_types.h"
#include "../../data/redis/redis_client.h"
#include "../../utils/config_manager.h"

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

/**
 * @brief 대기행렬 분석 클래스
 * 
 * 신호 변경 시점에 차로별/접근로별 대기행렬을 분석하고
 * Redis로 전송
 * 
 * 이미지 캡처는 트리거만 하고, 실제 저장은 ImageCaptureHandler에서 처리
 */
class QueueAnalyzer {
private:
    // 설정
    QueueConfig config_;
    
    // 외부 모듈 참조
    RedisClient* redis_client_ = nullptr;
    
    // 신호 상태
    int last_green_start_time_ = 0;    // 이전 녹색 신호 시작 시간
    int last_red_start_time_ = 0;      // 마지막 적색 신호 시작 시간
    int current_cycle_ = 0;            // 현재 신호 주기
    
    // 대기행렬 추적
    std::map<int, int> max_vehicles_per_lane_;      // 차로별 최대 차량 수
    std::map<int, int> residual_vehicles_per_lane_; // 차로별 잔여 차량 수
    mutable std::mutex queue_mutex_;
    
    // 이미지 캡처 관련
    std::atomic<int> residual_timestamp_{0};     // 대기행렬 이미지 캡처 시간
    std::atomic<bool> waiting_image_save_{false}; // 이미지 캡처 플래그
    
    // 로거
    std::shared_ptr<spdlog::logger> logger = nullptr;
    
    // 내부 메서드
    std::string generateImageFileName(int timestamp) const;
    double calculateQueueLength(int vehicle_count) const;
    std::string queueDataToJson(const QueueDataPacket& packet) const;
    bool sendQueueData(const QueueDataPacket& packet);
    
public:
    QueueAnalyzer();
    ~QueueAnalyzer() = default;
    
    /**
     * @brief 초기화
     * @param redis_client Redis 클라이언트 포인터
     * @return 성공 시 true
     */
    bool initialize(RedisClient* redis_client);
    
    /**
     * @brief 신호가 적색으로 변경됨
     * @param timestamp 변경 시간
     */
    void onRedSignal(int timestamp);
    
    /**
     * @brief 신호가 녹색으로 변경됨
     * @param timestamp 변경 시간
     * @param residual_cars 차로별 잔여 차량 수
     * @return 대기행렬 데이터 패킷
     */
    QueueDataPacket onGreenSignal(int timestamp, 
                                 const std::map<int, int>& residual_cars);
    
    /**
     * @brief 차로별 차량 수 업데이트
     * @param lane_counts 현재 차로별 차량 수
     */
    void updateLaneCounts(const std::map<int, int>& lane_counts);
    
    /**
     * @brief 대기행렬 분석
     * @param residual_cars 차로별 잔여 차량 수
     * @return 대기행렬 데이터 패킷
     */
    QueueDataPacket analyzeQueue(const std::map<int, int>& residual_cars);
    
    /**
     * @brief 대기행렬 데이터 로깅
     * @param data 대기행렬 데이터
     */
    void logQueueData(const QueueDataPacket& data) const;
    
    /**
     * @brief 이미지 캡처 트리거
     * @param need_capture true일 경우 이미지 캡처 필요
     */
    void triggerImageCapture(bool need_capture = true) { 
        waiting_image_save_.store(need_capture); 
    }
    
    /**
     * @brief 이미지 캡처 필요 여부 확인
     * @return 캡처가 필요하면 true
     */
    bool isImageCaptureNeeded() const { 
        return waiting_image_save_.load(); 
    }
    
    /**
     * @brief 이미지 캡처 완료 및 타임스탬프 설정
     * @param timestamp 캡처 시간
     */
    void setImageCaptured(int timestamp) {
        residual_timestamp_.store(timestamp);
        waiting_image_save_.store(false);
    }
    
    /**
     * @brief 대기행렬 이미지 타임스탬프 조회
     * @return 마지막 캡처 시간
     */
    int getResidualTimestamp() const { 
        return residual_timestamp_.load(); 
    }
};

#endif // QUEUE_ANALYZER_H