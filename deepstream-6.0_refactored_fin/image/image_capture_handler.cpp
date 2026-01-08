/*
 * image_capture_handler.cpp
 * 
 * 대기행렬 이미지 캡처 관리 클래스 구현
 */

#include "image_capture_handler.h"
#include "image_cropper.h"
#include "image_storage.h"
#include "../analytics/queue/queue_analyzer.h"
#include "../utils/config_manager.h"
#include <sstream>

ImageCaptureHandler::ImageCaptureHandler() 
    : image_cropper_(nullptr)
    , image_storage_(nullptr)
    , queue_analyzer_(nullptr)
    , capture_pending_(false)
    , capture_timestamp_(0)
    , enabled_(false) {
    
    logger = getLogger("DS_ImageCaptureHandler_log");
    logger->info("ImageCaptureHandler 생성");
}

ImageCaptureHandler::~ImageCaptureHandler() {
    logger->info("ImageCaptureHandler 종료");
}

bool ImageCaptureHandler::initialize(ImageCropper* cropper, ImageStorage* storage) {
    if (!cropper || !storage) {
        logger->error("ImageCropper 또는 ImageStorage가 NULL");
        return false;
    }
    
    image_cropper_ = cropper;
    image_storage_ = storage;
    
    // ConfigManager에서 설정 확인
    auto& config_manager = ConfigManager::getInstance();
    enabled_ = config_manager.isWaitQueueEnabled();
    
    if (enabled_) {
        queue_image_path_ = config_manager.getFullImagePath("wait_queue");
        logger->info("대기행렬 이미지 캡처 활성화 - 경로: {}", queue_image_path_);
    } else {
        logger->info("대기행렬 이미지 캡처 비활성화");
    }
    
    logger->info("ImageCaptureHandler 초기화 완료");
    return true;
}

void ImageCaptureHandler::setQueueAnalyzer(QueueAnalyzer* queue_analyzer) {
    queue_analyzer_ = queue_analyzer;
    logger->debug("QueueAnalyzer 연결 완료");
}

bool ImageCaptureHandler::processFrame(NvBufSurface* surface, int current_time) {
    // 필수 컴포넌트 확인
    if (!surface || !image_cropper_ || !image_storage_) {
        return false;
    }
    
    // 대기행렬 캡처가 비활성화되었거나 QueueAnalyzer가 없으면 스킵
    if (!enabled_ || !queue_analyzer_) {
        return false;
    }
    
    // QueueAnalyzer에서 캡처가 필요한지 확인
    if (!queue_analyzer_->isImageCaptureNeeded()) {
        return false;
    }
    
    // 캡처 대기 중인지 확인
    if (!needsCapture()) {
        return false;
    }
    
    // 대기행렬 이미지 캡처 수행
    bool captured = captureQueueImage(surface, current_time);
    
    if (captured) {
        // 캡처 완료 처리
        markCaptured();
        
        // QueueAnalyzer에 캡처 완료 알림
        queue_analyzer_->setImageCaptured(current_time);
        
        logger->info("대기행렬 이미지 캡처 완료 - 시간: {}", current_time);
    }
    
    return captured;
}

bool ImageCaptureHandler::captureQueueImage(NvBufSurface* surface, int timestamp) {
    try {
        // 전체 프레임 스냅샷 (batch_idx 0 사용 - 단일 스트림)
        cv::Mat frame_image = image_cropper_->getFullFrame(surface, 0);
        if (frame_image.empty()) {
            logger->error("대기행렬 프레임 캡처 실패");
            return false;
        }
        
        // 파일명 생성 (타임스탬프.jpg)
        std::stringstream ss;
        ss << timestamp << ".jpg";
        std::string filename = ss.str();
        
        // 이미지 저장
        std::string saved_path = image_storage_->saveImage(frame_image, queue_image_path_, filename);
        
        if (!saved_path.empty()) {
            logger->info("대기행렬 이미지 저장 성공: {}", saved_path);
            return true;
        } else {
            logger->error("대기행렬 이미지 저장 실패: {}/{}", queue_image_path_, filename);
            return false;
        }
        
    } catch (const std::exception& e) {
        logger->error("대기행렬 이미지 처리 중 오류: {}", e.what());
        return false;
    } catch (...) {
        logger->error("대기행렬 이미지 처리 중 알 수 없는 오류");
        return false;
    }
}

void ImageCaptureHandler::requestCapture(int timestamp) {
    if (!enabled_) {
        logger->debug("대기행렬 캡처 비활성화 상태 - 요청 무시");
        return;
    }
    
    std::lock_guard<std::mutex> lock(capture_mutex_);
    
    capture_pending_.store(true);
    capture_timestamp_ = timestamp;
    
    logger->debug("대기행렬 이미지 캡처 요청 - 시간: {}", timestamp);
    
    // QueueAnalyzer가 연결되어 있으면 트리거 설정
    if (queue_analyzer_) {
        queue_analyzer_->triggerImageCapture(true);
    }
}

bool ImageCaptureHandler::needsCapture() const {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    return capture_pending_.load();
}

void ImageCaptureHandler::markCaptured() {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    
    if (capture_pending_.load()) {
        capture_pending_.store(false);
        logger->debug("대기행렬 캡처 완료 표시 - 시간: {}", capture_timestamp_);
        capture_timestamp_ = 0;
    }
}