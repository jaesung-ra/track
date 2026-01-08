/*
 * image_capture_handler.h
 * 
 * 이미지 프레임 캡처 관리 클래스
 * - 대기행렬 이미지 캡처 전용
 * - ImageCropper와 ImageStorage 조율
 */

#ifndef IMAGE_CAPTURE_HANDLER_H
#define IMAGE_CAPTURE_HANDLER_H

#include <atomic>
#include <memory>
#include <mutex>
#include "nvbufsurface.h"
#include "opencv2/opencv.hpp"

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

// Forward declarations
class ImageCropper;
class ImageStorage;
class QueueAnalyzer;

/**
 * @brief 대기행렬 이미지 캡처 관리 클래스
 * 
 * 대기행렬 이미지 캡처 로직을 관리
 * process_meta에서 호출되어 프레임별 캡처 처리
 * 
 * 주요 기능:
 * - 적색 신호 시 대기행렬 이미지 캡처
 * - 전체 프레임 스냅샷 저장
 * - QueueAnalyzer와 연동
 */
class ImageCaptureHandler {
private:
    // 의존성
    ImageCropper* image_cropper_;
    ImageStorage* image_storage_;
    QueueAnalyzer* queue_analyzer_;
    
    // 캡처 상태 관리
    std::atomic<bool> capture_pending_;    // 캡처 대기 중
    int capture_timestamp_;                // 캡처 요청 시간
    mutable std::mutex capture_mutex_;
    
    // 설정
    bool enabled_;                         // 대기행렬 캡처 활성화 여부
    std::string queue_image_path_;         // 대기행렬 이미지 경로
    
    // 로거
    std::shared_ptr<spdlog::logger> logger;
    
    /**
     * @brief 대기행렬 이미지 캡처 처리 (내부 메서드)
     * @param surface NvBufSurface 포인터
     * @param timestamp 현재 시간
     * @return 성공 시 true
     */
    bool captureQueueImage(NvBufSurface* surface, int timestamp);

public:
    ImageCaptureHandler();
    ~ImageCaptureHandler();
    
    /**
     * @brief 초기화
     * @param cropper ImageCropper 포인터
     * @param storage ImageStorage 포인터
     * @return 성공 시 true
     */
    bool initialize(ImageCropper* cropper, ImageStorage* storage);
    
    /**
     * @brief QueueAnalyzer 설정
     * @param queue_analyzer QueueAnalyzer 포인터
     */
    void setQueueAnalyzer(QueueAnalyzer* queue_analyzer);
    
    /**
     * @brief 프레임 처리 (process_meta에서 호출)
     * @param surface NvBufSurface 포인터
     * @param current_time 현재 시간
     * @return 캡처가 수행되었으면 true
     * 
     * QueueAnalyzer에서 요청한 대기행렬 캡처를 처리
     */
    bool processFrame(NvBufSurface* surface, int current_time);
    
    /**
     * @brief 대기행렬 이미지 캡처 요청
     * @param timestamp 타임스탬프
     * 
     * QueueAnalyzer가 적색 신호 감지 시 호출
     */
    void requestCapture(int timestamp);
    
    /**
     * @brief 캡처가 필요한지 확인
     * @return 캡처가 필요하면 true
     */
    bool needsCapture() const;
    
    /**
     * @brief 캡처 완료 표시
     */
    void markCaptured();
    
    /**
     * @brief 활성화 상태 확인
     * @return 대기행렬 캡처가 활성화되어 있으면 true
     */
    bool isEnabled() const { return enabled_; }
};

#endif // IMAGE_CAPTURE_HANDLER_H