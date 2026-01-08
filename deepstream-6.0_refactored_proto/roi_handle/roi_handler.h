#ifndef ROIHANDLER_H
#define ROIHANDLER_H

#include <cmath>                       
#include <map>
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <limits>
#include <dirent.h>

#include "deepstream_app.h"
#include "nvll_osd_struct.h"
#include "config_parser.h"
#include "process_meta.h"
#include "calibration.h"   
#include "roi_utils.h"
#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

using roi = std::vector<ObjPoint>;

/**
 * @brief ROI 관련 기능을 담당하는 클래스 
 * ROI 파일 로드
 * 검지된 객체가 ROI 내부에 존재하는지 판단
 * ROI 색상 설정 및 송출 영상에 ROI Overlay
 */
class ROIHandler {
private:
    std::vector<ObjPoint> lane_points;

    // ROI 관련 매핑 테이블
    std::map<std::string, roi*> single_roi_map;
    std::map<std::string, std::map<int, roi>*> multi_roi_map;
    std::map<std::string, int> type_mapping;                    // 파싱 패턴 매핑
    std::map<std::string, NvOSD_ColorParams> color_mapping;     // ROI 색상 매핑

    // ROI Line 캐시
    static std::vector<NvOSD_LineParams> roi_lines;   

    // 로거 인스턴스
    std::shared_ptr<spdlog::logger> logger = NULL;
    
    const AppCtx &appCtx;
    
    /**
     * @brief ROI 좌표를 파일로부터 로드하는 함수
     * @return 성공 시 0, 실패 시 음수 값
     */
    int loadROI(std::string& source_name, const std::string& type);

    /**
     * @brief 화면에 그릴 ROI 라인을 캐싱하는 함수 
     */
    void cacheROILines();

    /**
     * @brief 로드된 ROI 들의 좌표를 로그에 저장하는 함수 
     */
    void logROICoords();

    void addROILine(size_t i, const NvOSD_ColorParams& color, const roi& roi_ref);

    std::string getFileName(const char* full_path);
public:
    // ROI 좌표 저장 변수
    static roi stop_line_roi;
    static roi reverse_stop_line_roi;
    static roi straight_roi;
    static roi reverse_straight_roi;
    static roi u_turn_roi;
    static roi intersection_roi;
    static roi intersection_roi2;
    static roi crosswalk_roi;
    static roi not_crosswalk_roi;
    static roi not_crosswalk_roi2;
    static roi pedestrian_roi;
    static roi reverse_area_roi;
    static std::map<int, roi> lane_roi;
    static std::map<int, roi> right_turn_roi;
    static std::map<int, roi> reverse_right_turn_roi;
    static std::map<int, roi> left_turn_roi;

    /**
     * @brief 생성자
     * @param appCtx_ref Deepstream App Context
     */
    ROIHandler(const AppCtx& appCtx_ref);   
    ~ROIHandler() = default;

    /**
     * @brief OSD 메타데이터로 ROI 라인을 영상 위에 그리는 함수
     * @param batch_meta 프레임의 메타데이터
     */
    int overlayROI(NvDsBatchMeta *batch_meta);

    /**
     * @brief 주어진 점이 어떤 차선 안에 있는지 반환하는 함수
     * @param p1 점의 좌표
     * @return 차선 내부 이면 차선 정보, 차선 외부 이면 0 반환
     */
    int getLaneNum(ObjPoint p1);

    /**
     * @brief 정지선 통과 여부 판단 함수
     * @param before 검출 객체의 이전 프레임 좌표
     * @param current 검출 객체의 현재 프레임 좌표
     * @return 이전-현재를 이은 선분이 정지선과 교차하면 차선 정보, 아니면 0 반환
     */
    int getLaneNum4k(ObjPoint before, ObjPoint current);
    
    /**
     * @brief 정지선 통과 여부 판단 함수
     * @param before 검출 객체의 이전 프레임 좌표
     * @param current 검출 객체의 현재 프레임 좌표
     * @return 이전-현재를 이은 선분이 정지선과 교차하면 true, 아니면 false 반환
     */
    bool stopLinePassCheck(ObjPoint before, ObjPoint current);

    /**
     * @brief 보행금지 영역인지 확인하는 함수
     * @param p1 점의 좌표
     * @return 보행금지 영역 내부이면 true, 외부이면 false 반환
     */
    bool isInNoPedZone(ObjPoint p1);

    /**
     * @brief 횡단보도 영역인지 확인하는 함수
     * @param p1 점의 좌표
     * @return 횡단보도 영역 내부이면 true, 외부이면 false 반환
     */
    bool isInCrossWalk(ObjPoint p1);

    /** 
     * @brief 유턴 영역인지 확인하는 함수
     * @param p1 점의 좌표
     * @return 유턴 영역 내부이면 true, 외부이면 false 반환
     */
    bool isInUTurnROI(ObjPoint p1);

    /**
     * @brief 교차로 내부 영역인지 확인하는 함수
     * @param p1 점의 좌표
     * @return 교차로 내부 영역 내부이면 true, 외부이면 false 반환
     */
    bool isInInterROI(ObjPoint p1);

    /**
     * @brief 방향별 ROI 영역인지 확인하는 함수
     * @param p1 점의 좌표
     * @return 방향별 ROI 영역 내부이면 회전유형코드, 외부이면 -1 반환
     */
    int isInTurnROI(ObjPoint p1);
};

#endif