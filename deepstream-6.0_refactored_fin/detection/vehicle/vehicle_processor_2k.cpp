/*
 * vehicle_processor_2k.cpp
 * 
 * 차량 감지 처리 클래스 구현 (2K 모드)
 * - obj_data 복사본 반환 방식으로 스레드 안전성 보장
 * - process_meta에서 전달받은 current_pos 활용
 * - Special Site 모드 지원 (신호 기반 방향 결정)
 */

#include "vehicle_processor_2k.h"
#include "../special/special_site_adapter.h"
#include "../../calibration/calibration.h"
#include "../../data/redis/channel_types.h"
#include "../../data/redis/redis_client.h"
#include "../../data/sqlite/sqlite_handler.h"
#include "../../image/image_cropper.h"
#include "../../image/image_storage.h"
#include "../../roi_module/roi_handler.h"
#include "../../server/manager/site_info_manager.h"
#include "../../utils/config_manager.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>

VehicleProcessor2K::VehicleProcessor2K(ROIHandler& roi, RedisClient& redis, SQLiteHandler& sqlite,
                                     ImageCropper& cropper, ImageStorage& storage, SiteInfoManager& site,
                                     SpecialSiteAdapter* special_adapter)
    : roi_handler(roi), redis_client(redis), sqlite_handler(sqlite),
      image_cropper(cropper), image_storage(storage), site_manager(site),
      special_site_adapter(special_adapter) {
    
    logger = getLogger("DS_VehicleProcessor2K_log");
    logger->info("VehicleProcessor2K 초기화");
    
    if (special_site_adapter && special_site_adapter->isActive()) {
        logger->info("Special Site 모드 활성화됨");
    }
}

obj_data VehicleProcessor2K::processVehicle(const obj_data& input_obj, const box& obj_box,
                                           const ObjPoint& current_pos, int current_time, 
                                           bool second_changed, NvBufSurface* surface) {
    // 입력 데이터 복사
    obj_data obj = input_obj;

    // 보행자 필터링 (안전장치)
    if (!isVehicleClass(obj.class_id)) {
        logger->warn("Non-vehicle object passed to VehicleProcessor: ID={}, class_id={}, label={}", 
                    obj.object_id, obj.class_id, obj.label);
        return obj;  // 수정 없이 반환
    }
    
    try {
        // 새 차량 체크 (data_processed 플래그로 판단)
        bool is_new = !obj.data_processed;
        if (is_new) {
            obj.data_processed = true;
            logger->debug("[NEW-VEHICLE] ID={} label={}", obj.object_id, obj.label);
        }
        
        // 속도 업데이트 (매 초마다)
        if (second_changed) {
            updateSpeed(obj, current_pos, current_time);
        }
        
        // ROI 전이 확인
        checkROITransition(obj, current_pos, current_time, obj_box, surface);
        
        // 주의: obj.last_pos는 process_meta에서 관리하므로 여기서 업데이트하지 않음
        
    } catch (const std::exception& e) {
        logger->error("2K 차량 처리 중 예외 발생: ID={}, 오류={}", 
                     obj.object_id, e.what());
    }
    
    // 수정된 obj_data 반환
    return obj;
}

void VehicleProcessor2K::updateSpeed(obj_data& obj, const ObjPoint& current_pos, 
                                    int current_time) {
    // prev_pos가 유효한 경우에만 속도 계산 (1초 전 위치)
    if (isValidPosition(obj.prev_pos) && isValidTimestamp(obj.prev_pos_time)) {
        // calculateSpeed 사용 (calibration.h의 함수)
        double speed = calculateSpeed(obj.prev_pos.x, obj.prev_pos.y, 
                                    current_pos.x, current_pos.y, 
                                    current_time - obj.prev_pos_time);
        
        // x축 이동거리가 20픽셀 이상이면 속도 보정
        if (std::fabs(current_pos.x - obj.prev_pos.x) > 20) {
            speed += 5.0;
        }
        
        // 첫 속도 계산 시 평균 초기화
        if (!isValidSpeed(obj.avg_speed)) {
            obj.avg_speed = speed;
        } else {
            // 누적 평균 계산
            obj.num_speed++;
            obj.avg_speed += (speed - obj.avg_speed) / obj.num_speed;
        }
        
        obj.speed = speed;
        obj.interval_speed = obj.avg_speed;     // 구간속도 = 평균속도
        
        logger->trace("2K 차량 ID {} 속도: 현재={:.2f}, 평균={:.2f}, 속도 계산 횟수={}", 
                     obj.object_id, speed, obj.avg_speed, obj.num_speed);
    } else {
        // 첫 속도 계산을 위한 초기화
        obj.num_speed = 0;
    }
    
    // 항상 위치와 시간 업데이트 (1초 전 위치)
    obj.prev_pos = current_pos;
    obj.prev_pos_time = current_time;
}

// current_pos는 현재 checkROITransition을 호출한 프레임(프레임 #i)에서 해당 객체 ID의 좌표
// obj.last_pos는 프레임 #i-1 에서 같은 객체 ID가 검출됬었던 좌표
// 위 사항은 로직 내에서 반드시 지켜져야 함
void VehicleProcessor2K::checkROITransition(obj_data& obj, const ObjPoint& current_pos, 
                                          int current_time, const box& obj_box, 
                                          NvBufSurface* surface) {
    // 이미 회전 ROI에 진입했으면 더 이상 처리하지 않음
    if (obj.turn_pass) {
        return;
    }
    
    // 차로 번호 가져오기
    int lane = roi_handler.getLaneNum(current_pos);
    
    // Special Site 모드: 방향별 ROI 미리 체크 (정지선 전)
    if (special_site_adapter && special_site_adapter->isActive() && !obj.stop_line_pass) {
        int turn_type = roi_handler.isInTurnROI(current_pos);
        
        if (turn_type > 0) {
            // straight_left 모드에서 우회전 감지 시 무시 표시
            auto config = special_site_adapter->getConfig();
            if (config.straight_left && (turn_type >= 31 && turn_type <= 32)) {
                obj.dir_out = -999;  // 우회전 무시 플래그
                logger->debug("[SPECIAL-PRE] 우회전 ROI 감지, 무시 예정: ID={}", obj.object_id);
                return;
            }
            // 직진/좌회전 또는 right 모드의 우회전 감지
            else {
                obj.dir_out = turn_type;  // 방향 미리 저장 (정지선에서 사용)
                logger->debug("[SPECIAL-PRE] 방향 ROI 감지: ID={}, 방향={}", 
                            obj.object_id, turn_type);
            }
        }
    }
    
    // 정지선 통과 체크
    if (!obj.stop_line_pass && isValidPosition(obj.last_pos)) {
        if (roi_handler.stopLinePassCheck(obj.last_pos, current_pos)) {
            obj.stop_line_pass = true;
            obj.stop_pass_time = current_time;
            obj.stop_pass_speed = isValidSpeed(obj.speed) ? obj.speed : 0.0;

            logger->debug("[STOPLINE-PASS] ID={} lane={} speed={:.2f}", 
                        obj.object_id, obj.lane, obj.stop_pass_speed);

            if (!obj.image_saved) {
                saveVehicleImage(obj, obj_box, surface, current_time);
                obj.image_saved = true;
            }
            
            // Special Site: 정지선 통과 시 최종 처리
            if (special_site_adapter && special_site_adapter->isActive()) {
                // 우회전 무시 플래그 체크
                if (obj.dir_out == -999) {
                    logger->info("[SPECIAL-STOPLINE] 우회전 차량 무시: ID={}", obj.object_id);
                    return;
                }
                
                // 차로 정보 처리
                auto config = special_site_adapter->getConfig();
                
                if (config.right) {
                    // right 모드는 차선 ROI가 없으므로 무조건 차로 1
                    obj.lane = 1;
                    logger->debug("[SPECIAL-RIGHT] 차로=1 설정 (차선 ROI 없음): ID={}", obj.object_id);
                } else if (config.straight_left) {
                    // straight_left 모드에서 차로 정보 확인
                    if (obj.lane <= 0) {
                        int current_lane = roi_handler.getLaneNum(current_pos);
                        if (current_lane > 0) {
                            obj.lane = current_lane;
                        } else {
                            // 차로 정보가 없으면 스킵
                            logger->info("[SPECIAL-STOPLINE] 차로 정보 없음, 스킵: ID={}", obj.object_id);
                            return;
                        }
                    }
                }
                
                // 방향 결정
                int final_direction = obj.dir_out;
                
                // 방향이 아직 결정되지 않은 경우 (방향별 ROI 미검출)
                if (final_direction <= 0) {
                    // 신호 기반 방향 결정 (straight_left 모드에서만)
                    auto config = special_site_adapter->getConfig();
                    if (config.straight_left) {
                        int turn = roi_handler.isInTurnROI(current_pos);
                        bool in_roi = (turn != -1);
                        final_direction = special_site_adapter->determineVehicleDirection(obj, in_roi, turn);
                        logger->info("[SPECIAL-SIGNAL] 신호 기반 방향 결정: ID={}, 방향={}", 
                                   obj.object_id, final_direction);
                    } else if (config.right) {
                        // right 모드에서 방향 ROI 미검출이면 스킵
                        logger->info("[SPECIAL-RIGHT] 우회전 ROI 미검출, 스킵: ID={}", obj.object_id);
                        return;
                    }
                }
                
                // 최종 데이터 설정 및 전송
                if (final_direction > 0) {
                    obj.dir_out = final_direction;
                    obj.turn_pass = true;
                    obj.turn_time = current_time;
                    obj.turn_pass_speed = isValidSpeed(obj.speed) ? obj.speed : 0.0;
                    
                    logger->info("[SPECIAL-FINAL] ID={} 정지선 통과 완료: 방향={}, 차로={}", 
                                obj.object_id, obj.dir_out, obj.lane);
                    
                    sendVehicleData(obj, current_time);
                    return;
                }
            }
        }
    }

    // 차선 ROI 업데이트
    if (lane != 0) {
        obj.lane = lane;
    }
    
    // 일반 모드: 기존 로직 (Special Site가 아닌 경우만)
    else if (obj.lane > 0 && (!special_site_adapter || !special_site_adapter->isActive())) {
        // ==== 일반 모드: 차선 ROI 밖 & 차선이 할당된 경우 ====
        
        // ROI에서 방향 판단
        int turn_type = roi_handler.isInTurnROI(current_pos);
        bool in_roi = (turn_type != -1);
        
        // 직진, 좌회전, 우회전 ROI 안에 존재
        if (in_roi && turn_type != -1) {
            // 직진의 경우 정지선 통과 확인
            if (turn_type == 11 && !obj.stop_line_pass) {
                return;
            }
            
            // 좌회전/우회전인데 정지선을 통과하지 않은 경우 추정값 설정
            if (!obj.stop_line_pass) {
                // 정지선통과시각 = 최초관측시각과 회전검지시각의 중간값
                if (obj.first_detected_time <= 0 || current_time <= 0) {
                    logger->error("[NEGATIVE-CHECK] ID={} first_time={} current_time={}", 
                                 obj.object_id, obj.first_detected_time, current_time);
                }
                obj.stop_pass_time = static_cast<int>(
                    (static_cast<int64_t>(obj.first_detected_time) + 
                    static_cast<int64_t>(current_time)) / 2
                );
                if (obj.stop_pass_time < 0) {
                    logger->error("[NEGATIVE-RESULT] ID={} stop_pass_time={}", 
                                 obj.object_id, obj.stop_pass_time);
                }
                // 정지선검지속도 = 구간속도(평균속도)
                obj.stop_pass_speed = isValidSpeed(obj.avg_speed) ? obj.avg_speed : 0.0;
                
                logger->debug("[STOPLINE-ESTIMATE] ID={} turn_type={} estimated_time={} estimated_speed={:.2f}", 
                            obj.object_id, turn_type, obj.stop_pass_time, obj.stop_pass_speed);
            }
            
            obj.dir_out = turn_type;
            obj.turn_pass = true;
            obj.turn_time = current_time;
            obj.turn_pass_speed = isValidSpeed(obj.speed) ? obj.speed : 0.0;

            logger->debug("[FINAL] ID={} dir={} lane={} label={} stop_pass={}", 
                obj.object_id, obj.dir_out, obj.lane, obj.label, obj.stop_line_pass);
            
            if (!obj.image_saved) {
                saveVehicleImage(obj, obj_box, surface, current_time);
                obj.image_saved = true;
            }

            // 데이터 전송 (data_sent_2k 체크 내부에서)
            sendVehicleData(obj, current_time);
        }
        // 직진, 좌회전, 우회전 ROI 안에 존재하지 않을 경우(일반 개소에서만)
        else {
            bool in_uturn = roi_handler.isInUTurnROI(current_pos);
            // U턴 ROI 체크
            if (in_uturn) {
                logger->debug("[U-TURN-DETECT] ID={} lane={} pos({:.0f},{:.0f})", 
                            obj.object_id, obj.lane, current_pos.x, current_pos.y);
                // U턴인데 정지선을 통과하지 않은 경우 추정값 설정
                if (!obj.stop_line_pass) {
                    if (obj.first_detected_time <= 0 || current_time <= 0) {
                        logger->error("[NEGATIVE-CHECK-UTURN] ID={} first_time={} current_time={}", 
                                     obj.object_id, obj.first_detected_time, current_time);
                    }
                    obj.stop_pass_time = static_cast<int>(
                        (static_cast<int64_t>(obj.first_detected_time) + 
                         static_cast<int64_t>(current_time)) / 2
                    );
                    
                    if (obj.stop_pass_time < 0) {
                        logger->error("[NEGATIVE-RESULT-UTURN] ID={} stop_pass_time={}", 
                                     obj.object_id, obj.stop_pass_time);
                    }
                    obj.stop_pass_speed = isValidSpeed(obj.avg_speed) ? obj.avg_speed : 0.0;
                    
                    logger->debug("[STOPLINE-ESTIMATE-UTURN] ID={} estimated_time={} estimated_speed={:.2f}", 
                                obj.object_id, obj.stop_pass_time, obj.stop_pass_speed);
                }
                
                obj.dir_out = 41;
                obj.turn_pass = true;
                obj.turn_time = current_time;
                obj.turn_pass_speed = isValidSpeed(obj.speed) ? obj.speed : 0.0;
                
                logger->debug("[FINAL] ID={} dir=41 lane={} label={}", 
                           obj.object_id, obj.lane, obj.label);
                
                if (!obj.image_saved) {
                    saveVehicleImage(obj, obj_box, surface, current_time);
                    obj.image_saved = true;
                }
                
                sendVehicleData(obj, current_time);
            }
        }
    }
}

void VehicleProcessor2K::sendVehicleData(const obj_data& obj, int current_time) {
    // data_sent_2k 플래그 체크 (중복 전송 방지)
    if (obj.data_sent_2k) {
        return;
    }
    
    try {
        // 메타데이터 생성 (cam_id 제외)
        std::string metadata = generateMetadata(obj);
        
        // Redis 전송
        int redis_result = redis_client.sendData(CHANNEL_VEHICLE_2K, metadata);
        
        if (redis_result == 0) {
            // Note: data_sent_2k 플래그는 process_meta에서 업데이트됨
            logger->info("2K 차량 데이터 Redis 전송 완료: ID={}, 방향={}, 차로={}, 차종={}", 
                        obj.object_id, obj.dir_out, obj.lane, obj.label);
        } else {
            logger->error("Redis 전송 실패: ID={}, 결과={}", obj.object_id, redis_result);
        }
        
        // Special Site 모드에서는 SQLite 저장 안함
        if (special_site_adapter && special_site_adapter->isActive()) {
            logger->debug("Special Site 모드 - SQLite 저장 스킵: ID={}", obj.object_id);
        } else {
            // SQLite 저장 - 3개 파라미터로 호출 (cam_id 없이, 차종 코드 변환)
            std::string vehicle_type_code = getVehicleTypeCode(obj.label);
            
            int sqlite_result = sqlite_handler.insertVehicleData(
                obj.object_id,      // vehicle_id
                obj,                // obj_data
                vehicle_type_code   // vehicle_type
            );
            
            if (sqlite_result != 0) {
                logger->error("SQLite 삽입 실패: ID={}, 차종={}, 에러코드={}", 
                             obj.object_id, vehicle_type_code, sqlite_result);
            }
        }
      
    } catch (const std::exception& e) {
        logger->error("2K 차량 데이터 전송 중 예외: ID={}, 오류={}", 
                     obj.object_id, e.what());
    }
}

std::string VehicleProcessor2K::generateMetadata(const obj_data& obj) {
    std::stringstream ss;
    
    // 차종 코드 변환
    std::string vehicle_type = getVehicleTypeCode(obj.label);

    // 이미지 저장 경로 가져오기
    auto& config = ConfigManager::getInstance();
    std::string car_image_path = config.getFullImagePath("vehicle_2k");
    
    // CSV 형식으로 메타데이터 생성 (cam_id 제외)
    // 형식: id,차종,차로,방향,회전검지시각,회전속도,정지선시각,정지선속도,구간속도,최초시각,관측시간,이미지경로,이미지파일명
    ss << obj.object_id << ","
       << vehicle_type << ","
       << obj.lane << ","
       << obj.dir_out << ","
       << obj.turn_time << ","
       << std::fixed << std::setprecision(3) << obj.turn_pass_speed << ","
       << obj.stop_pass_time << ","
       << obj.stop_pass_speed << ","
       << obj.interval_speed << ","
       << obj.first_detected_time << ","
       << (obj.turn_time - obj.first_detected_time) << ","
       << car_image_path << ","
       << obj.image_name;
    
    return ss.str();
}

void VehicleProcessor2K::saveVehicleImage(obj_data& obj, const box& obj_box, 
                                         NvBufSurface* surface, int current_time) {
    try {
        // 이미지 파일명 생성
        std::stringstream filename;
        filename << obj.object_id << "_" << current_time << ".jpg";
        obj.image_name = filename.str();
        
        // ImageCropper로 차량 이미지 크롭
        cv::Mat cropped = image_cropper.cropObject(surface, 0, obj_box);
        
        if (!cropped.empty()) {
            // ConfigManager에서 차량 이미지 경로 가져오기
            auto& config = ConfigManager::getInstance();
            std::string car_image_path = config.getFullImagePath("vehicle_2k");
            
            logger->debug("2K 차량 이미지 저장 시도: 경로={}, 파일={}", 
                        car_image_path, obj.image_name);
            
            std::string saved_path = image_storage.saveImage(cropped, car_image_path, obj.image_name);
            if (!saved_path.empty()) {
                logger->debug("2K 차량 이미지 저장 완료: ID={}, 파일={}, 경로={}", 
                            obj.object_id, obj.image_name, saved_path);
            } else {
                logger->error("2K 차량 이미지 저장 실패: ID={}, 파일={}, 경로={}", 
                            obj.object_id, obj.image_name, car_image_path);
            }
        } else {
            logger->error("2K 차량 이미지 크롭 실패: ID={}", obj.object_id);
        }
        
    } catch (const std::exception& e) {
        logger->error("2K 차량 이미지 저장 중 예외: ID={}, 오류={}", 
                     obj.object_id, e.what());
    }
}