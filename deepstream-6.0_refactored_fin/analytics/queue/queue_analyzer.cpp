#include "queue_analyzer.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include "../../common/common_types.h"
#include "../../data/redis/channel_types.h"
#include "../../json/json.h"

QueueAnalyzer::QueueAnalyzer() {
    logger = getLogger("DS_QueueAnalyzer_log");
    logger->info("QueueAnalyzer 생성");
    
    // 기본 설정
    config_.capture_image = true;
}

bool QueueAnalyzer::initialize(RedisClient* redis_client) {
    auto& config = ConfigManager::getInstance();
    
    // 차량 4K 전용 모드 체크
    if (config.is4KOnlyMode()) {
        logger->warn("QueueAnalyzer: 차량 4K 전용 모드에서는 대기행렬 분석 비활성화");
        return false;
    }
    
    // wait_queue 설정 체크 (이미 ConfigManager에서 차량 4K 모드 체크함)
    if (!config.isWaitQueueEnabled()) {
        logger->info("QueueAnalyzer: wait_queue가 비활성화되어있음");
        return false;
    }
    
    if (!redis_client) {
        logger->error("RedisClient가 NULL");
        return false;
    }
    
    redis_client_ = redis_client;
    
    // 이미지 저장 경로 가져오기
    config_.image_save_path = config.getFullImagePath("wait_queue");
    
    logger->info("QueueAnalyzer 초기화 완료 - 이미지 경로: {}", config_.image_save_path);
    return true;
}

void QueueAnalyzer::onRedSignal(int timestamp) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    
    last_red_start_time_ = timestamp;
    max_vehicles_per_lane_.clear();
    
    // 적색 신호 시 이미지 캡처 트리거
    triggerImageCapture(true);
    
    logger->info("적색 신호 시작: {} - 대기행렬 추적 시작, 이미지 캡처 트리거", timestamp);
}

QueueDataPacket QueueAnalyzer::onGreenSignal(int timestamp, 
                                            const std::map<int, int>& residual_cars) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    
    logger->info("녹색 신호 시작: {} (주기: {})", timestamp, current_cycle_);
    
    // 첫 번째 녹색 신호인 경우 스킵 (이전 녹색 시작 시간이 없음)
    if (last_green_start_time_ == 0) {
        last_green_start_time_ = timestamp;
        current_cycle_++;
        logger->info("첫 번째 녹색 신호 - 데이터 전송 스킵");
        return QueueDataPacket();
    }
    
    // 대기행렬 분석
    QueueDataPacket packet = analyzeQueue(residual_cars);
    
    // Redis로 전송
    if (packet.is_valid) {
        if (sendQueueData(packet)) {
            logger->info("대기행렬 데이터 Redis 전송 성공");
        } else {
            logger->error("대기행렬 데이터 Redis 전송 실패");
        }
    }
    
    // 녹색 시작 시간 업데이트
    last_green_start_time_ = timestamp;
    
    // 주기 증가
    current_cycle_++;
    
    return packet;
}

void QueueAnalyzer::updateLaneCounts(const std::map<int, int>& lane_counts) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    
    // 각 차로별 최대값 업데이트
    for (const auto& [lane, count] : lane_counts) {
        max_vehicles_per_lane_[lane] = std::max(max_vehicles_per_lane_[lane], count);
    }
}

QueueDataPacket QueueAnalyzer::analyzeQueue(const std::map<int, int>& residual_cars) {
    QueueDataPacket packet;
    packet.timestamp = getCurTime();
    packet.signal_cycle = current_cycle_;
    
    try {
        // 이미지 정보 (멤버 변수 사용)
        int captured_timestamp = getResidualTimestamp();
        if (config_.capture_image && captured_timestamp > 0) {
            packet.has_image = true;
            packet.image_timestamp = std::to_string(captured_timestamp);
            
            // 이미지 파일명 생성
            std::string img_file = generateImageFileName(captured_timestamp);
            
            // 접근로별 대기행렬
            packet.approach.stats_bgng_unix_tm = last_green_start_time_;   // 이전 녹색 시작
            packet.approach.stats_end_unix_tm = packet.timestamp;          // 현재 녹색 시작
            packet.approach.img_path_nm = config_.image_save_path;
            packet.approach.img_file_nm = img_file;
        }
        
        // 접근로 전체 통계
        double total_residual = 0;
        double total_max = 0;
        
        // 차로별 대기행렬 계산
        for (const auto& [lane, residual_count] : residual_cars) {
            LaneQueue lane_queue;
            lane_queue.lane_no = lane;
            lane_queue.stats_bgng_unix_tm = last_green_start_time_;   // 이전 녹색 시작
            lane_queue.stats_end_unix_tm = packet.timestamp;          // 현재 녹색 시작
            
            // 잔여 대기행렬
            lane_queue.rmnn_queu_lngt = calculateQueueLength(residual_count);
            total_residual += lane_queue.rmnn_queu_lngt;
            
            // 최대 대기행렬
            if (max_vehicles_per_lane_.find(lane) != max_vehicles_per_lane_.end()) {
                lane_queue.max_queu_lngt = calculateQueueLength(max_vehicles_per_lane_[lane]);
                total_max += lane_queue.max_queu_lngt;
            } else {
                lane_queue.max_queu_lngt = lane_queue.rmnn_queu_lngt;
                total_max += lane_queue.max_queu_lngt;
            }
            
            // 이미지 정보
            if (packet.has_image) {
                lane_queue.img_path_nm = config_.image_save_path;
                lane_queue.img_file_nm = packet.approach.img_file_nm;
            }
            
            lane_queue.is_valid = true;
            packet.lanes.push_back(lane_queue);
            
            logger->debug("차로 {} 대기행렬: 잔여={:.1f}, 최대={:.1f}", 
                         lane, lane_queue.rmnn_queu_lngt, lane_queue.max_queu_lngt);
        }
        
        // 접근로 전체 값 설정
        packet.approach.rmnn_queu_lngt = total_residual;
        packet.approach.max_queu_lngt = total_max;
        packet.approach.is_valid = true;
        
        packet.is_valid = true;
        
        logger->info("대기행렬 분석 완료: 접근로 잔여={:.1f}, 최대={:.1f}, 차로수={}", 
                    total_residual, total_max, packet.lanes.size());
        
    } catch (const std::exception& e) {
        logger->error("대기행렬 분석 중 오류: {}", e.what());
        packet.is_valid = false;
    }
    
    return packet;
}

double QueueAnalyzer::calculateQueueLength(int vehicle_count) const {
    // 차량 수를 대기행렬 길이로 변환
    // 차량 평균 길이 등을 고려하지 않고 여기서는 단순히 차량 수를 반환
    return static_cast<double>(vehicle_count);
}

std::string QueueAnalyzer::generateImageFileName(int timestamp) const {
    return std::to_string(timestamp) + ".jpg";
}

std::string QueueAnalyzer::queueDataToJson(const QueueDataPacket& packet) const {
    Json::Value root;
    Json::FastWriter writer;
    
    // 접근로별 대기행렬
    Json::Value approach;
    approach["stats_bgng_unix_tm"] = packet.approach.stats_bgng_unix_tm;
    approach["stats_end_unix_tm"] = packet.approach.stats_end_unix_tm;
    approach["rmnn_queu_lngt"] = packet.approach.rmnn_queu_lngt;
    approach["max_queu_lngt"] = packet.approach.max_queu_lngt;
    approach["img_path_nm"] = packet.approach.img_path_nm;
    approach["img_file_nm"] = packet.approach.img_file_nm;
    
    // 차로별 대기행렬
    Json::Value lanes(Json::arrayValue);
    for (const auto& lane : packet.lanes) {
        Json::Value lane_obj;
        lane_obj["lane_no"] = lane.lane_no;
        lane_obj["stats_bgng_unix_tm"] = lane.stats_bgng_unix_tm;
        lane_obj["stats_end_unix_tm"] = lane.stats_end_unix_tm;
        lane_obj["rmnn_queu_lngt"] = lane.rmnn_queu_lngt;
        lane_obj["max_queu_lngt"] = lane.max_queu_lngt;
        lane_obj["img_path_nm"] = lane.img_path_nm;
        lane_obj["img_file_nm"] = lane.img_file_nm;
        lanes.append(lane_obj);
    }
    
    // 전체 구조
    root["approach"] = approach;
    root["lanes"] = lanes;
    
    return writer.write(root);
}

bool QueueAnalyzer::sendQueueData(const QueueDataPacket& packet) {
    if (!redis_client_) {
        logger->error("Redis 클라이언트가 초기화되지 않음");
        return false;
    }
    
    try {
        // JSON 변환
        std::string json_data = queueDataToJson(packet);
        
        // Redis 전송
        int result = redis_client_->sendData(CHANNEL_QUEUE, json_data);
        
        if (result == 0) {
            logger->info("대기행렬 데이터 전송 성공 (크기: {} bytes)", json_data.size());
            logger->info("전송 데이터: {}", json_data);
            return true;
        } else {
            logger->error("대기행렬 데이터 전송 실패 (결과: {})", result);
            return false;
        }
        
    } catch (const std::exception& e) {
        logger->error("대기행렬 데이터 전송 중 예외: {}", e.what());
        return false;
    }
}

void QueueAnalyzer::logQueueData(const QueueDataPacket& data) const {
    logger->info("=== 대기행렬 데이터 ===");
    logger->info("신호 주기: {}, 이전 녹색: {} → 현재 녹색: {}", 
                data.signal_cycle,
                data.approach.stats_bgng_unix_tm,
                data.approach.stats_end_unix_tm);
    
    logger->info("[접근로] 잔여: {:.1f}대, 최대: {:.1f}대",
                data.approach.rmnn_queu_lngt,
                data.approach.max_queu_lngt);
    
    for (const auto& lane : data.lanes) {
        logger->info("[차로 {}] 잔여: {:.1f}대, 최대: {:.1f}대",
                    lane.lane_no,
                    lane.rmnn_queu_lngt,
                    lane.max_queu_lngt);
    }
    
    if (data.has_image) {
        logger->info("대기행렬 이미지: {}", data.approach.img_file_nm);
    }
}