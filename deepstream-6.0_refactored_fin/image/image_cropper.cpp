/*
 * image_cropper.cpp
 * 
 * 범용적인 이미지 크롭 기능 구현
 * NvBufSurfTransform API 사용
 */

#include "image_cropper.h"
#include <glib.h>
#include <nvbufsurftransform.h>
#include <opencv2/opencv.hpp>

ImageCropper::ImageCropper() {
    logger = getLogger("DS_ImageCrop_log");
    logger->info("ImageCropper 초기화");
}

ImageCropper::~ImageCropper() {
    // Cleanup if needed
}

cv::Mat ImageCropper::extractFullFrame(NvBufSurface* surface, int batch_idx) {
    cv::Mat frame;
    
    if (!surface || batch_idx >= static_cast<int>(surface->numFilled)) {
        logger->error("Invalid surface or batch index");
        return frame;
    }
    
    try {
        // 새로운 서피스 생성 (전체 프레임용)
        NvBufSurface* new_surf = nullptr;
        NvBufSurfaceCreateParams create_params;
        
        create_params.gpuId = surface->gpuId;
        create_params.width = surface->surfaceList[batch_idx].width;
        create_params.height = surface->surfaceList[batch_idx].height;
        create_params.size = 0;
        create_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
        create_params.layout = NVBUF_LAYOUT_PITCH;
        
#ifdef __aarch64__
        create_params.memType = NVBUF_MEM_DEFAULT;
#else
        create_params.memType = NVBUF_MEM_CUDA_UNIFIED;
#endif
        
        if (NvBufSurfaceCreate(&new_surf, 1, &create_params) != 0) {
            logger->error("Failed to create new NvBufSurface");
            return frame;
        }
        
        // 서피스 복사
        new_surf->numFilled = surface->numFilled;
        if (NvBufSurfaceCopy(surface, new_surf) != 0) {
            logger->error("Failed to copy NvBufSurface");
            NvBufSurfaceDestroy(new_surf);
            return frame;
        }
        
        // CPU 접근을 위한 Map
        if (NvBufSurfaceMap(new_surf, 0, 0, NVBUF_MAP_READ) != 0) {
            logger->error("Failed to map surface");
            NvBufSurfaceDestroy(new_surf);
            return frame;
        }
        
        // CPU 동기화
        if (NvBufSurfaceSyncForCpu(new_surf, 0, 0) != 0) {
            logger->error("Failed to sync surface for CPU");
            NvBufSurfaceUnMap(new_surf, 0, 0);
            NvBufSurfaceDestroy(new_surf);
            return frame;
        }
        
        // OpenCV Mat으로 변환
        NvBufSurfaceParams* params = &new_surf->surfaceList[0];
        cv::Mat rgba_frame(params->height, params->width, CV_8UC4, 
                          params->mappedAddr.addr[0], params->pitch);
        
        // RGBA를 BGR로 변환
        cv::cvtColor(rgba_frame, frame, cv::COLOR_RGBA2BGR);
        
        // 정리
        NvBufSurfaceUnMap(new_surf, 0, 0);
        NvBufSurfaceDestroy(new_surf);
        
        logger->trace("Extracted full frame: {}x{}", params->width, params->height);
        
    } catch (const std::exception& e) {
        logger->error("Failed to extract frame: {}", e.what());
    }
    
    return frame;
}

cv::Mat ImageCropper::cropObject(NvBufSurface* surface, int batch_idx, 
                                const box& bbox, int padding) {
    cv::Mat cropped;
    
    if (!surface || batch_idx >= static_cast<int>(surface->numFilled)) {
        logger->error("Invalid surface or batch index");
        return cropped;
    }
    
    try {
        // 원본 서피스 파라미터
        NvBufSurfaceParams* src_params = &surface->surfaceList[batch_idx];
        
        // 패딩을 포함한 crop 영역 계산
        int src_left = std::max(0, static_cast<int>(bbox.left) - padding);
        int src_top = std::max(0, static_cast<int>(bbox.top) - padding);
        int src_width = std::min(static_cast<int>(src_params->width - src_left), 
                                static_cast<int>(bbox.width) + 2 * padding);
        int src_height = std::min(static_cast<int>(src_params->height - src_top), 
                                 static_cast<int>(bbox.height) + 2 * padding);
        
        // 유효성 검사
        if (src_width <= 0 || src_height <= 0) {
            logger->warn("Invalid crop dimensions: width={}, height={}", src_width, src_height);
            return cropped;
        }
        
        // 새로운 서피스 생성 (크롭된 이미지용)
        NvBufSurface* new_surf = nullptr;
        NvBufSurfaceCreateParams create_params;
        
        create_params.gpuId = surface->gpuId;
        create_params.width = src_width;
        create_params.height = src_height;
        create_params.size = 0;
        create_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
        create_params.layout = NVBUF_LAYOUT_PITCH;
        
#ifdef __aarch64__
        create_params.memType = NVBUF_MEM_DEFAULT;
#else
        create_params.memType = NVBUF_MEM_CUDA_UNIFIED;
#endif
        
        if (NvBufSurfaceCreate(&new_surf, 1, &create_params) != 0) {
            logger->error("Failed to create new NvBufSurface");
            return cropped;
        }
        
        // Transform 파라미터 설정
        NvBufSurfTransformParams transform_params;
        NvBufSurfTransformRect src_rect = {
            static_cast<guint>(src_top), 
            static_cast<guint>(src_left), 
            static_cast<guint>(src_width), 
            static_cast<guint>(src_height)
        };
        NvBufSurfTransformRect dst_rect = {0, 0, static_cast<guint>(src_width), static_cast<guint>(src_height)};
        
        transform_params.src_rect = &src_rect;
        transform_params.dst_rect = &dst_rect;
        transform_params.transform_flag = NVBUFSURF_TRANSFORM_FILTER | 
                                         NVBUFSURF_TRANSFORM_CROP_SRC | 
                                         NVBUFSURF_TRANSFORM_CROP_DST;
        transform_params.transform_filter = NvBufSurfTransformInter_Default;
        
        // 메모리 초기화
        NvBufSurfaceMemSet(new_surf, 0, 0, 0);
        
        // Transform 실행
        NvBufSurfTransform_Error err = NvBufSurfTransform(surface, new_surf, &transform_params);
        if (err != NvBufSurfTransformError_Success) {
            logger->error("Failed to transform nvbufsurface: {}", err);
            NvBufSurfaceDestroy(new_surf);
            return cropped;
        }
        
        // CPU 접근을 위한 Map
        if (NvBufSurfaceMap(new_surf, 0, 0, NVBUF_MAP_READ) != 0) {
            logger->error("Failed to map new surface");
            NvBufSurfaceDestroy(new_surf);
            return cropped;
        }
        
        // CPU 동기화
        if (NvBufSurfaceSyncForCpu(new_surf, 0, 0) != 0) {
            logger->error("Failed to sync new surface for CPU");
            NvBufSurfaceUnMap(new_surf, 0, 0);
            NvBufSurfaceDestroy(new_surf);
            return cropped;
        }
        
        // OpenCV Mat으로 변환
        NvBufSurfaceParams* new_params = &new_surf->surfaceList[0];
        cv::Mat rgba_img(new_params->height, new_params->width, CV_8UC4, 
                        new_params->mappedAddr.addr[0], new_params->pitch);
        cv::cvtColor(rgba_img, cropped, cv::COLOR_RGBA2BGR);
        
        // 정리
        NvBufSurfaceUnMap(new_surf, 0, 0);
        NvBufSurfaceDestroy(new_surf);
        
        logger->trace("Cropped object: {}x{} from ({},{}) with padding {}", 
                     src_width, src_height, src_left, src_top, padding);
                     
    } catch (const std::exception& e) {
        logger->error("Error during object crop: {}", e.what());
    }
    
    return cropped;
}

cv::Mat ImageCropper::cropRegion(NvBufSurface* surface, int batch_idx,
                               int x, int y, int width, int height,
                               int src_width, int src_height) {
    cv::Mat cropped;
    
    // box 구조체 생성
    box region_box;
    region_box.left = x;
    region_box.top = y;
    region_box.width = width;
    region_box.height = height;
    
    // cropObject 함수 재사용 (패딩 0으로 설정)
    return cropObject(surface, batch_idx, region_box, 0);
}