/*
 * system_manager.cpp
 * 
 * ITS 전체 시스템 통합 관리 클래스 구현
 * - 모든 모듈의 초기화 및 생명주기 관리
 * - Redis/SQLite 통합 관리
 * - 신호 변경 이벤트를 StatsGenerator, QueueAnalyzer, IncidentDetector로 전달
 * - Presence 모듈 독립적 관리
 */

#include "system_manager.h"
#include "../../analytics/queue/queue_analyzer.h"
#include "../../image/image_cropper.h"
#include "../../image/image_storage.h"
#include "../../monitoring/car_presence.h"
#include "../../monitoring/pedestrian_presence.h"
#include "../../utils/config_manager.h"
#include <chrono>

SystemManager::SystemManager() {
    logger = getLogger("DS_SystemManager_log");
    logger->info("SystemManager 생성");
}

SystemManager::~SystemManager() {
    stop();
}

bool SystemManager::initialize(const std::string& config_path, 
                              ROIHandler* roi_handler,
                              ImageCropper* image_cropper,
                              ImageStorage* image_storage) {
    logger->info("시스템 매니저 초기화 시작: {}", config_path);
    
    try {
        // Config는 이미 main에서 로드되었으므로 getInstance()로 접근
        logger->debug("Config 인스턴스 가져오기");
        auto& config = ConfigManager::getInstance();
        logger->info("Config 인스턴스 가져오기 성공");
        
        // ROI Handler 저장
        roi_handler_ = roi_handler;
        logger->info("ROI Handler 설정 완료");
        
        // ====== 1단계: 기반 인프라 초기화 ======
        
        // 1-1. Redis 클라이언트 초기화
        redis_client_ = std::make_unique<RedisClient>();
        if (!redis_client_->isConnected()) {
            logger->error("Redis 연결 실패");
            return false;
        }
        logger->info("Redis 연결 성공");
        
        // 1-2. SQLite 핸들러 초기화
        sqlite_handler_ = std::make_unique<SQLiteHandler>();
        if (!sqlite_handler_->isHealthy()) {
            logger->error("SQLite 초기화 실패");
            return false;
        }
        logger->info("SQLite 초기화 성공");
        
        // 1-3. 사이트 정보 매니저 초기화
        site_info_mgr_ = std::make_unique<SiteInfoManager>();
        if (!site_info_mgr_->initialize(config_path)) {
            logger->error("사이트 정보 매니저 초기화 실패");
            return false;
        }
        
        site_info_ = site_info_mgr_->getSiteInfo();
        logger->info("사이트 정보 초기화 완료 - CAM ID: {}", site_info_.spot_camr_id);
        
        // ====== 2단계: Presence 모듈 초기화 (독립적 운영) ======
        
        // 2-1. 차량 Presence 모듈
        if (config.isVehiclePresenceEnabled()) {
            if (roi_handler_) {
                car_presence_ = std::make_unique<CarPresence>(*roi_handler_, *redis_client_);
                if (car_presence_->initialize()) {
                    logger->info("차량 Presence 모듈 초기화 성공");
                } else {
                    logger->warn("차량 Presence 모듈 초기화 실패 - 비활성화");
                    car_presence_.reset();
                }
            } else {
                logger->warn("ROI Handler 없음 - 차량 Presence 비활성화");
            }
        } else {
            logger->info("차량 Presence 모듈 비활성 (config.json에서 false로 설정됨)");
        }
        
        // 2-2. 보행자 Presence 모듈
        if (config.isPedestrianPresenceEnabled()) {
            if (roi_handler_) {
                ped_presence_ = std::make_unique<PedestrianPresence>(*roi_handler_, *redis_client_);
                if (ped_presence_->initialize()) {
                    logger->info("보행자 Presence 모듈 초기화 성공");
                } else {
                    logger->warn("보행자 Presence 모듈 초기화 실패 - 비활성화");
                    ped_presence_.reset();
                }
            } else {
                logger->warn("ROI Handler 없음 - 보행자 Presence 비활성화");
            }
        } else {
            logger->info("보행자 Presence 모듈 비활성 (config.json에서 false로 설정됨)");
        }
        
        // ====== 3단계: 분석 모듈 생성 (순서 무관) ======
        
        // 3-1. Special Site 어댑터 생성 (신호 계산기 연결 전)
        if (config.isSpecialSiteEnabled() && config.isVehicle2KEnabled() && !config.isVehicle4KEnabled()) {
            special_site_adapter_ = std::make_unique<SpecialSiteAdapter>(nullptr, roi_handler_);
            if (special_site_adapter_->initialize()) {
                logger->info("Special Site 어댑터 초기화 성공");
                
                // 통계와 대기행렬이 자동 비활성화되어야 함
                if (config.isStatisticsEnabled() || config.isWaitQueueEnabled()) {
                    logger->warn("Special Site 모드에서 통계/대기행렬은 자동 비활성화");
                }
            } else {
                logger->error("Special Site 어댑터 초기화 실패");
                special_site_adapter_.reset();
            }
        }
        
        // 3-2. 대기행렬 분석기 생성
        if (config.isWaitQueueEnabled()) {
            // Special Site 모드에서는 대기행렬 분석 비활성화
            if (special_site_adapter_ && special_site_adapter_->isActive()) {
                logger->info("Special Site 모드 활성화로 대기행렬 분석기 비활성화");
            } else {
                queue_analyzer_ = std::make_unique<QueueAnalyzer>();
                if (!queue_analyzer_->initialize(redis_client_.get())) {
                    logger->error("대기행렬 분석기 초기화 실패");
                    return false;
                }
                logger->info("대기행렬 분석기 초기화 성공");
            }
        } else {
            if (!config.isVehicle2KEnabled()) {
                logger->info("대기행렬 분석기 비활성 (차량 2K 비활성으로 인한 강제 비활성화)");
            } else {
                if (special_site_adapter_ && special_site_adapter_->isActive()) {
                    logger->info("대기행렬 분석기 비활성 (Special Site 모드 활성화로 인한 자동 비활성화)");
                } else {
                    logger->info("대기행렬 분석기 비활성 (config.json에서 false로 설정됨)");
                } 
            }
        }
        
        // 3-3. 돌발상황 감지기 생성
        if (config.isIncidentEventEnabled()) {
            incident_detector_ = std::make_unique<IncidentDetector>();
            if (!incident_detector_->initialize(roi_handler_, redis_client_.get(),
                                                image_cropper, image_storage)) {
                logger->error("돌발상황 감지기 초기화 실패");
                return false;
            }
            
            // 개별 활성화 상태 로그
            std::vector<std::string> enabled_types;
            if (config.isReverseDrivingEnabled()) enabled_types.push_back("역주행");
            if (config.isAbnormalStopEnabled()) enabled_types.push_back("차량정지-꼬리물기-사고");
            if (config.isPedestrianJaywalkEnabled()) enabled_types.push_back("무단횡단");
            
            std::string enabled_str;
            for (size_t i = 0; i < enabled_types.size(); ++i) {
                if (i > 0) enabled_str += ", ";
                enabled_str += enabled_types[i];
            }
            logger->info("돌발상황 감지기 초기화 성공 - 활성화: [{}]", enabled_str);
        } else {
            if (!config.isVehicle2KEnabled()) {
                logger->info("돌발상황 감지기 비활성 (차량 2K 비활성으로 인한 강제 비활성화)");
            } else {
                logger->info("돌발상황 감지기 비활성 (모든 돌발 이벤트가 false)");
            }
        }
        
        // ====== 4단계: 이미지 캡처 핸들러 초기화 및 연결 ======
        
        if (image_cropper && image_storage) {
            logger->debug("ImageCaptureHandler 생성 시작");
            image_capture_handler_ = std::make_unique<ImageCaptureHandler>();
            logger->debug("ImageCaptureHandler 생성 완료");

            // 필수 컴포넌트만으로 초기화 (1회만)
            if (!image_capture_handler_->initialize(image_cropper, image_storage)) {
                logger->error("이미지 캡처 핸들러 초기화 실패");
                return false;
            }
            logger->info("이미지 캡처 핸들러 초기화 완료 (대기행렬 전용)");
            
            // Setter로 분석 모듈 연결
            if (queue_analyzer_) {
                image_capture_handler_->setQueueAnalyzer(queue_analyzer_.get());
                logger->debug("ImageCaptureHandler에 QueueAnalyzer 연결 완료");
            }
            
        } else {
            logger->warn("ImageCropper 또는 ImageStorage가 제공되지 않음 - 이미지 캡처 비활성화");
        }
        
        // ====== 5단계: 통계 및 신호 처리 모듈 ======
        
        // 5-1. 통계 생성기 초기화
        if (config.isStatisticsEnabled()) {
            // Special Site 모드에서는 통계 생성 비활성화
            if (special_site_adapter_ && special_site_adapter_->isActive()) {
                logger->info("Special Site 모드 활성화로 통계 생성기 비활성화");
            } else {
                stats_gen_ = std::make_unique<StatsGenerator>();
                
                // 차로 수 결정
                int total_lanes = 0;
                if (roi_handler_ && !roi_handler_->lane_roi.empty()) {
                    // ROIHandler에서 차로 수 가져오기 (lane_roi 맵의 크기)
                    total_lanes = roi_handler_->lane_roi.size();
                    logger->info("ROIHandler에서 차로 수 획득: {} 차로", total_lanes);
                } else {
                    // ROI가 없으면 기본값 4차로 사용
                    total_lanes = 4;
                    logger->warn("ROIHandler에서 차로 정보를 가져올 수 없음. 기본값 {} 차로 사용", total_lanes);
                    logger->warn("ROI 파일 확인 필요. 통계는 기본 차로값으로 계속 생성");
                }
                
                // ConfigManager에서 검증된 인터벌 값 가져오기
                int interval_minutes = config.getStatsIntervalMinutes();
                
                // Redis와 SQLite 핸들러, ROIHandler 전달
                stats_gen_->initialize(redis_client_.get(), sqlite_handler_.get(), 
                                    roi_handler_, total_lanes, interval_minutes);
                logger->info("통계 생성기 초기화 완료 - 차로: {}, 인터벌: {}분", 
                            total_lanes, interval_minutes);
                logger->info("인터벌 통계 활성화");
            }
        } else {
            if (!config.isVehicle2KEnabled()) {
                logger->info("통계 생성기 비활성 (차량 2K 비활성으로 인한 강제 비활성화)");
            } else {
                if (special_site_adapter_ && special_site_adapter_->isActive()) {
                    logger->info("통계 생성기 비활성 (Special Site 모드 활성화로 인한 자동 비활성화)");
                } else {
                    logger->info("통계 생성기 비활성 (config.json에서 false로 설정됨)");
                }
            }
        }

        // 5-2. 신호 계산기 초기화
        if (site_info_mgr_->isSignalDbEnabled()) {
            // 신호역산이 지원되고 타겟 신호가 유효한 경우
            if (site_info_.supports_signal_calc && site_info_.target_signal > 0) {
                signal_calc_ = std::make_unique<SignalCalculator>(site_info_mgr_->getDataProvider());
                
                // 신호 변경 콜백 설정
                auto signal_callback = [this](const SignalChangeEvent& event) {
                    this->handleSignalChangeCallback(event);
                };
                
                // 신호 계산기 시작
                if (signal_calc_->start(site_info_, signal_callback)) {
                    logger->info("신호 계산기 시작 성공 - 교차로: {}, 타겟신호: {}", 
                                site_info_.spot_ints_id, site_info_.target_signal);
                    logger->info("신호현시 통계 활성화");
                    
                    // Special Site 어댑터에 SignalCalculator 연결
                    if (special_site_adapter_) {
                        special_site_adapter_->setSignalCalculator(signal_calc_.get());
                        logger->info("Special Site 어댑터에 SignalCalculator 연결 완료");
                    }
                } else {
                    logger->error("신호 계산기 시작 실패");
                    signal_calc_.reset();
                }
            } else {
                logger->info("신호역산 미지원 또는 타겟 신호 없음 - 인터벌 통계만 생성 가능");
            }
        } else {
            if (!config.isVehicle2KEnabled()) {
                logger->info("신호 DB 비활성 (차량 2K 비활성으로 인한 강제 비활성화)");
            } else {
                logger->info("신호 DB 비활성 (config.json에서 false로 설정됨)");
            }
        }
        
        // ====== 6단계: 최종 상태 로그 ======
        logger->info("=== 활성 모듈 요약 ===");
        logger->info("  기반 인프라:");
        logger->info("    - Redis: 활성");
        logger->info("    - SQLite: 활성");
        logger->info("    - 사이트 정보: 활성 (CAM ID: {})", site_info_.spot_camr_id);
        
        logger->info("  Presence 모듈:");
        logger->info("    - 차량 Presence: {}", car_presence_ ? "활성" : "비활성");
        logger->info("    - 보행자 Presence: {}", ped_presence_ ? "활성" : "비활성");
        
        logger->info("  분석 모듈:");
        logger->info("    - 통계 생성기: {}", stats_gen_ ? "활성" : "비활성");
        logger->info("    - 대기행렬 분석: {}", queue_analyzer_ ? "활성" : "비활성");
        logger->info("    - 돌발상황 감지: {}", incident_detector_ ? "활성" : "비활성");
        logger->info("    - 신호 계산기: {}", signal_calc_ ? "활성" : "비활성");
        logger->info("    - 이미지 캡처: {}", image_capture_handler_ ? "활성" : "비활성");
        logger->info("    - Special Site: {}", 
                    (special_site_adapter_ && special_site_adapter_->isActive()) ? "활성" : "비활성");
        
        logger->info("시스템 매니저 초기화 완료");
        return true;
        
    } catch (const std::exception& e) {
        logger->error("시스템 매니저 초기화 중 오류: {}", e.what());
        return false;
    } catch (...) {
        logger->error("시스템 매니저 초기화 중 알 수 없는 예외 발생");
        return false;
    }
}

void SystemManager::start() {
    logger->info("시스템 매니저 시작");
    
    running_ = true;
    
    // 통계 생성기 시작 (내부 타이머 시작)
    if (stats_gen_) {
        stats_gen_->start();
        logger->info("통계 생성기 시작");
    }
    
    // 신호 계산기는 initialize에서 이미 시작됨
    
    // 대기행렬 분석기는 이벤트 기반이므로 별도 시작 불필요
    if (queue_analyzer_) {
        logger->info("대기행렬 분석기 준비 완료");
    }
    
    // 돌발상황 감지기는 이벤트 기반이므로 별도 시작 불필요
    if (incident_detector_) {
        logger->info("돌발상황 감지기 준비 완료");
    }
    
    // Presence 모듈들은 매 프레임 호출 대기
    if (car_presence_) {
        logger->info("차량 Presence 모듈 준비 완료");
    }
    
    if (ped_presence_) {
        logger->info("보행자 Presence 모듈 준비 완료");
    }
    
    logger->info("모든 모듈 시작 완료");
}

void SystemManager::stop() {
    logger->info("시스템 매니저 중지 시작");
    auto total_start = std::chrono::steady_clock::now();
    
    running_ = false;
    
    // 모듈 중지 (역순)
    
    // Presence 모듈 먼저 중지 (통계 로깅)
    if (car_presence_) {
        auto start = std::chrono::steady_clock::now();
        car_presence_->logStatistics();
        car_presence_.reset();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>
                      (std::chrono::steady_clock::now() - start);
        logger->info("차량 Presence 모듈 중지 완료: {}ms", elapsed.count());
    }
    
    if (ped_presence_) {
        auto start = std::chrono::steady_clock::now();
        ped_presence_->logStatistics();
        ped_presence_.reset();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>
                      (std::chrono::steady_clock::now() - start);
        logger->info("보행자 Presence 모듈 중지 완료: {}ms", elapsed.count());
    }
    
    if (stats_gen_) {
        auto start = std::chrono::steady_clock::now();
        stats_gen_->stop();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>
                      (std::chrono::steady_clock::now() - start);
        logger->info("통계 생성기 중지 완료: {}ms", elapsed.count());
    }
    
    if (signal_calc_) {
        auto start = std::chrono::steady_clock::now();
        signal_calc_->stop();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>
                      (std::chrono::steady_clock::now() - start);
        logger->info("신호 계산기 중지 완료: {}ms", elapsed.count());
    }
    
    // SQLite 연결 종료
    if (sqlite_handler_) {
        auto start = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>
                      (std::chrono::steady_clock::now() - start);
        logger->info("SQLite 연결 종료 완료: {}ms", elapsed.count());
    }
    
    // Redis 연결은 마지막에 종료
    if (redis_client_) {
        auto start = std::chrono::steady_clock::now();
        redis_client_->disconnect();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>
                      (std::chrono::steady_clock::now() - start);
        logger->info("Redis 연결 종료 완료: {}ms", elapsed.count());
    }
    
    // 전체 종료 시간
    auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>
                        (std::chrono::steady_clock::now() - total_start);
    logger->info("시스템 매니저 중지 완료 - 총 소요시간: {}ms", total_elapsed.count());
}

void SystemManager::updatePresenceModules(const std::map<int, ObjPoint>& vehicle_positions,
                                         const std::map<int, ObjPoint>& pedestrian_positions,
                                         int current_time) {
    // Presence 모듈은 신호와 무관하게 매 프레임 업데이트
    
    // 차량 Presence 업데이트
    if (car_presence_ && car_presence_->isEnabled()) {
        car_presence_->updateVehicles(vehicle_positions, current_time);
    }
    
    // 보행자 Presence 업데이트
    if (ped_presence_ && ped_presence_->isEnabled()) {
        ped_presence_->updatePedestrians(pedestrian_positions, current_time);
    }
}

void SystemManager::updatePerSecondData(const std::map<int, int>& lane_counts, int current_time) {
    if (!running_) return;
    
    // 1. 대기행렬 차로별 차량 수 업데이트 (적색 신호일 때만)
    if (queue_analyzer_ && signal_calc_) {
        if (!signal_calc_->isGreenSignal()) {
            queue_analyzer_->updateLaneCounts(lane_counts);
        }
    }
    
    // 2. 차로별 차량 수 저장 (신호 변경 시 사용)
    {
        std::lock_guard<std::mutex> lock(lane_counts_mutex_);
        last_lane_counts_ = lane_counts;
    }
    
    // 3. 통계 생성기에 프레임 데이터 업데이트
    if (stats_gen_) {
        stats_gen_->updateFrameData(lane_counts);
    }
    
    // 4. 돌발상황 감지기 정기 업데이트
    if (incident_detector_ && incident_detector_->isEnabled()) {
        incident_detector_->updatePerSecond(current_time);
    }
    
    // 5. Presence 모듈 주기적 통계 출력 (5분마다)
    static auto last_presence_log_time = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_presence_log_time).count();
    
    if (elapsed >= 300) {  // 5분 = 300초
        if (car_presence_) {
            car_presence_->logStatistics();
        }
        if (ped_presence_) {
            ped_presence_->logStatistics();
        }
        last_presence_log_time = now;
    }
}

void SystemManager::handleSignalChangeCallback(const SignalChangeEvent& event) {
    logger->info("신호 변경 콜백 수신: {} at {} (페이즈: {})", 
                event.type == SignalChangeEvent::Type::GREEN_ON ? "GREEN_ON" : "GREEN_OFF",
                event.timestamp, event.phase);
    
    // 1. 통계 생성기에 알림
    if (stats_gen_) {
        stats_gen_->onSignalChange(event);
        logger->debug("통계 생성기에 신호 변경 이벤트 전달");
    }
    
    // 2. 대기행렬 분석기에 알림
    if (queue_analyzer_) {
        bool is_green = (event.type == SignalChangeEvent::Type::GREEN_ON);
        
        if (is_green) {
            // 녹색 신호 시작 - 잔여 차량으로 대기행렬 분석
            std::map<int, int> residual_cars;
            {
                std::lock_guard<std::mutex> lock(lane_counts_mutex_);
                residual_cars = last_lane_counts_;
            }
            
            auto queue_data = queue_analyzer_->onGreenSignal(event.timestamp, residual_cars);
            
            if (queue_data.is_valid) {
                logger->info("대기행렬 분석 완료 - 접근로 잔여: {:.1f}, 최대: {:.1f}",
                           queue_data.approach.rmnn_queu_lngt,
                           queue_data.approach.max_queu_lngt);
            }
        } else {
            // 적색 신호 시작 - 대기행렬 추적 시작
            queue_analyzer_->onRedSignal(event.timestamp);
            
            // ImageCaptureHandler를 통해 캡처 요청 (대기행렬 전용)
            if (image_capture_handler_ && queue_analyzer_->isImageCaptureNeeded()) {
                image_capture_handler_->requestCapture(event.timestamp);
                logger->debug("대기행렬 이미지 캡처 예약 (적색신호 시작)");
            }
        }
    }

    // 3. 돌발상황 감지기에 알림
    if (incident_detector_ && incident_detector_->isEnabled()) {
        incident_detector_->onSignalChange(event);
        logger->debug("돌발상황 감지기에 신호 변경 이벤트 전달");
    }
    
    // 4. 상태 업데이트
    last_signal_state_ = (event.type == SignalChangeEvent::Type::GREEN_ON);
}

bool SystemManager::isGreenSignal() const {
    return signal_calc_ ? signal_calc_->isGreenSignal() : false;
}