#include "incident_detector.h"
#include "../../calibration/calibration.h"
#include "../../data/redis/channel_types.h"
#include "../../data/redis/redis_client.h"
#include "../../image/image_cropper.h"
#include "../../image/image_storage.h"
#include "../../roi_module/roi_handler.h"
#include "../../utils/config_manager.h"
#include <algorithm>
#include <cmath>
#include <sstream>

IncidentDetector::IncidentDetector() 
    : roi_handler_(nullptr)
    , redis_client_(nullptr)
    , image_cropper_(nullptr)
    , image_storage_(nullptr)
    , next_event_id_(1)
    , current_phase_(0)
    , current_cycle_(0)
    , has_signal_info_(false)
    , enabled_(false)
    , abnormal_stop_sequence_enabled_(false)
    , reverse_driving_enabled_(false)
    , pedestrian_jaywalk_enabled_(false) {
    
    logger = getLogger("DS_IncidentDetector_log");
    logger->info("돌발이벤트 감지기 생성");
}

IncidentDetector::~IncidentDetector() {
    if (logger) {
        // 모든 활성 이벤트 종료 처리
        int current_time = getCurTime();
        for (auto& [event_id, incident] : active_incidents_) {
            if (!incident.end_sent) {
                endIncident(event_id, current_time);
            }
        }
        logger->info("돌발상황 감지기 종료");
    }
}

bool IncidentDetector::initialize(ROIHandler* roi_handler, RedisClient* redis_client,
                                 ImageCropper* image_cropper, ImageStorage* image_storage) {
    
    logger->info("돌발상황 감지기 초기화 시작");
    
    try {
        // 필수 의존성 먼저 확인
        if (!roi_handler) {
            logger->error("ROI 핸들러가 NULL");
            return false;
        }
        
        if (!redis_client) {
            logger->error("Redis 클라이언트가 NULL");
            return false;
        }

        if (!image_cropper) {
            logger->error("ImageCropper가 NULL");
            return false;
        }
        
        if (!image_storage) {
            logger->error("ImageStorage가 NULL");
            return false;
        }   

        // 의존성 설정
        roi_handler_ = roi_handler;
        redis_client_ = redis_client;
        image_cropper_ = image_cropper;
        image_storage_ = image_storage;       

        logger->debug("의존성 설정 완료");
        
        // ConfigManager 인스턴스 가져오기
        auto& config_manager = ConfigManager::getInstance();
        logger->debug("ConfigManager 인스턴스 획득");
        
        // 이미지 저장 경로 설정
        incident_image_path_ = config_manager.getFullImagePath("incident_event");
        logger->info("돌발상황 이미지 저장 경로: {}", incident_image_path_);
        
        // 돌발상황 타입별 활성화 여부 확인
        abnormal_stop_sequence_enabled_ = config_manager.isAbnormalStopEnabled();
        reverse_driving_enabled_ = config_manager.isReverseDrivingEnabled();
        pedestrian_jaywalk_enabled_ = config_manager.isPedestrianJaywalkEnabled();

        logger->info("설정 값 읽기 - 연쇄이벤트: {}, 역주행: {}, 무단횡단: {}",
                abnormal_stop_sequence_enabled_,
                reverse_driving_enabled_,
                pedestrian_jaywalk_enabled_);
        
        // 하나라도 활성화되어 있으면 전체 활성화
        enabled_ = abnormal_stop_sequence_enabled_ || reverse_driving_enabled_ || pedestrian_jaywalk_enabled_;
        
        if (!enabled_) {
            logger->info("돌발상황 감지 비활성 (모든 돌발 타입 비활성) - 초기화는 성공");
            return true;
        }
        
        // Redis 연결 상태 한번 더 확인
        if (!redis_client_->isConnected()) {
            logger->error("Redis 연결 상태 불량");
            return false;
        }
        
        logger->info("돌발상황 감지기 초기화 완료");
        logger->info("  - 연쇄이벤트(정지-꼬리물기-사고): {}", abnormal_stop_sequence_enabled_ ? "활성" : "비활성");
        logger->info("  - 역주행: {}", reverse_driving_enabled_ ? "활성" : "비활성");
        logger->info("  - 무단횡단: {}", pedestrian_jaywalk_enabled_ ? "활성" : "비활성");
        
        return true;
        
    } catch (const std::exception& e) {
        logger->error("돌발상황 감지기 초기화 실패: {}", e.what());
        return false;
    } catch (...) {
        logger->error("돌발상황 감지기 초기화 중 알 수 없는 예외 발생");
        return false;
    }
}

void IncidentDetector::processVehicle(int id, const obj_data& obj, const box& bbox,
                                     NvBufSurface* surface, int current_time) {
    if (!enabled_) return;
    
    // 차량 상태 업데이트
    auto& state = vehicle_states_[id];
    ObjPoint current_pos = obj.last_pos;
    
    // 이전 위치가 있으면 속도 계산
    if (state.last_position.x >= 0) {
        double distance = calculateDistance(state.last_position, current_pos);
        double time_diff = current_time - state.last_update_time;
        if (time_diff > 0) {
            state.last_speed = distance / time_diff;
        }
    }
    
    // 상태 업데이트
    state.lane_id = obj.lane;
    state.direction = obj.dir_out;
    state.last_position = current_pos;
    state.last_update_time = current_time;
    state.in_intersection = roi_handler_->isInInterROI(current_pos);
    
    // 정지선 근처 여부 판단
    if (!roi_handler_->stop_line_roi.empty()) {
        double min_distance = 999999.0;
        for (const auto& point : roi_handler_->stop_line_roi) {
            double dist = calculateDistance(current_pos, point);
            if (dist < min_distance) {
                min_distance = dist;
            }
        }
        state.near_stop_line = (min_distance < IncidentThresholds::REVERSE_NEAR_STOPLINE_DISTANCE);
    }
    
    // 연쇄 이벤트 감지 (차량정지 -> 꼬리물기 -> 사고)
    if (abnormal_stop_sequence_enabled_ && state.in_intersection) {
        // 교차로 내부에서만 연쇄 이벤트 감지
        checkVehicleStop(id, state, bbox, surface, current_time);
        checkTailGating(id, state, bbox, surface, current_time);
        checkAccident(id, state, bbox, surface, current_time);
    }
    
    // 역주행 감지
    if (reverse_driving_enabled_) {
        checkReverseDriving(id, state, bbox, surface, current_time);
    }
}

void IncidentDetector::processPedestrian(int id, const obj_data& obj, const box& bbox,
                                        NvBufSurface* surface, int current_time) {
    if (!enabled_ || !pedestrian_jaywalk_enabled_) return;

    // obj_data에서 위치 추출 (last_pos 사용)
    ObjPoint position = obj.last_pos;

    // 보행자 상태 업데이트
    auto& state = pedestrian_states_[id];
    state.last_position = position;
    state.last_update_time = current_time;
    
    // 무단횡단 감지
    checkPedestrianJaywalk(id, state, position, bbox, surface, current_time);
}

void IncidentDetector::checkVehicleStop(int id, VehicleTrackingState& state, const box& bbox,
                                       NvBufSurface* surface, int current_time) {
    // 이미 정지 상태면 스킵
    if (state.is_stopped) return;
    
    // 속도가 5 미만인지 확인
    if (state.last_speed < IncidentThresholds::STOP_SPEED_THRESHOLD) {
        // 정지 시작 시간 기록
        if (state.stop_start_time == 0) {
            state.stop_start_time = current_time;
            logger->debug("차량 {} 정지 시작 - 속도: {:.2f}", id, state.last_speed);
        }
        
        // 정지 지속 시간 계산
        state.stop_duration = current_time - state.stop_start_time;
        
        // 4초 이상 지속되면 차량정지 이벤트 생성
        if (state.stop_duration >= IncidentThresholds::STOP_DURATION_THRESHOLD && !state.is_stopped) {
            // 즉시 이미지 저장
            saveIncidentImage(surface, id, bbox, current_time, IncidentType::ILLEGAL_WAIT);
            
            // 이벤트 생성
            int event_id = createIncident(IncidentType::ILLEGAL_WAIT, id, current_time);
            
            // 현재 신호 phase 저장
            active_incidents_[event_id].stop_start_phase = current_phase_;
            
            state.is_stopped = true;
            state.stop_event_id = event_id;
            
            logger->info("차량정지 감지 - ID: {}, 차로: {}, 정지시간: {}초", 
                        id, state.lane_id, state.stop_duration);
        }
    } else {
        // 움직이기 시작했으면 정지 상태 해제
        if (state.is_stopped && state.stop_event_id > 0) {
            // 차량정지 이벤트 종료
            endIncident(state.stop_event_id, current_time);
            
            // 연쇄 이벤트도 모두 종료
            if (state.is_tail_gating && state.tail_gate_event_id > 0) {
                endIncident(state.tail_gate_event_id, current_time);
            }
            if (state.is_accident && state.accident_event_id > 0) {
                endIncident(state.accident_event_id, current_time);
            }
            
            logger->debug("차량 {} 정지 해제 - 속도: {:.2f}", id, state.last_speed);
        }
        
        // 상태 리셋
        state.stop_start_time = 0;
        state.stop_duration = 0;
        state.is_stopped = false;
        state.is_tail_gating = false;
        state.is_accident = false;
        state.stop_event_id = 0;
        state.tail_gate_event_id = 0;
        state.accident_event_id = 0;
        
        // 역주행 관련 상태도 리셋
        state.reverse_start_time = 0;
        state.reverse_duration = 0;
        state.reverse_detected = false;
    }
}

void IncidentDetector::checkTailGating(int id, VehicleTrackingState& state, const box& bbox,
                                      NvBufSurface* surface, int current_time) {
    // 차량정지 상태가 아니면 스킵
    if (!state.is_stopped || state.is_tail_gating) return;
    
    // 신호 정보가 있는 경우
    if (has_signal_info_) {
        // 차량정지 시작시 phase와 현재 phase가 다르면 꼬리물기
        auto incident = active_incidents_.find(state.stop_event_id);
        if (incident != active_incidents_.end()) {
            if (incident->second.stop_start_phase != current_phase_) {
                // 즉시 이미지 저장
                saveIncidentImage(surface, id, bbox, current_time, IncidentType::TAILGATE);
                
                // 꼬리물기 이벤트 생성
                int event_id = createIncident(IncidentType::TAILGATE, id, current_time);
                
                // 현재 주기 저장
                active_incidents_[event_id].tail_gate_start_cycle = current_cycle_;
                
                state.is_tail_gating = true;
                state.tail_gate_event_id = event_id;
                
                logger->info("꼬리물기 감지 - 차량 ID: {}, 시작 phase: {}, 현재 phase: {}", 
                           id, incident->second.stop_start_phase, current_phase_);
            }
        }
    } else {
        // 신호 정보 없이 교차로에서 장시간 정지시 꼬리물기 의심
        if (state.stop_duration > 30) {  // 30초 이상
            if (!state.is_tail_gating) {
                // 즉시 이미지 저장
                saveIncidentImage(surface, id, bbox, current_time, IncidentType::TAILGATE);
                
                int event_id = createIncident(IncidentType::TAILGATE, id, current_time);
                state.is_tail_gating = true;
                state.tail_gate_event_id = event_id;
                logger->info("꼬리물기 감지(신호정보없음) - 차량 ID: {}, 정지시간: {}초", 
                           id, state.stop_duration);
            }
        }
    }
}

void IncidentDetector::checkAccident(int id, VehicleTrackingState& state, const box& bbox,
                                    NvBufSurface* surface, int current_time) {
    // 꼬리물기 상태가 아니면 스킵
    if (!state.is_tail_gating || state.is_accident) return;
    
    // 신호 정보가 있는 경우
    if (has_signal_info_) {
        // 꼬리물기 시작 주기의 다음 주기가 끝났는지 확인
        auto incident = active_incidents_.find(state.tail_gate_event_id);
        if (incident != active_incidents_.end()) {
            // 꼬리물기 시작 주기 + 1 < 현재 주기 이면 사고
            if (current_cycle_ > incident->second.tail_gate_start_cycle + 1) {
                // 즉시 이미지 저장
                saveIncidentImage(surface, id, bbox, current_time, IncidentType::ACCIDENT);
                
                // 사고 이벤트 생성
                int event_id = createIncident(IncidentType::ACCIDENT, id, current_time);
                
                state.is_accident = true;
                state.accident_event_id = event_id;
                
                logger->warn("사고 감지 - 차량 ID: {}, 꼬리물기 시작 주기: {}, 현재 주기: {}", 
                           id, incident->second.tail_gate_start_cycle, current_cycle_);
            }
        }
    } else {
        // 신호 정보 없이 매우 장시간 정지시 사고 의심 (5분 이상)
        if (state.stop_duration > IncidentThresholds::ACCIDENT_DURATION_WITHOUT_SIGNAL) {
            if (!state.is_accident) {
                // 즉시 이미지 저장
                saveIncidentImage(surface, id, bbox, current_time, IncidentType::ACCIDENT);
                
                int event_id = createIncident(IncidentType::ACCIDENT, id, current_time);
                state.is_accident = true;
                state.accident_event_id = event_id;
                logger->warn("사고 감지(신호정보없음) - 차량 ID: {}, 정지시간: {}초", 
                           id, state.stop_duration);
            }
        }
    }
}

void IncidentDetector::checkReverseDriving(int id, const VehicleTrackingState& state, const box& bbox,
                                          NvBufSurface* surface, int current_time) {
    // 역주행 이미 감지된 경우 스킵
    if (state.reverse_detected) return;
    
    // 정지선 근처가 아니면 역주행 카운터 리셋
    if (!state.near_stop_line) {
        auto& mutable_state = vehicle_states_[id];
        mutable_state.reverse_start_time = 0;
        mutable_state.reverse_duration = 0;
        mutable_state.initial_y = 0;
        return;
    }
    
    // 속도가 최소 속도 이상이어야 함 (확실한 이동만 감지, 박스 흔들림 제외)
    if (state.last_speed < IncidentThresholds::REVERSE_MIN_SPEED) {
        auto& mutable_state = vehicle_states_[id];
        mutable_state.reverse_start_time = 0;
        mutable_state.reverse_duration = 0;
        mutable_state.initial_y = 0;
        return;
    }
    
    // 역방향 이동 감지 (Y좌표가 지속적으로 감소)
    auto& mutable_state = vehicle_states_[id];
    
    // 역방향 이동 시작 감지
    if (mutable_state.reverse_start_time == 0) {
        // 이전 Y좌표보다 현재 Y좌표가 작으면 (카메라 기준 위로 이동)
        if (state.last_position.y < mutable_state.initial_y - IncidentThresholds::REVERSE_START_THRESHOLD) {
            mutable_state.reverse_start_time = current_time;
            mutable_state.initial_y = state.last_position.y;
            logger->debug("차량 {} 역방향 이동 시작 감지 - 정지선 근처", id);
        } else {
            mutable_state.initial_y = state.last_position.y;
        }
    } else {
        // 계속 역방향으로 이동하는지 확인
        if (state.last_position.y < mutable_state.initial_y) {
            // 역방향 이동 지속 시간 계산
            mutable_state.reverse_duration = current_time - mutable_state.reverse_start_time;
            
            // 최소 지속 시간 이상 역방향 이동하면 역주행으로 판단
            if (mutable_state.reverse_duration >= IncidentThresholds::REVERSE_MIN_DURATION) {
                // 이동 거리도 확인 (최소 거리 이상)
                double total_reverse_distance = mutable_state.initial_y - state.last_position.y;
                if (total_reverse_distance > IncidentThresholds::REVERSE_MIN_DISTANCE) {
                    // 즉시 이미지 저장
                    saveIncidentImage(surface, id, bbox, current_time, IncidentType::REVERSE);
                    
                    // 역주행 이벤트 생성
                    int event_id = createIncident(IncidentType::REVERSE, id, current_time);
                    endIncident(event_id, current_time + 1);  // 1초 후 종료
                    
                    mutable_state.reverse_detected = true;
                    
                    logger->warn("역주행 감지 - 차량 ID: {}, 차로: {}, 역방향 이동시간: {}초, 이동거리: {:.1f}픽셀", 
                               id, state.lane_id, mutable_state.reverse_duration, total_reverse_distance);
                }
            }
        } else {
            // 정방향으로 이동하면 카운터 리셋
            mutable_state.reverse_start_time = 0;
            mutable_state.reverse_duration = 0;
            mutable_state.initial_y = state.last_position.y;
        }
    }
}

void IncidentDetector::checkPedestrianJaywalk(int id, PedestrianTrackingState& state, 
                                             const ObjPoint& position, const box& bbox, 
                                             NvBufSurface* surface, int current_time) {
    // 보행자 금지구역 확인
    bool in_forbidden = roi_handler_->isInNoPedZone(position);
    
    if (in_forbidden) {
        // 이미 무단횡단 이벤트가 있으면 스킵
        if (state.jaywalk_event_id > 0) return;
        
        // 즉시 이미지 저장
        saveIncidentImage(surface, id, bbox, current_time, IncidentType::JAYWALK);
        
        // 무단횡단 이벤트 생성
        int event_id = createIncident(IncidentType::JAYWALK, id, current_time);
        state.jaywalk_event_id = event_id;
        
        logger->info("무단횡단 감지 - 보행자 ID: {}", id);
    } else {
        // 금지구역을 벗어났으면 이벤트 종료
        if (state.jaywalk_event_id > 0) {
            endIncident(state.jaywalk_event_id, current_time);
            state.jaywalk_event_id = 0;
            logger->debug("무단횡단 종료 - 보행자 ID: {}", id);
        }
    }
}

void IncidentDetector::saveIncidentImage(NvBufSurface* surface, int object_id, const box& bbox,
                                        int timestamp, IncidentType type) {
    try {
        // 전체 프레임 스냅샷
        cv::Mat frame_image = image_cropper_->getFullFrame(surface, 0);
        if (frame_image.empty()) {
            logger->error("프레임 스냅샷 실패 - 객체ID: {}", object_id);
            return;
        }
        
        // 전달받은 bbox 그리기
        drawBbox(frame_image, bbox);
        
        // 파일명 생성
        std::string filename = generateIncidentFilename(object_id, timestamp, type);
        
        // 이미지 저장
        std::string saved_path = image_storage_->saveImage(frame_image, incident_image_path_, filename);
        if (!saved_path.empty()) {
            logger->info("돌발상황 이미지 저장 성공: {}", saved_path);
        } else {
            logger->error("돌발상황 이미지 저장 실패: {}", filename);
        }
    } catch (const std::exception& e) {
        logger->error("돌발상황 이미지 저장 중 오류: {}", e.what());
    }
}

void IncidentDetector::drawBbox(cv::Mat& image, const box& bbox) {
    cv::Point tl(static_cast<int>(bbox.left),                  
                static_cast<int>(bbox.top));                   
    cv::Point br(static_cast<int>(bbox.left + bbox.width),        
                static_cast<int>(bbox.top + bbox.height));     
    
    // 보라색 bbox (BGR)
    const cv::Scalar ds_color_bgr(200, 50, 200);   
    const int thickness = 12;
    
    cv::rectangle(
        image,      
        tl, br,           
        ds_color_bgr,     
        thickness,       
        cv::LINE_AA       
    );
}

std::string IncidentDetector::generateIncidentFilename(int object_id, int timestamp, IncidentType type) {
    // 이미지 파일명 생성 (id_event type_timestamp.jpg 형식)
    std::stringstream ss;
    ss << object_id << "_" << static_cast<int>(type) << "_" << timestamp << ".jpg";
    return ss.str();
}

int IncidentDetector::createIncident(IncidentType type, int object_id, int start_time) {
    int event_id = next_event_id_++;
    
    ActiveIncident incident;
    incident.type = type;
    incident.object_id = object_id;
    incident.start_time = start_time;
    incident.end_time = 0;

    incident.image_file = generateIncidentFilename(object_id, start_time, type);
    
    incident.end_sent = false;
    incident.stop_start_phase = 0;
    incident.tail_gate_start_cycle = 0;
    
    active_incidents_[event_id] = incident;
    
    // 발생 메시지 즉시 전송
    sendIncidentStart(incident);
    
    return event_id;
}

void IncidentDetector::endIncident(int event_id, int end_time) {
    auto it = active_incidents_.find(event_id);
    if (it == active_incidents_.end()) return;
    
    if (it->second.end_sent) return;  // 이미 종료 메시지 전송됨
    
    it->second.end_time = end_time;
    it->second.end_sent = true;
    
    // 종료 메시지 전송
    sendIncidentEnd(it->second);
    
    // 이벤트 제거
    active_incidents_.erase(it);
}

void IncidentDetector::sendIncidentStart(const ActiveIncident& incident) {
    try {
        std::string json_str = createStartJson(incident);
        int result = redis_client_->sendData(CHANNEL_INCIDENT, json_str);
        if (result != 0) {
            logger->error("돌발이벤트 발생 전송 실패 - Redis 에러");
        } else {
            logger->info("돌발이벤트 발생 전송 - 타입: {}, ID: {}", 
                         static_cast<int>(incident.type), incident.object_id);
        }
    } catch (const std::exception& e) {
        logger->error("돌발이벤트 발생 전송 실패: {}", e.what());
    }
}

void IncidentDetector::sendIncidentEnd(const ActiveIncident& incident) {
    try {
        std::string json_str = createEndJson(incident);
        int result = redis_client_->sendData(CHANNEL_INCIDENT, json_str);
        if (result != 0) {
            logger->error("돌발이벤트 종료 전송 실패 - Redis 에러");
        } else {
            logger->info("돌발이벤트 종료 전송 - 타입: {}, ID: {}", 
                         static_cast<int>(incident.type), incident.object_id);
        }
    } catch (const std::exception& e) {
        logger->error("돌발이벤트 종료 전송 실패: {}", e.what());
    }
}

std::string IncidentDetector::createStartJson(const ActiveIncident& incident) {
    Json::Value root;
    Json::Value start;
    
    start[IncidentJsonKeys::TRACE_ID] = incident.object_id;
    start[IncidentJsonKeys::OCCUR_TIME] = incident.start_time;
    start[IncidentJsonKeys::EVENT_TYPE] = static_cast<int>(incident.type);
    start[IncidentJsonKeys::IMAGE_PATH] = incident_image_path_;
    start[IncidentJsonKeys::IMAGE_FILE] = incident.image_file;
    
    root[IncidentJsonKeys::START_KEY] = start;
    
    Json::FastWriter writer;
    return writer.write(root);
}

std::string IncidentDetector::createEndJson(const ActiveIncident& incident) {
    Json::Value root;
    Json::Value end;
    
    end[IncidentJsonKeys::TRACE_ID] = incident.object_id;
    end[IncidentJsonKeys::OCCUR_TIME] = incident.start_time;
    end[IncidentJsonKeys::END_TIME] = incident.end_time;
    end[IncidentJsonKeys::PROCESS_TIME] = incident.end_time - incident.start_time;  // 처리시간
    end[IncidentJsonKeys::EVENT_TYPE] = static_cast<int>(incident.type);
    end[IncidentJsonKeys::IMAGE_PATH] = incident_image_path_;
    end[IncidentJsonKeys::IMAGE_FILE] = incident.image_file;

    root[IncidentJsonKeys::END_KEY] = end;
    
    Json::FastWriter writer;
    return writer.write(root);
}

void IncidentDetector::onSignalChange(const SignalChangeEvent& event) {
    if (!enabled_) return;
    
    // 신호 정보 사용 가능 표시
    has_signal_info_ = true;
    
    // 신호 phase 업데이트 (GREEN_ON = 1, GREEN_OFF = 0)
    int prev_phase = current_phase_;
    current_phase_ = (event.type == SignalChangeEvent::Type::GREEN_ON) ? 1 : 0;
    
    // 신호가 녹색으로 바뀌면 주기 증가
    if (event.type == SignalChangeEvent::Type::GREEN_ON && prev_phase == 0) {
        current_cycle_++;
        logger->debug("신호 주기 증가: {}", current_cycle_);
    }
    
    logger->info("신호 변경 이벤트 수신 - 타입: {}, phase: {} -> {}, 주기: {}", 
                 event.type == SignalChangeEvent::Type::GREEN_ON ? "GREEN_ON" : "GREEN_OFF",
                 prev_phase, current_phase_, current_cycle_);
}

void IncidentDetector::updatePerSecond(int current_time) {
    if (!enabled_) return;
    
    static int cleanup_counter = 0;
    
    // 10초마다 오래된 상태 정리
    if (++cleanup_counter >= 10) {
        cleanupOldStates(current_time);
        cleanup_counter = 0;
    }
    
    // 타임아웃된 이벤트 확인
    checkIncidentTimeouts(current_time);
}

void IncidentDetector::cleanupOldStates(int current_time) {
    // 차량 상태 정리
    for (auto it = vehicle_states_.begin(); it != vehicle_states_.end();) {
        if (current_time - it->second.last_update_time > IncidentThresholds::EVENT_CLEANUP_TIMEOUT) {
            // 활성 이벤트가 있으면 종료
            if (it->second.stop_event_id > 0) {
                endIncident(it->second.stop_event_id, current_time);
            }
            if (it->second.tail_gate_event_id > 0) {
                endIncident(it->second.tail_gate_event_id, current_time);
            }
            if (it->second.accident_event_id > 0) {
                endIncident(it->second.accident_event_id, current_time);
            }
            
            logger->trace("오래된 차량 상태 제거 - ID: {}", it->first);
            it = vehicle_states_.erase(it);
        } else {
            ++it;
        }
    }
    
    // 보행자 상태 정리
    for (auto it = pedestrian_states_.begin(); it != pedestrian_states_.end();) {
        if (current_time - it->second.last_update_time > IncidentThresholds::EVENT_CLEANUP_TIMEOUT) {
            // 활성 이벤트가 있으면 종료
            if (it->second.jaywalk_event_id > 0) {
                endIncident(it->second.jaywalk_event_id, current_time);
            }
            
            logger->trace("오래된 보행자 상태 제거 - ID: {}", it->first);
            it = pedestrian_states_.erase(it);
        } else {
            ++it;
        }
    }
}

void IncidentDetector::checkIncidentTimeouts(int current_time) {
    // 타임아웃된 이벤트 자동 종료
    for (auto& [event_id, incident] : active_incidents_) {
        if (!incident.end_sent && 
            current_time - incident.start_time > IncidentThresholds::EVENT_END_TIMEOUT) {
            logger->debug("이벤트 타임아웃 - ID: {}, 타입: {}", 
                        event_id, static_cast<int>(incident.type));
            endIncident(event_id, current_time);
        }
    }
}

bool IncidentDetector::hasIncident(int object_id) const {
    if (!enabled_) return false;
    
    std::lock_guard<std::mutex> lock(incident_mutex_);
    
    // 차량 상태 확인
    auto vehicle_it = vehicle_states_.find(object_id);
    if (vehicle_it != vehicle_states_.end()) {
        const auto& state = vehicle_it->second;
        // 차량정지, 꼬리물기, 사고, 역주행 중 하나라도 있으면 true
        return state.is_stopped || state.is_tail_gating || 
               state.is_accident || state.reverse_detected;
    }
    
    // 보행자 상태 확인
    auto ped_it = pedestrian_states_.find(object_id);
    if (ped_it != pedestrian_states_.end()) {
        const auto& state = ped_it->second;
        // 무단횡단 이벤트가 있으면 true
        return state.jaywalk_event_id > 0;
    }
    
    return false;
}