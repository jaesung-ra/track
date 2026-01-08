/*
 * vehicle_processor_4k.cpp
 * 
 * 차량 감지 처리 클래스 구현 (4K 모드)
 * - 오토바이: 정지선 통과시 1장만 저장
 * - 기타 차종: 정지선 전/중/후 다수 이미지 저장
 * - 속도 계산 및 저장
 * - 메타데이터 순서: obj_id,정지선통과시각,차로,차종,이미지경로(이미지 파일명 제외)
 */

#include "vehicle_processor_4k.h"
#include "../../calibration/calibration.h"
#include "../../data/redis/channel_types.h"
#include "../../data/redis/redis_client.h"
#include "../../image/image_cropper.h"
#include "../../image/image_storage.h"
#include "../../roi_module/roi_handler.h"
#include "../../utils/config_manager.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>

// ========== ImageSaver 클래스 구현 ==========
VehicleProcessor4K::ImageSaver::ImageSaver(ImageCropper& cropper, ImageStorage& storage)
    : cropper_(cropper), storage_(storage) {
    logger_ = getLogger("DS_VehicleProcessor4K_ImageSaver_log");
}

std::string VehicleProcessor4K::ImageSaver::saveVehicleImage(
    NvBufSurface* surface, const box& bbox,
    int object_id, int image_count, int timestamp,
    const std::string& save_path) {
    
    try {
        // ImageCropper로 차량 이미지 크롭
        cv::Mat cropped = cropper_.cropObject(surface, 0, bbox);
        
        if (cropped.empty()) {
            logger_->error("4K 차량 이미지 크롭 실패: ID={}", object_id);
            return "";
        }
        
        // 파일명 생성
        std::string filename = generateFilename(object_id, image_count, timestamp);
        
        logger_->debug("4K 차량 이미지 저장 시도: 경로={}, 파일={}", 
                      save_path, filename);
        
        // ImageStorage로 이미지 저장
        std::string saved_full_path = storage_.saveImage(cropped, save_path, filename);
        
        if (!saved_full_path.empty()) {
            logger_->info("4K 차량 이미지 저장 성공: {}", saved_full_path);
            return filename;  // 파일명만 반환
        } else {
            logger_->error("4K 차량 이미지 저장 실패: ID={}", object_id);
            return "";
        }
        
    } catch (const std::exception& e) {
        logger_->error("4K 차량 이미지 저장 중 예외: ID={}, 오류={}", 
                      object_id, e.what());
        return "";
    }
}

std::string VehicleProcessor4K::ImageSaver::generateFilename(
    int object_id, int image_count, int timestamp) {
    
    std::stringstream filename;
    filename << object_id << "_" 
            << image_count << "_"
            << timestamp << ".jpg";
    
    return filename.str();
}

// ========== VehicleProcessor4K 클래스 구현 ==========
VehicleProcessor4K::VehicleProcessor4K(ROIHandler& roi, RedisClient& redis,
                                     ImageCropper& cropper, ImageStorage& storage)
    : roi_handler(roi), redis_client(redis),
      image_cropper(cropper), image_storage(storage) {
    
    logger = getLogger("DS_VehicleProcessor4K_log");
    logger->info("VehicleProcessor4K 초기화");
    
    // ImageSaver 인스턴스 생성
    image_saver_ = std::make_unique<ImageSaver>(image_cropper, image_storage);
    
    // ConfigManager에서 FPS 정보 가져오기
    try {
        auto& config = ConfigManager::getInstance();
        camera_fps_ = config.getCameraFPS();
        logger->info("4K 카메라 FPS: {}", camera_fps_);
    } catch (...) {
        camera_fps_ = FRAMES_PER_SECOND_FOR_CAPTURE;
        logger->warn("FPS 정보 없음, 기본값 {} 사용", camera_fps_);
    }
}

obj_data VehicleProcessor4K::processVehicle(const obj_data& input_obj, const box& obj_box,
                                           const ObjPoint& current_pos, int current_time, 
                                           bool second_changed, NvBufSurface* surface) {
    // 입력 데이터 복사
    obj_data obj = input_obj;
    
    // 보행자 필터링 (안전장치)
    if (!isVehicleClass(obj.class_id)) {
        logger->warn("Non-vehicle object passed to VehicleProcessor4K: ID={}, class_id={}, label={}", 
                    obj.object_id, obj.class_id, obj.label);
        return obj; // 수정 없이 반환
    }
    
    try {
        // 새 차량 체크 (data_processed 플래그로 판단)
        bool is_new = !obj.data_processed;
        if (is_new) {
            obj.data_processed = true;
            logger->debug("4K 새 차량 감지: ID={}, label={}", obj.object_id, obj.label);
            
            // 캡처 상태 초기화
            capture_states_[obj.object_id] = ImageCaptureState();
            
            // 첫 프레임에서는 정지선 체크 불가
            return obj;
        }
        
        // 속도 업데이트 (매 초마다)
        if (second_changed) {
            updateSpeed(obj, current_pos, current_time);
        }
        
        // 이전 위치가 유효한지 확인 (첫 프레임 이후)
        if (!isValidPosition(obj.last_pos)) {
            logger->trace("4K 차량 ID {} 이전 위치 무효", obj.object_id);
            return obj;
        }
        
        // 차로 감지 (4K 전용 getLaneNum4k 사용)
        // obj.last_pos(이전)와 current_pos(현재) 사용
        int lane = roi_handler.getLaneNum4k(obj.last_pos, current_pos);
        if (lane != 0) {
            obj.lane = lane;
            logger->debug("4K 차량 ID {} 차로 감지: {}", obj.object_id, lane);
        }
        
        // 이미지 캡처 처리 (정지선 전)
        if (!obj.stop_line_pass) {
            processImageCapture(obj, current_pos, current_time, obj_box, surface);
        }
        
        // 정지선 통과 체크
        checkStopLine(obj, current_pos, current_time, obj_box, surface);

        // 주의: obj.last_pos는 process_meta에서 관리하므로 여기서 업데이트하지 않음
        
        // 정지선 통과 후 1초 이미지 캡처
        if (obj.stop_line_pass && capture_states_.count(obj.object_id)) {
            auto& state = capture_states_[obj.object_id];

            logger->debug("정지선 후 체크: ID={}, 오토바이={}, 이미 저장={}, 경과시간={}", 
                        obj.object_id, isMotorbike(obj.label), 
                        state.after_stop_image_saved,
                        current_time - state.stop_pass_time);           
            
            // 오토바이가 아니고, 정지선 통과 후 1초가 지났으며, 아직 미저장 시
            if (!isMotorbike(obj.label) && 
                !state.after_stop_image_saved &&
                (current_time - state.stop_pass_time) >= 1) {  // 1초 체크
                
                // ConfigManager에서 4K 차량 이미지 경로 가져오기
                auto& config = ConfigManager::getInstance();
                std::string car_image_path = config.getFullImagePath("vehicle_4k");
                
                state.image_count++;
                std::string saved_filename = image_saver_->saveVehicleImage(
                    surface, obj_box, obj.object_id, state.image_count, 
                    current_time, car_image_path);
                
                if (!saved_filename.empty()) {
                    state.saved_images.push_back(saved_filename);
                    state.after_stop_image_saved = true;
                    obj.image_name = saved_filename;
                    logger->info("4K 차량 ID {} 정지선 후 1초 이미지 저장 (#{}/{})", 
                               obj.object_id, state.image_count, state.saved_images.size());
                }
            }
        }
        
        // 주기적으로 오래된 상태 정리 (메모리 관리)
        if (second_changed) {
            cleanupOldStates(current_time);
        }
        
    } catch (const std::exception& e) {
        logger->error("4K 차량 처리 중 예외 발생: ID={}, 오류={}", 
                     obj.object_id, e.what());
    }
    
    return obj; // 수정된 obj_data 반환
}

void VehicleProcessor4K::updateSpeed(obj_data& obj, const ObjPoint& current_pos, 
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
        
        logger->trace("4K 차량 ID {} 속도: 현재={:.2f}, 평균={:.2f}, count={}", 
                     obj.object_id, speed, obj.avg_speed, obj.num_speed);
    } else {
        // 첫 속도 계산을 위한 초기화
        obj.num_speed = 0;
    }
    
    // 항상 위치와 시간 업데이트 (1초 전 위치)
    obj.prev_pos = current_pos;
    obj.prev_pos_time = current_time;
}

void VehicleProcessor4K::checkStopLine(obj_data& obj, const ObjPoint& current_pos, 
                                      int current_time, const box& obj_box, 
                                      NvBufSurface* surface) {
    
    // 이미 정지선 통과했으면 스킵
    if (obj.stop_line_pass) {
        return;
    }
    
    // 정지선 통과 확인
    // obj.last_pos(이전 프레임)와 current_pos(현재 프레임) 비교
    if (roi_handler.stopLinePassCheck(obj.last_pos, current_pos)) {
        obj.stop_line_pass = true;
        obj.stop_pass_time = current_time;
        obj.stop_pass_speed = isValidSpeed(obj.speed) ? obj.speed : 0.0;
        
        logger->info("4K 차량 ID {} 정지선 통과: 차종={}, 차로={}, 시간={}, 속도={:.2f}", 
                    obj.object_id, obj.label, obj.lane, current_time, obj.stop_pass_speed);
        
        // 캡처 상태 업데이트
        if (capture_states_.count(obj.object_id)) {
            auto& state = capture_states_[obj.object_id];
            state.stop_pass_time = current_time;
            
            // ConfigManager에서 4K 차량 이미지 경로 가져오기
            auto& config = ConfigManager::getInstance();
            std::string car_image_path = config.getFullImagePath("vehicle_4k");
            
            // 정지선 통과시 이미지 저장
            state.image_count++;
            std::string saved_filename = image_saver_->saveVehicleImage(
                surface, obj_box, obj.object_id, state.image_count, 
                current_time, car_image_path);
            
            if (!saved_filename.empty()) {
                state.saved_images.push_back(saved_filename);
                state.stop_line_image_saved = true;
                state.image_path = car_image_path;  // 경로 저장
                obj.image_name = saved_filename;
                logger->info("4K 차량 ID {} 정지선 통과 이미지 저장 (#{}/{})", 
                           obj.object_id, state.image_count, state.saved_images.size());
            }
            
            // 데이터 전송 (data_sent_4k 플래그 체크)
            if (!obj.data_sent_4k) {
                // 이미지 경로만 전송 (파일명 제외)
                sendVehicleData(obj, current_time, state.image_path);
                obj.data_sent_4k = true;
            }
        }
    }
}

void VehicleProcessor4K::processImageCapture(obj_data& obj, const ObjPoint& current_pos,
                                            int current_time, const box& obj_box, 
                                            NvBufSurface* surface) {
    
    logger->debug("processImageCapture 시작: ID={}, label={}, speed={}", 
                 obj.object_id, obj.label, obj.speed);    
    
    // 오토바이는 정지선 전 이미지 저장 안함
    if (isMotorbike(obj.label)) {
        logger->debug("오토바이 차종은 스킵: ID={}", obj.object_id);
        return;
    }
    
    // 속도 체크 (5km/h 이상)
    if (obj.speed < MIN_SPEED_FOR_IMAGE_CAPTURE) {
        logger->debug("속도 5km/h 미만으로 스킵: ID={}, speed={}", obj.object_id, obj.speed);
        return;
    }
    
    // Calibration ROI 내부인지 체크 (ROIHandler 사용)
    if (!roi_handler.isInCalibrationROI(current_pos)) {
        logger->debug("Calibration ROI 밖이라서 스킵: ID={}, pos=({},{})", 
                     obj.object_id, current_pos.x, current_pos.y);        
        return;
    }

    logger->debug("모든 조건 통과, 이미지 저장 진행: ID={}", obj.object_id);
    
    // 캡처 상태 가져오기
    if (!capture_states_.count(obj.object_id)) {
        capture_states_[obj.object_id] = ImageCaptureState();
    }
    auto& state = capture_states_[obj.object_id];
    
    // 최대 이미지 수 체크
    if (state.image_count >= MAX_IMAGES_BEFORE_STOPLINE) {
        return;
    }
    
    // 초당 1장 제한 (시간 체크)
    if (state.last_capture_time > 0) {
        int seconds_passed = (current_time - state.last_capture_time);
        if (seconds_passed < 1) {  // 1초 체크
            return;  // 아직 1초 안됨
        }
    }
    
    // ConfigManager에서 4K 차량 이미지 경로 가져오기
    auto& config = ConfigManager::getInstance();
    std::string car_image_path = config.getFullImagePath("vehicle_4k");
    
    // 이미지 캡처
    state.image_count++;
    std::string saved_filename = image_saver_->saveVehicleImage(
        surface, obj_box, obj.object_id, state.image_count, 
        current_time, car_image_path);
    
    if (!saved_filename.empty()) {
        state.saved_images.push_back(saved_filename);
        state.last_capture_time = current_time;
        state.image_path = car_image_path;  // 첫 번째 이미지일 때 경로 저장
        obj.image_name = saved_filename;
        logger->debug("4K 차량 ID {} 정지선 전 이미지 저장 (#{}/{}, 속도={:.1f}km/h)", 
                     obj.object_id, state.image_count, state.saved_images.size(), obj.speed);
    } else {
        state.image_count--;  // 실패시 카운트 복원
    }
}

void VehicleProcessor4K::sendVehicleData(const obj_data& obj, int current_time, 
                                        const std::string& image_path) {
    try {
        // 메타데이터 생성
        std::string metadata = generateMetadata(obj, image_path);
        
        // Redis 전송 (CHANNEL_VEHICLE_4K 사용)
        int redis_result = redis_client.sendData(CHANNEL_VEHICLE_4K, metadata);
        
        if (redis_result == 0) {
            logger->info("4K 차량 데이터 Redis 전송 완료: ID={}, 차종={}, 차로={}", 
                        obj.object_id, obj.label, obj.lane);
        } else {
            logger->error("Redis 전송 실패: ID={}, 결과={}", obj.object_id, redis_result);
        }
        
        // SQLite 저장 없음 (4K는 Redis만 사용)
        
    } catch (const std::exception& e) {
        logger->error("4K 차량 데이터 전송 중 예외: ID={}, 오류={}", obj.object_id, e.what());
    }
}

std::string VehicleProcessor4K::generateMetadata(const obj_data& obj, const std::string& image_path) {
    std::stringstream ss;
    
    // 차량 4K 메타데이터 형식: obj_id,정지선통과시각,차로,차종,이미지경로
    // 이미지경로에서 이미지파일명은 제외
    ss << obj.object_id << ","
       << obj.stop_pass_time << ","
       << obj.lane << ","
       << obj.label << ","
       << image_path;
    
    return ss.str();
}

void VehicleProcessor4K::cleanupOldStates(int current_time) {
    // 30초 이상 업데이트 없는 상태 정리
    const int CLEANUP_TIMEOUT = 30;
    
    auto it = capture_states_.begin();
    while (it != capture_states_.end()) {
        if ((current_time - it->second.stop_pass_time) > CLEANUP_TIMEOUT &&
            it->second.stop_pass_time > 0) {
            logger->debug("4K 캡처 상태 정리: ID={}", it->first);
            it = capture_states_.erase(it);
        } else {
            ++it;
        }
    }
}