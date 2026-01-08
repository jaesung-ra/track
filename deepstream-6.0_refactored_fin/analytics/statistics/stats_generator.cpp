/*
 * stats_generator.cpp
 * 
 * 통계 데이터 생성 및 Redis 전송 구현
 * - 인터벌 통계 (hr_type_cd = 3)
 * - 신호현시 통계 (hr_type_cd = 1)
 * - Redis 채널: statistics
 * - StatsQueryHelper를 통한 DB 접근
 * - 차선별 실제 거리 기반 교통밀도 계산 (대/km)
 * - 24/7 안정성 강화: 예외처리, 유효성 검사, 기본값 설정
 */

#include "stats_generator.h"
#include "../../calibration/calibration.h"
#include "../../utils/config_manager.h"
#include <climits>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

StatsGenerator::StatsGenerator() {
    logger = getLogger("DS_StatsGen_log");
    logger->info("StatsGenerator 생성");
}

StatsGenerator::~StatsGenerator() {
    stop();
}

void StatsGenerator::initializeROIDistance() {
    try {
        // ROIHandler에서 차선별 길이 정보 로드
        if (roi_handler_) {
            auto all_lengths = roi_handler_->getAllLaneLengths();
            
            if (!all_lengths.empty()) {
                lane_lengths_ = all_lengths;
                
                // 디버깅을 위한 차선별 길이 로그
                logger->info("차선별 실제 길이 로드 완료:");
                for (const auto& [lane, length] : lane_lengths_) {
                    logger->info("  차로 {}: {:.2f}m", lane, length);
                }
                
                // 평균 거리 계산 (폴백용)
                double total_length = 0.0;
                for (const auto& [lane, length] : lane_lengths_) {
                    total_length += length;
                }
                roi_distance_m_ = total_length / lane_lengths_.size();
                logger->info("평균 ROI 길이: {:.2f}m", roi_distance_m_);
                
                return;
            } else {
                logger->warn("ROIHandler에서 차선 길이 정보를 가져올 수 없음");
            }
        }
        
        // ROIHandler가 없거나 차선 길이 정보가 없는 경우 캘리브레이션 거리 사용
        double distance = DISTANCE[0];
        
        if (distance > 0 && distance < 10000) {
            roi_distance_m_ = distance;
            logger->info("캘리브레이션 거리 사용: {}m", roi_distance_m_);
            
            // 모든 차선에 동일한 거리 적용
            for (int lane = 1; lane <= total_lanes_; lane++) {
                lane_lengths_[lane] = roi_distance_m_;
            }
        } else {
            // 기본값 사용
            roi_distance_m_ = DEFAULT_ROI_DISTANCE;
            logger->warn("유효하지 않은 거리값, 기본값 사용: {}m", roi_distance_m_);
            
            // 모든 차선에 기본값 적용
            for (int lane = 1; lane <= total_lanes_; lane++) {
                lane_lengths_[lane] = roi_distance_m_;
            }
        }
        
    } catch (const std::exception& e) {
        roi_distance_m_ = DEFAULT_ROI_DISTANCE;
        logger->error("ROI 거리 로드 실패({}), 기본값 사용: {}m", 
                     e.what(), roi_distance_m_);
        
        // 모든 차선에 기본값 적용
        for (int lane = 1; lane <= total_lanes_; lane++) {
            lane_lengths_[lane] = roi_distance_m_;
        }
    } catch (...) {
        roi_distance_m_ = DEFAULT_ROI_DISTANCE;
        logger->error("ROI 거리 로드 중 알 수 없는 오류, 기본값 사용: {}m", 
                     roi_distance_m_);
        
        // 모든 차선에 기본값 적용
        for (int lane = 1; lane <= total_lanes_; lane++) {
            lane_lengths_[lane] = roi_distance_m_;
        }
    }
}

void StatsGenerator::initialize(RedisClient* redis_client, SQLiteHandler* sqlite_handler,
                              ROIHandler* roi_handler, int total_lanes, int interval_minutes) {
    logger->info("통계 생성기 초기화 - 차로: {}, 인터벌: {}분", total_lanes, interval_minutes);
    
    redis_client_ = redis_client;
    sqlite_handler_ = sqlite_handler;
    roi_handler_ = roi_handler;
    total_lanes_ = total_lanes;
    interval_minutes_ = interval_minutes;
    
    // ConfigManager에서 FPS 값 캐싱 (초기화 시 한 번만)
    try {
        auto& config = ConfigManager::getInstance();
        camera_fps_ = config.getCameraFPS();
        if (camera_fps_ <= 0 || camera_fps_ > 100) {
            camera_fps_ = 15;  // 기본값
            logger->warn("비정상적인 FPS 값, 기본값 사용: {}", camera_fps_);
        }
        logger->info("카메라 FPS 캐싱: {}", camera_fps_);
    } catch (...) {
        camera_fps_ = 15;  // 예외 시 기본값
        logger->error("FPS 로드 실패, 기본값 사용: {}", camera_fps_);
    }
    
    // ROI 거리 초기화 (24/7 안정성)
    initializeROIDistance();
    
    // 프레임 데이터 초기화
    resetFrameData();
    
    // StatsQueryHelper 생성
    if (sqlite_handler_) {
        try {
            query_helper_ = std::make_unique<StatsQueryHelper>(sqlite_handler_);
            logger->info("StatsQueryHelper 초기화 완료");
        } catch (const std::exception& e) {
            logger->error("StatsQueryHelper 생성 실패: {}", e.what());
        }
    } else {
        logger->error("SQLiteHandler가 null이므로 StatsQueryHelper를 생성할 수 없음");
    }
}

void StatsGenerator::start() {
    if (running_.load()) {
        logger->warn("통계 생성기 이미 실행 중");
        return;
    }
    
    running_ = true;
    
    // 인터벌 타이머 스레드 시작
    try {
        interval_thread_ = std::thread(&StatsGenerator::intervalTimerThread, this);
        logger->info("통계 생성기 시작됨");
    } catch (const std::exception& e) {
        running_ = false;
        logger->error("인터벌 스레드 시작 실패: {}", e.what());
    }
}

void StatsGenerator::stop() {
    if (!running_.load()) {
        return;
    }
    
    logger->info("통계 생성기 중지 시작");
    
    running_ = false;
    cv_.notify_all();  // 대기 중인 스레드 즉시 깨우기
    
    // 스레드 종료 대기
    try {
        if (interval_thread_.joinable()) {
            interval_thread_.join();
        }
    } catch (const std::exception& e) {
        logger->error("스레드 종료 중 오류: {}", e.what());
    }
    
    logger->info("통계 생성기 중지 완료");
}

void StatsGenerator::updateFrameData(const std::map<int, int>& lane_counts) {
    try {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        
        // 각 차로별 데이터 업데이트
        for (int lane = 1; lane <= total_lanes_; lane++) {
            int count = 0;
            auto it = lane_counts.find(lane);
            if (it != lane_counts.end()) {
                count = it->second;
            }
            
            // 현재 프레임 데이터 저장
            per_lane_count_[lane] = count;
            
            // 누적 데이터 업데이트
            per_lane_total_[lane] += count;
            
            // 최대값 업데이트
            if (count > per_lane_max_[lane]) {
                per_lane_max_[lane] = count;
            }
            
            // 최소값 업데이트
            if (count < per_lane_min_[lane]) {
                per_lane_min_[lane] = count;
            }
        }
        
        frame_count_++;
        
    } catch (const std::exception& e) {
        logger->error("프레임 데이터 업데이트 중 오류: {}", e.what());
    }
}

void StatsGenerator::resetFrameData() {
    try {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        
        frame_count_ = 0;
        per_lane_count_.clear();
        per_lane_total_.clear();
        per_lane_max_.clear();
        per_lane_min_.clear();
        
        // 초기화
        for (int lane = 1; lane <= total_lanes_; lane++) {
            per_lane_count_[lane] = 0;
            per_lane_total_[lane] = 0;
            per_lane_max_[lane] = 0;
            per_lane_min_[lane] = INT_MAX;
        }
    } catch (const std::exception& e) {
        logger->error("프레임 데이터 리셋 중 오류: {}", e.what());
    }
}

int StatsGenerator::calculateNextIntervalTime(int current_time) const {
    // 현재 시간을 tm 구조체로 변환
    std::time_t time_t_current = static_cast<std::time_t>(current_time);
    std::tm* tm_current = std::localtime(&time_t_current);
    
    int current_minute = tm_current->tm_min;
    int current_second = tm_current->tm_sec;
    
    // 다음 인터벌 시간 계산 (정각 기준)
    int minutes_to_next = interval_minutes_ - (current_minute % interval_minutes_);
    if (minutes_to_next == interval_minutes_) {
        minutes_to_next = 0;  // 이미 정확한 인터벌 시간인 경우
    }
    
    // 다음 인터벌까지의 초 계산
    int seconds_to_next = (minutes_to_next * 60) - current_second;
    
    // 0초 이하인 경우 다음 인터벌로
    if (seconds_to_next <= 0) {
        seconds_to_next += (interval_minutes_ * 60);
    }
    
    int next_time = current_time + seconds_to_next;
    
    // 디버깅 로그
    logger->trace("다음 인터벌 계산 - 현재: {}:{:02d}, 다음까지: {}초, 다음 시간: {}", 
                 tm_current->tm_hour, current_minute, seconds_to_next, next_time);
    
    return next_time;
}

void StatsGenerator::intervalTimerThread() {
    logger->info("인터벌 타이머 스레드 시작 ({}분 주기)", interval_minutes_);
    
    // 첫 실행: 다음 인터벌까지 대기
    {
        int current_time = std::time(nullptr);
        int next_interval = calculateNextIntervalTime(current_time);
        int wait_seconds = next_interval - current_time;
        
        std::time_t next_time_t = static_cast<std::time_t>(next_interval);
        std::tm* tm_next = std::localtime(&next_time_t);
        
        logger->info("첫 인터벌 통계 생성 예정 시간: {:02d}:{:02d} ({}초 후)", 
                    tm_next->tm_hour, tm_next->tm_min, wait_seconds);
        
        // 첫 인터벌까지 대기
        std::unique_lock<std::mutex> lock(cv_mutex_);
        if (cv_.wait_for(lock, std::chrono::seconds(wait_seconds), 
                        [this]() { return !running_.load(); })) {
            // 중단 신호를 받은 경우
            logger->info("인터벌 타이머 스레드 조기 종료");
            return;
        }
        
        // 첫 통계 생성
        if (running_.load()) {
            logger->info("첫 인터벌 통계 생성 시작 (인터벌 정렬 완료)");
            generateIntervalStats();
        }
    }
    
    while (running_.load()) {
        try {
            // 정확한 인터벌만큼 대기 (이미 정각에 맞춰져 있음)
            std::unique_lock<std::mutex> lock(cv_mutex_);
            auto wait_result = cv_.wait_for(lock, std::chrono::minutes(interval_minutes_), 
                                           [this]() { return !running_.load(); });
            
            if (!running_.load()) {
                break;
            }
            
            // 시간이 만료되면 인터벌 통계 생성
            if (!wait_result) {
                logger->info("인터벌 타이머 트리거 - 통계 생성 시작");
                generateIntervalStats();
            }
        } catch (const std::exception& e) {
            logger->error("인터벌 타이머 스레드 오류: {}", e.what());
            // 오류 발생해도 계속 실행 (24/7 안정성)
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    }
    
    logger->info("인터벌 타이머 스레드 종료");
}

bool StatsGenerator::generateIntervalStats() {
    try {
        int current_time = std::time(nullptr);
        int start_time = current_time - (interval_minutes_ * 60);
        
        logger->info("인터벌 통계 생성 시작 - 기간: {} ~ {}", start_time, current_time);
        
        StatsDataPacket stats = generateStatistics(StatsType::STATS_INTERVAL, start_time, current_time);
        
        if (validateStats(stats)) {
            logStats(stats);
            bool result = sendToRedis(stats);
            
            // 통계 생성 후 프레임 데이터 리셋
            resetFrameData();
            
            return result;
        } else {
            logger->warn("인터벌 통계 검증 실패");
            return false;
        }
        
    } catch (const std::exception& e) {
        logger->error("인터벌 통계 생성 중 예외: {}", e.what());
        return false;
    } catch (...) {
        logger->error("인터벌 통계 생성 중 알 수 없는 예외");
        return false;
    }
}

void StatsGenerator::onSignalChange(const SignalChangeEvent& event) {
    if (event.type == SignalChangeEvent::Type::GREEN_ON) {
        try {
            int current_time = std::time(nullptr);
            int start_time = last_signal_stats_time_ > 0 ? 
                            last_signal_stats_time_ : current_time - 300;
            
            logger->info("신호현시 통계 생성 시작 - 기간: {} ~ {}", start_time, current_time);
            
            StatsDataPacket stats = generateStatistics(StatsType::STATS_SIGNAL_PHASE, start_time, current_time);
            
            if (validateStats(stats)) {
                logStats(stats);
                sendToRedis(stats);
                
                // 통계 생성 후 프레임 데이터 리셋
                resetFrameData();
            } else {
                logger->warn("신호현시 통계 검증 실패");
            }
            
            last_signal_stats_time_ = current_time;
            
        } catch (const std::exception& e) {
            logger->error("신호현시 통계 생성 중 예외: {}", e.what());
        } catch (...) {
            logger->error("신호현시 통계 생성 중 알 수 없는 예외");
        }
    }
}

StatsDataPacket StatsGenerator::generateStatistics(StatsType type, int start_time, int end_time) const {
    StatsDataPacket packet;
    packet.type = type;
    
    try {
        // 프레임 기반 밀도 계산 (차선별 거리 반영)
        std::map<int, DensityInfo> density = calculateDensity(end_time - start_time);
        
        // 각 통계 생성
        packet.approach = generateApproachStats(type, start_time, end_time, density);
        packet.turn_types = generateTurnTypeStats(type, start_time, end_time);
        packet.vehicle_types = generateVehicleTypeStats(type, start_time, end_time);
        packet.lanes = generateLaneStats(type, start_time, end_time, density);
        
        packet.is_valid = true;
        
    } catch (const std::exception& e) {
        logger->error("통계 생성 중 오류: {}", e.what());
        packet.is_valid = false;
    }
    
    return packet;
}

std::map<int, DensityInfo> StatsGenerator::calculateDensity(int time_window_sec) const {
    std::map<int, DensityInfo> densities;
    
    try {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        
        // 캐싱된 FPS 값 사용
        int fps = camera_fps_;
        
        // 총 프레임 수 계산
        int expected_frames = time_window_sec * fps;
        int actual_frames = (frame_count_ > 0) ? frame_count_ : expected_frames;
        
        // 전체 차로의 총 차량 수 계산
        int total_vehicles_all_lanes = 0;
        for (int lane = 1; lane <= total_lanes_; lane++) {
            total_vehicles_all_lanes += per_lane_total_.at(lane);
        }
        
        logger->debug("밀도 계산 - 시간창: {}초, FPS: {}, 실제프레임: {}", 
                     time_window_sec, fps, actual_frames);
        
        // 각 차로별 밀도 정보 계산
        for (int lane = 1; lane <= total_lanes_; lane++) {
            DensityInfo info;
            
            // 차선별 실제 거리 가져오기
            double lane_distance = roi_distance_m_;  // 기본값
            auto it = lane_lengths_.find(lane);
            if (it != lane_lengths_.end() && it->second > 0) {
                lane_distance = it->second;
            } else {
                logger->warn("차로 {} 길이 정보 없음, 기본값 사용: {}m", lane, lane_distance);
            }
            
            // 거리 변환 계수 (m -> km)
            double distance_factor = 1000.0 / lane_distance;
            
            // 평균 밀도 계산 - 거리 기반 변환 (대/km)
            if (actual_frames > 0) {
                // 프레임당 평균 차량 수
                double avg_vehicles_per_frame = 
                    static_cast<double>(per_lane_total_.at(lane)) / actual_frames;
                
                // km당 밀도로 변환
                info.avg_density = static_cast<int>(
                    std::round(avg_vehicles_per_frame * distance_factor)
                );
            } else {
                info.avg_density = 0;
            }
            
            // 최소 밀도 - km당 변환
            if (per_lane_min_.at(lane) == INT_MAX) {
                info.min_density = 0;  // 차량이 한 번도 없었던 경우
            } else {
                info.min_density = static_cast<int>(
                    std::round(per_lane_min_.at(lane) * distance_factor)
                );
            }
            
            // 최대 밀도 - km당 변환
            info.max_density = static_cast<int>(
                std::round(per_lane_max_.at(lane) * distance_factor)
            );
            
            // 차로별 교통량 점유율
            if (total_vehicles_all_lanes > 0) {
                info.occupancy_rate = (static_cast<double>(per_lane_total_.at(lane)) / 
                                      total_vehicles_all_lanes) * 100.0;
            } else {
                info.occupancy_rate = 0.0;
            }
            
            densities[lane] = info;
            
            logger->debug("차로 {} - 밀도(평균/최소/최대): {}/{}/{} 대/km, 점유율: {:.2f}%",
                         lane, 
                         info.avg_density, info.min_density, info.max_density,
                         info.occupancy_rate);
        }
        
    } catch (const std::exception& e) {
        logger->error("밀도 계산 중 오류: {}", e.what());
        // 오류 시 빈 밀도 정보 반환
        for (int lane = 1; lane <= total_lanes_; lane++) {
            densities[lane] = DensityInfo();
        }
    }
    
    return densities;
}

ApproachStats StatsGenerator::generateApproachStats(StatsType type, int start_time, int end_time,
                                                   const std::map<int, DensityInfo>& density) const {
    ApproachStats stats;
    
    if (!query_helper_) {
        logger->error("StatsQueryHelper가 초기화되지 않음");
        return stats;
    }
    
    try {
        stats.hr_type_cd = static_cast<int>(type);
        stats.stats_bgng_unix_tm = start_time;
        stats.stats_end_unix_tm = end_time;
        
        // 전체 통행량
        stats.totl_trvl = query_helper_->getTotalVehicleCount(start_time, end_time);
        
        // 평균 속도
        stats.avg_stln_dttn_sped = query_helper_->getTotalAverageStopLineSpeed(start_time, end_time);
        stats.avg_sect_sped = query_helper_->getTotalAverageIntervalSpeed(start_time, end_time);
        
        // 전체 차로의 평균 밀도 계산 (대/km/차선)
        int total_avg_density = 0;
        int total_min_density = 0;
        int total_max_density = 0;
        double total_occupancy = 0.0;
        int valid_lanes = 0;
        
        for (const auto& [lane, info] : density) {
            if (lane >= 1 && lane <= total_lanes_) {
                total_avg_density += info.avg_density;
                total_min_density += info.min_density;
                total_max_density += info.max_density;
                total_occupancy += info.occupancy_rate;
                valid_lanes++;
            }
        }
        
        if (valid_lanes > 0) {
            stats.avg_trfc_dnst = total_avg_density / valid_lanes;
            stats.min_trfc_dnst = total_min_density / valid_lanes;
            stats.max_trfc_dnst = total_max_density / valid_lanes;
            
            // 평균 차로 점유율
            stats.avg_lane_ocpn_rt = total_occupancy / valid_lanes;
        } else {
            stats.avg_trfc_dnst = 0;
            stats.min_trfc_dnst = 0;
            stats.max_trfc_dnst = 0;
            stats.avg_lane_ocpn_rt = 0.0;
        }
        
        stats.is_valid = (stats.totl_trvl > 0);
        
    } catch (const std::exception& e) {
        logger->error("접근로별 통계 생성 중 오류: {}", e.what());
    }
    
    return stats;
}

std::vector<TurnTypeStats> StatsGenerator::generateTurnTypeStats(StatsType type, int start_time, int end_time) const {
    std::vector<TurnTypeStats> results;
    
    if (!query_helper_) {
        logger->error("StatsQueryHelper가 초기화되지 않음");
        return results;
    }
    
    try {
        // 각 회전 방향별 통계 생성
        for (int turn : STATS_TURN_TYPES) {
            TurnTypeStats stats;
            stats.turn_type_cd = turn;
            stats.hr_type_cd = static_cast<int>(type);
            stats.stats_bgng_unix_tm = start_time;
            stats.stats_end_unix_tm = end_time;
            
            // 차종별 교통량 조회 (서버 DB KNCR 순서대로)
            stats.kncr1_trvl = query_helper_->getVehicleCountByTurnAndType(start_time, end_time, turn, KNCR_MAPPING[0]);  // MBUS
            stats.kncr2_trvl = query_helper_->getVehicleCountByTurnAndType(start_time, end_time, turn, KNCR_MAPPING[1]);  // LBUS
            stats.kncr3_trvl = query_helper_->getVehicleCountByTurnAndType(start_time, end_time, turn, KNCR_MAPPING[2]);  // PCAR
            stats.kncr4_trvl = query_helper_->getVehicleCountByTurnAndType(start_time, end_time, turn, KNCR_MAPPING[3]);  // MOTOR
            stats.kncr5_trvl = query_helper_->getVehicleCountByTurnAndType(start_time, end_time, turn, KNCR_MAPPING[4]);  // MTRUCK
            stats.kncr6_trvl = query_helper_->getVehicleCountByTurnAndType(start_time, end_time, turn, KNCR_MAPPING[5]);  // LTRUCK
            
            // 전체 교통량 계산
            stats.totl_trvl = stats.kncr1_trvl + stats.kncr2_trvl + stats.kncr3_trvl + 
                             stats.kncr4_trvl + stats.kncr5_trvl + stats.kncr6_trvl;
            
            // 평균 속도 (전체)
            stats.avg_stln_dttn_sped = query_helper_->getAverageStopLineSpeedByTurn(start_time, end_time, turn);
            stats.avg_sect_sped = query_helper_->getAverageIntervalSpeedByTurn(start_time, end_time, turn);
            
            if (stats.totl_trvl > 0) {
                stats.is_valid = true;
                results.push_back(stats);
            }
        }
        
    } catch (const std::exception& e) {
        logger->error("회전별 통계 생성 중 오류: {}", e.what());
    }
    
    return results;
}

std::vector<VehicleTypeStats> StatsGenerator::generateVehicleTypeStats(StatsType type, int start_time, int end_time) const {
    std::vector<VehicleTypeStats> results;
    
    if (!query_helper_) {
        logger->error("StatsQueryHelper가 초기화되지 않음");
        return results;
    }
    
    try {
        // 각 차종별 통계 생성
        for (const auto& kncr : STATS_VEHICLE_TYPES) {
            VehicleTypeStats stats;
            stats.kncr_cd = kncr;
            stats.hr_type_cd = static_cast<int>(type);
            stats.stats_bgng_unix_tm = start_time;
            stats.stats_end_unix_tm = end_time;
            
            // StatsQueryHelper 사용
            stats.totl_trvl = query_helper_->getVehicleCountByType(start_time, end_time, kncr);
            stats.avg_stln_dttn_sped = query_helper_->getAverageStopLineSpeedByType(start_time, end_time, kncr);
            stats.avg_sect_sped = query_helper_->getAverageIntervalSpeedByType(start_time, end_time, kncr);
            
            if (stats.totl_trvl > 0) {
                stats.is_valid = true;
                results.push_back(stats);
            }
        }
        
    } catch (const std::exception& e) {
        logger->error("차종별 통계 생성 중 오류: {}", e.what());
    }
    
    return results;
}

std::vector<LaneStats> StatsGenerator::generateLaneStats(StatsType type, int start_time, int end_time,
                                                        const std::map<int, DensityInfo>& density) const {
    std::vector<LaneStats> results;
    
    if (!query_helper_) {
        logger->error("StatsQueryHelper가 초기화되지 않음");
        return results;
    }
    
    try {
        // 각 차로별 통계 생성
        for (int lane = 1; lane <= total_lanes_; lane++) {
            LaneStats stats;
            stats.lane_no = lane;
            stats.hr_type_cd = static_cast<int>(type);
            stats.stats_bgng_unix_tm = start_time;
            stats.stats_end_unix_tm = end_time;
            
            // StatsQueryHelper 사용
            stats.totl_trvl = query_helper_->getVehicleCountByLane(start_time, end_time, lane);
            stats.avg_stln_dttn_sped = query_helper_->getAverageStopLineSpeedByLane(start_time, end_time, lane);
            stats.avg_sect_sped = query_helper_->getAverageIntervalSpeedByLane(start_time, end_time, lane);
            
            // 거리 기반 밀도 정보 (대/km)
            auto it = density.find(lane);
            if (it != density.end()) {
                stats.avg_trfc_dnst = it->second.avg_density;
                stats.min_trfc_dnst = it->second.min_density;
                stats.max_trfc_dnst = it->second.max_density;
                stats.ocpn_rt = it->second.occupancy_rate;  // 차로별 교통량 점유율
            } else {
                stats.avg_trfc_dnst = 0;
                stats.min_trfc_dnst = 0;
                stats.max_trfc_dnst = 0;
                stats.ocpn_rt = 0.0;
            }
            
            if (stats.totl_trvl > 0) {
                stats.is_valid = true;
                results.push_back(stats);
            }
        }
        
    } catch (const std::exception& e) {
        logger->error("차로별 통계 생성 중 오류: {}", e.what());
    }
    
    return results;
}

bool StatsGenerator::sendToRedis(const StatsDataPacket& stats) const {
    if (!redis_client_ || !redis_client_->isConnected()) {
        logger->error("Redis 클라이언트가 연결되지 않음");
        return false;
    }
    
    try {
        // JSON 형식으로 변환
        std::stringstream json_data;
        json_data << "{";
        
        // 접근로별 통계
        if (stats.approach.is_valid) {
            json_data << "\"approach\":{";
            json_data << "\"hr_type_cd\":" << stats.approach.hr_type_cd << ",";
            json_data << "\"stats_bgng_unix_tm\":" << stats.approach.stats_bgng_unix_tm << ",";
            json_data << "\"stats_end_unix_tm\":" << stats.approach.stats_end_unix_tm << ",";
            json_data << "\"totl_trvl\":" << stats.approach.totl_trvl << ",";
            json_data << "\"avg_stln_dttn_sped\":" << std::fixed << std::setprecision(2) << stats.approach.avg_stln_dttn_sped << ",";
            json_data << "\"avg_sect_sped\":" << stats.approach.avg_sect_sped << ",";
            json_data << "\"avg_trfc_dnst\":" << stats.approach.avg_trfc_dnst << ",";
            json_data << "\"min_trfc_dnst\":" << stats.approach.min_trfc_dnst << ",";
            json_data << "\"max_trfc_dnst\":" << stats.approach.max_trfc_dnst << ",";
            json_data << "\"avg_lane_ocpn_rt\":" << std::setprecision(2) << stats.approach.avg_lane_ocpn_rt;
            json_data << "},";
        }
        
        // 회전별 통계
        json_data << "\"turn_types\":[";
        for (size_t i = 0; i < stats.turn_types.size(); i++) {
            const auto& turn = stats.turn_types[i];
            json_data << "{";
            json_data << "\"turn_type_cd\":" << turn.turn_type_cd << ",";
            json_data << "\"hr_type_cd\":" << turn.hr_type_cd << ",";
            json_data << "\"stats_bgng_unix_tm\":" << turn.stats_bgng_unix_tm << ",";
            json_data << "\"stats_end_unix_tm\":" << turn.stats_end_unix_tm << ",";
            json_data << "\"kncr1_trvl\":" << turn.kncr1_trvl << ",";
            json_data << "\"kncr2_trvl\":" << turn.kncr2_trvl << ",";
            json_data << "\"kncr3_trvl\":" << turn.kncr3_trvl << ",";
            json_data << "\"kncr4_trvl\":" << turn.kncr4_trvl << ",";
            json_data << "\"kncr5_trvl\":" << turn.kncr5_trvl << ",";
            json_data << "\"kncr6_trvl\":" << turn.kncr6_trvl << ",";
            json_data << "\"totl_trvl\":" << turn.totl_trvl << ",";
            json_data << "\"avg_stln_dttn_sped\":" << std::fixed << std::setprecision(2) << turn.avg_stln_dttn_sped << ",";
            json_data << "\"avg_sect_sped\":" << turn.avg_sect_sped;
            json_data << "}";
            if (i < stats.turn_types.size() - 1) json_data << ",";
        }
        json_data << "],";
        
        // 차종별 통계
        json_data << "\"vehicle_types\":[";
        for (size_t i = 0; i < stats.vehicle_types.size(); i++) {
            const auto& vehicle = stats.vehicle_types[i];
            json_data << "{";
            json_data << "\"kncr_cd\":\"" << vehicle.kncr_cd << "\",";
            json_data << "\"hr_type_cd\":" << vehicle.hr_type_cd << ",";
            json_data << "\"stats_bgng_unix_tm\":" << vehicle.stats_bgng_unix_tm << ",";
            json_data << "\"stats_end_unix_tm\":" << vehicle.stats_end_unix_tm << ",";
            json_data << "\"totl_trvl\":" << vehicle.totl_trvl << ",";
            json_data << "\"avg_stln_dttn_sped\":" << std::fixed << std::setprecision(2) << vehicle.avg_stln_dttn_sped << ",";
            json_data << "\"avg_sect_sped\":" << vehicle.avg_sect_sped;
            json_data << "}";
            if (i < stats.vehicle_types.size() - 1) json_data << ",";
        }
        json_data << "],";
        
        // 차로별 통계
        json_data << "\"lanes\":[";
        for (size_t i = 0; i < stats.lanes.size(); i++) {
            const auto& lane = stats.lanes[i];
            json_data << "{";
            json_data << "\"lane_no\":" << lane.lane_no << ",";
            json_data << "\"hr_type_cd\":" << lane.hr_type_cd << ",";
            json_data << "\"stats_bgng_unix_tm\":" << lane.stats_bgng_unix_tm << ",";
            json_data << "\"stats_end_unix_tm\":" << lane.stats_end_unix_tm << ",";
            json_data << "\"totl_trvl\":" << lane.totl_trvl << ",";
            json_data << "\"avg_stln_dttn_sped\":" << std::fixed << std::setprecision(2) << lane.avg_stln_dttn_sped << ",";
            json_data << "\"avg_sect_sped\":" << lane.avg_sect_sped << ",";
            json_data << "\"avg_trfc_dnst\":" << lane.avg_trfc_dnst << ",";
            json_data << "\"min_trfc_dnst\":" << lane.min_trfc_dnst << ",";
            json_data << "\"max_trfc_dnst\":" << lane.max_trfc_dnst << ",";
            json_data << "\"ocpn_rt\":" << std::setprecision(2) << lane.ocpn_rt;
            json_data << "}";
            if (i < stats.lanes.size() - 1) json_data << ",";
        }
        json_data << "]";
        
        json_data << "}";
        
        // Redis로 전송
        int result = redis_client_->sendData(CHANNEL_STATS, json_data.str());
        
        if (result == 0) {
            logger->info("{} 통계 Redis 전송 성공 ({}바이트)", 
                        stats.type == StatsType::STATS_INTERVAL ? "인터벌" : "신호현시",
                        json_data.str().length());
            return true;
        } else {
            logger->error("Redis 전송 실패: {}", result);
            return false;
        }
        
    } catch (const std::exception& e) {
        logger->error("Redis 전송 중 예외 발생: {}", e.what());
        return false;
    }

    return true;
}

bool StatsGenerator::validateStats(const StatsDataPacket& stats) const {
    // 기본 검증
    if (!stats.is_valid) {
        logger->error("통계 패킷이 유효하지 않음");
        return false;
    }
    
    // 접근로별 통계 검증
    if (!stats.approach.is_valid) {
        logger->warn("접근로별 통계가 유효하지 않음");
    }
    
    // 최소 하나의 통계는 있어야 함
    if (stats.turn_types.empty() && stats.vehicle_types.empty() && stats.lanes.empty()) {
        logger->error("모든 통계가 비어있음");
        return false;
    }
    
    return true;
}

void StatsGenerator::logStats(const StatsDataPacket& stats) const {
    try {
        std::string type_str = stats.type == StatsType::STATS_INTERVAL ? "인터벌" : "신호현시";
        
        logger->info("===== {} 통계 생성 완료 =====", type_str);
        logger->info("기간: {} ~ {}", stats.approach.stats_bgng_unix_tm, stats.approach.stats_end_unix_tm);
        
        // 접근로 통계 로깅
        logger->info("접근로 - 통행량: {}, 평균속도: {:.2f}km/h, 평균밀도: {}대/km, 최소밀도: {}대/km, 최대밀도: {}대/km, 평균차로점유율: {:.2f}%", 
                    stats.approach.totl_trvl, 
                    stats.approach.avg_sect_sped, 
                    stats.approach.avg_trfc_dnst,
                    stats.approach.min_trfc_dnst,
                    stats.approach.max_trfc_dnst,
                    stats.approach.avg_lane_ocpn_rt);
        
        // 차로별 통계
        double total_share = 0.0;
        int lanes_with_traffic = 0;
        
        for (const auto& lane : stats.lanes) {
            // 차선별 길이 정보 포함
            double lane_length = roi_distance_m_;
            auto it = lane_lengths_.find(lane.lane_no);
            if (it != lane_lengths_.end()) {
                lane_length = it->second;
            }
            
            logger->info("차로 {} (길이: {:.1f}m) - 통행량: {}, 평균속도: {:.2f}km/h, 평균밀도: {}대/km, 최소밀도: {}대/km, 최대밀도: {}대/km, 점유율: {:.2f}%", 
                        lane.lane_no, 
                        lane_length,
                        lane.totl_trvl, 
                        lane.avg_sect_sped, 
                        lane.avg_trfc_dnst,
                        lane.min_trfc_dnst,
                        lane.max_trfc_dnst,
                        lane.ocpn_rt);
            total_share += lane.ocpn_rt;
            lanes_with_traffic++;
        }
        
        // 차로별 점유율 합계 로그 개선
        if (lanes_with_traffic < total_lanes_) {
            logger->debug("차로별 점유율 합계: {:.2f}% (전체 {}개 차로 중 {}개 차로에서만 차량 검출)", 
                         total_share, total_lanes_, lanes_with_traffic);
            
            // 차량이 검출되지 않은 차로 표시
            for (int lane = 1; lane <= total_lanes_; lane++) {
                bool found = false;
                for (const auto& lane_stat : stats.lanes) {
                    if (lane_stat.lane_no == lane) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    logger->debug("  차로 {}: 차량 미검출", lane);
                }
            }
        } else {
            logger->debug("차로별 점유율 합계: {:.2f}% (전체 {}개 차로)", 
                         total_share, total_lanes_);
        }
        
        // 회전별 통계
        for (const auto& turn : stats.turn_types) {
            std::string turn_name;
            switch (turn.turn_type_cd) {
                case 11: turn_name = "직진"; break;
                case 21: case 22: turn_name = "좌회전"; break;
                case 31: case 32: turn_name = "우회전"; break;
                case 41: turn_name = "유턴"; break;
                case -11: turn_name = "역방향직진"; break;
                case -21: case -22: turn_name = "역방향좌회전"; break;
                case -31: case -32: turn_name = "역방향우회전"; break;
                case -41: turn_name = "역방향유턴"; break;
                default: turn_name = "기타"; break;
            }
            logger->info("{} - 총통행량: {}, 평균속도: {:.2f}km/h (MBUS:{}, LBUS:{}, PCAR:{}, MOTOR:{}, MTRUCK:{}, LTRUCK:{})", 
                        turn_name, turn.totl_trvl, turn.avg_sect_sped,
                        turn.kncr1_trvl, turn.kncr2_trvl, turn.kncr3_trvl,
                        turn.kncr4_trvl, turn.kncr5_trvl, turn.kncr6_trvl);
        }
        
        // 차종별 통계
        for (const auto& vehicle : stats.vehicle_types) {
            logger->info("차종 {} - 통행량: {}, 평균속도: {:.2f}km/h", 
                        vehicle.kncr_cd, vehicle.totl_trvl, vehicle.avg_sect_sped);
        }
    } catch (const std::exception& e) {
        logger->error("통계 로깅 중 오류: {}", e.what());
    }
}