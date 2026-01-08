#include "roi_handler.h"       

roi ROIHandler::stop_line_roi;
roi ROIHandler::reverse_stop_line_roi;
roi ROIHandler::straight_roi;
roi ROIHandler::reverse_straight_roi;
roi ROIHandler::u_turn_roi;
roi ROIHandler::intersection_roi;
roi ROIHandler::intersection_roi2;
roi ROIHandler::crosswalk_roi;
roi ROIHandler::not_crosswalk_roi;
roi ROIHandler::not_crosswalk_roi2;
roi ROIHandler::pedestrian_roi;
roi ROIHandler::reverse_area_roi;
std::map<int, roi> ROIHandler::lane_roi;
std::map<int, roi> ROIHandler::right_turn_roi;
std::map<int, roi> ROIHandler::reverse_right_turn_roi;
std::map<int, roi> ROIHandler::left_turn_roi;
std::vector<NvOSD_LineParams> ROIHandler::roi_lines;

ROIHandler::ROIHandler(const AppCtx &appCtx_ref) : appCtx(appCtx_ref) {
    logger = getLogger("DS_ROI_log");
    
    single_roi_map = {
        {"calibration", &stop_line_roi}, {"r_calibration", &reverse_stop_line_roi},
        {"straight_lane_roi", &straight_roi}, {"r_straight_lane_roi", &reverse_straight_roi},
        {"u_turn_roi", &u_turn_roi}, {"intersection_roi", &intersection_roi},
        {"intersection_roi_2", &intersection_roi2}, {"crosswalk_roi", &crosswalk_roi},
        {"not_crosswalk_roi", &not_crosswalk_roi}, {"not_crosswalk_roi_2", &not_crosswalk_roi2},
        {"pedestrian", &pedestrian_roi}, {"reverse_area_roi", &reverse_area_roi}
    };

    multi_roi_map = {
        {"lane", &lane_roi}, {"right_turn_roi", &right_turn_roi},
        {"r_right_turn_roi", &reverse_right_turn_roi}, {"left_turn_roi", &left_turn_roi}
    };

    // ROI 타입에 따른 파싱 포맷 정의
    type_mapping = {
        {"right_turn_roi", 4}, {"r_right_turn_roi", 4}, {"left_turn_roi", 4},
        {"calibration", 3}, {"r_calibration", 3},
        {"pedestrian", 2}, {"u_turn_roi", 2}, {"straight_lane_roi", 2}, {"r_straight_lane_roi", 2},
        {"reverse_area_roi", 2}, {"intersection_roi", 2}, {"intersection_roi_2", 2},
        {"crosswalk_roi", 2}, {"not_crosswalk_roi", 2}, {"not_crosswalk_roi_2", 2},
        {"lane", 1}
    };

    // ROI 타입에 따른 색상 정의
    color_mapping = {
        {"right_turn_roi", {138.0/255, 43.0/255, 116.0/255, 1.0}}, {"r_right_turn_roi",{138.0/255, 43.0/255, 116.0/255, 1.0}},
        {"left_turn_roi", {0.5, 0.5, 0.0, 1.0}},
        {"calibration", {1.0, 0, 0, 1.0}}, {"r_calibration", {1.0, 0, 0, 1.0}},
        {"pedestrian",{0.0, 1.0, 0.0, 1.0}}, {"u_turn_roi", {65.0/255, 105.0/255, 225.0/255, 1.0}},
        {"straight_lane_roi", {1.0, 215.0/255, 0, 1.0}}, {"r_straight_lane_roi", {1.0, 215.0/255, 0, 1.0}},
        {"reverse_area_roi", {1.0, 215.0/255, 120, 1.0}}, {"intersection_roi", {5.0/255, 105.0/255, 125.0/255, 1.0}},
        {"intersection_roi_2", {5.0/255, 105.0/255, 125.0/255, 1.0}}, {"crosswalk_roi", {125.0/255, 15.0/255, 25.0/255, 1.0}},
        {"not_crosswalk_roi", {125.0/255, 15.0/255, 25.0/255, 1.0}}, {"not_crosswalk_roi_2", {125.0/255, 15.0/255, 25.0/255, 1.0}},
        {"lane", {230.0/255, 0, 0, 1.0}}
    };

    int num_sources = appCtx.config.tiled_display_config.columns * appCtx.config.tiled_display_config.rows;
    int res = 0;
    for (int i = 0; i < num_sources; i++) {
        if (appCtx.config.multi_source_config[i].uri != NULL) {
            std::string source_name = getFileName(appCtx.config.multi_source_config[i].uri);
            frameWidth[i] = appCtx.config.streammux_config.pipeline_width;
            frameHeight[i] = appCtx.config.streammux_config.pipeline_height;
            // 단일 ROI 타입 파일 로딩
            for (const auto& pair : single_roi_map) {
                res = loadROI(source_name, pair.first);
            }
            // 다중 ROI 타입 파일 로딩
            for (const auto& pair : multi_roi_map) {
                res = loadROI(source_name, pair.first);
            }
            if (res == -1) {
                logger->error("No rois Folder Exists");
                throw std::runtime_error("ROIs folder does not exist");
            }
        }
    }

    // ROI 좌표 로그 파일 저장 및 ROI 선 캐싱
    logROICoords();
    cacheROILines();
    if (roi_lines.size() == 0) {
        logger->info("No ROI Files Loaded");
    }
}


void ROIHandler::logROICoords(){
    // 단일 ROI 좌표 로그 저장
    for (const std::pair<const std::string, roi*>& pair : single_roi_map) {
        if (pair.second == nullptr) continue;

        const std::string& roi_type = pair.first;
        const roi& roi_ref = *(pair.second);
        if (roi_ref.size() == 0) {
            logger->info("[ROI] {}: Empty",roi_type);
            continue;
        }
        std::ostringstream ss;
        ss << "[ROI] " << roi_type << ": [";
        for (size_t i = 0; i < roi_ref.size(); ++i) {
            ss << "(" << roi_ref[i].x << ", " << roi_ref[i].y << ")";
            if (i != roi_ref.size() - 1) ss << ", ";
        }
        ss << "]";
        logger->info(ss.str());
    }

    // 다중 ROI 좌표 로그 저장
    for (const std::pair<const std::string, std::map<int, roi>*>& pair : multi_roi_map) {
        const std::string& roi_type = pair.first;
        std::map<int, roi>* roi_map_ptr = pair.second;

        if (roi_map_ptr == nullptr) continue;

        for (const std::pair<const int, roi>& lane_entry : *roi_map_ptr) {
            int id = lane_entry.first;
            const roi& roi_ref = lane_entry.second;

            std::ostringstream ss;
            ss << "[ROI] " << roi_type << "[" << id + 1 << "]: [";
            for (size_t i = 0; i < roi_ref.size(); ++i) {
                ss << "(" << roi_ref[i].x << ", " << roi_ref[i].y << ")";
                if (i != roi_ref.size() - 1) ss << ", ";
            }
            ss << "]";
            logger->info(ss.str());
        }
    }
}

int ROIHandler::loadROI(std::string& source_name, const std::string& type){
    // Input 영상 URI -> 파일명 변환
    std::string f_name;
    if (source_name.rfind("rtsp://", 0) == 0 || source_name.rfind("rtspt://", 0) == 0 || source_name.rfind("http://", 0) == 0){
        for (char &ch : source_name){
            if (ch == ':' || ch == '/')
                ch = '_';
        }
    }

    // ROI 파일 탐색
    std::string dir_path;
    if (realtime_enabled_4k) {
        if (type != "calibration")
            return 0;
        dir_path = "/opt/nvidia/deepstream/deepstream-6.0/sources/objectDetector_GB/calibration/";
        f_name = source_name;
    }
    else{
        dir_path = "/opt/nvidia/deepstream/deepstream-6.0/sources/objectDetector_GB/settings/rois/";
        f_name = type + '_' + source_name;
    }
    DIR *dir = opendir(dir_path.c_str());
    if (!dir) 
        return -1;

    struct dirent *entry;
    std::string matched_file;
    while ((entry = readdir(dir)) != NULL) {
        std::string file_name(entry->d_name);
        if (file_name.find(f_name) == 0) {  
            matched_file = dir_path + file_name;
            break;
        }
    }
    closedir(dir);

    if (matched_file.empty()) 
        return 0;
    std::ifstream file(matched_file);

    // 파싱 방식 결정
    auto type_it = type_mapping.find(type);

    int format_type = type_it->second;
    roi* target_roi = nullptr;
    std::map<int, roi>* target_roi_map = nullptr;
    if (format_type == 2 || format_type == 3)
        target_roi = single_roi_map[type];
    else
        target_roi_map = multi_roi_map[type];
    
    // ROI 좌표 파싱
    int num_points = 0;
    int num = 0;
    int num_lanes = 0;
    ObjPoint p;
    
    switch (format_type){
        case 1: {
            while (file >> num_points){
                roi& lane = (*target_roi_map)[num++];
                for (int i=0; i<num_points; i++){
                    file >> p.x; 
                    file.ignore(); 
                    file >> p.y;
                    lane.push_back(p);
                }
            }
            break;
        }

        case 2: {
            while (file >> p.x) {
                file.ignore();
                if (!(file >> p.y)) break;
                target_roi->push_back(p);
            }
            break;
        }

        case 3: {
            VDISTANCE[0] = 10;
            file >> DISTANCE[0];
            for (int i=0; i<4; i++){
                file >> POINT[0][i][0]; 
                file.ignore();
                file >> POINT[0][i][1];
            }
            for (int i=0; i<2; i++){
                file >> p.x;
                file.ignore();
                file >> p.y;
                target_roi->push_back(p);
            }
            std::string dummy;
            file >> dummy >> num_lanes;

            for (int i=0; i<=num_lanes; i++){
                file >> p.x;
                file.ignore();
                file >> p.y;
                lane_points.push_back(p);
            }
            computeCameraCalibration(0);
            break;
        }

        case 4: {
            file.ignore();
            for (int i=0; i<2; i++){
                roi& turn = (*target_roi_map)[i];
                file >> num_points;
                for (int j=0; j<num_points; j++){
                    file >> p.x;
                    file.ignore();
                    file >> p.y;
                    turn.push_back(p);
                }
            }
            break;
        }
    }
    file.close();
    logger->info("Successfully loaded file : {}", matched_file);
    return 0;
}

void ROIHandler::cacheROILines(){
    roi_lines.clear();

    // Calibration 라인 추가
    if (POINT[0][0][0] != -1) {
        NvOSD_LineParams line;
        line.x1 = POINT[0][0][0];
        line.y1 = POINT[0][0][1];
        line.x2 = POINT[0][1][0];
        line.y2 = POINT[0][1][1];
        line.line_width = 6;
        line.line_color = {50.0/255, 205.0/255, 50.0/255, 1.0};
        roi_lines.push_back(line);
    }

    // 단일 ROI 라인 추가
    for (const std::pair<const std::string, roi*>& pair : single_roi_map) {
        if (!pair.second || pair.second->size() < 2)
            continue;
        const std::string& roi_name = pair.first;
        const roi& roi_ref = *(pair.second);
        NvOSD_ColorParams color = color_mapping[roi_name];
        for (size_t i = 0; i < roi_ref.size(); i++){
            addROILine(i, color, roi_ref);
        }
    }

    // 다중 ROI 라인 추가
    for (const std::pair<const std::string, std::map<int, roi>*>& pair : multi_roi_map) {
        const std::string& roi_name = pair.first;
        std::map<int, roi>* roi_map_ptr = pair.second;
        if (!roi_map_ptr || roi_map_ptr->empty()) 
            continue;
        NvOSD_ColorParams color = color_mapping[roi_name];
        for (const std::pair<const int, roi>& roi_pair : *roi_map_ptr) {
            const roi& roi_ref = roi_pair.second;
            if (roi_ref.size() < 2) 
                continue;
            for (size_t i = 0; i < roi_ref.size(); i++) {
                addROILine(i, color, roi_ref);
            }
        }
    }
    return;
}

void ROIHandler::addROILine(size_t i, const NvOSD_ColorParams& color, const roi& roi_ref) {
    NvOSD_LineParams line;
    line.x1 = roi_ref[i].x;
    line.y1 = roi_ref[i].y;
    if (i == roi_ref.size() -1) {
        if (roi_ref.size() == 2)
            return;
        line.x2 = roi_ref[0].x;
        line.y2 = roi_ref[0].y;    
    } else {
        line.x2 = roi_ref[i + 1].x;
        line.y2 = roi_ref[i + 1].y;
    }
    line.line_width = 4;
    line.line_color = color;
    roi_lines.push_back(line);
    return;
}

std::string ROIHandler::getFileName(const char* full_path) {
    if (!full_path) 
        return "";  

    std::string path_str(full_path);  

    if (path_str.rfind("rtsp://", 0) == 0 || 
        path_str.rfind("http://", 0) == 0 || 
        path_str.rfind("rtspt://", 0) == 0) {
        return path_str;  
    }

    size_t pos = path_str.find_last_of("/");
    if (pos == std::string::npos) {
        return path_str;  
    }

    return path_str.substr(pos + 1); 
}

int ROIHandler::overlayROI(NvDsBatchMeta *batch_meta){
    size_t line_count = 0;
    size_t total_lines = roi_lines.size();

    // roi_lines의 선 들을 display_meta에 추가
    while (line_count < total_lines) {
        NvDsDisplayMeta *display_meta = nvds_acquire_display_meta_from_pool(batch_meta);
        if (!display_meta)
            return -1;
        display_meta->num_lines = 0;
        NvOSD_LineParams *line_params = display_meta->line_params;

        for (int i = 0; i < 16 && line_count < total_lines; i++, line_count++) {
            line_params[display_meta->num_lines] = roi_lines[line_count];  
            display_meta->num_lines++;
        }
        nvds_add_display_meta_to_frame(nvds_get_nth_frame_meta(batch_meta->frame_meta_list, 0), display_meta);
    }
    return 0;
}

int ROIHandler::getLaneNum(ObjPoint p1){
    int n = lane_roi.size();
    for (int i=0; i<n; i++){
        if (insidePolygon(p1, lane_roi[i]))
            return i+1;
    }
    return 0; 
}

int ROIHandler::getLaneNum4k(ObjPoint before, ObjPoint current) {
    if (before.x == -1)
        return 0;
    int last_lane_num = 0;

    ObjPoint p = getIntersectPoint(before, current, stop_line_roi[0], stop_line_roi[1]);

    for (size_t i = 0; i < lane_points.size() - 1; i++) {
        double maxy = max(lane_points[i].y, lane_points[i + 1].y);
        double miny = min(lane_points[i].y, lane_points[i + 1].y);

        if(maxy - miny < 0.05){
            double maxx = max(lane_points[i].x, lane_points[i + 1].x);
            double minx = min(lane_points[i].x, lane_points[i + 1].x);
            if(maxx >= p.x && p.x >= minx){
                last_lane_num = lane_points.size() - 1 - i;
                return last_lane_num;
            }
        }
        else{
            if (maxy >= p.y && p.y >= miny)
            {
                last_lane_num = lane_points.size() - 1 - i;
                return last_lane_num;
            }
        }
    }

    return last_lane_num;
}

bool ROIHandler::stopLinePassCheck(ObjPoint before, ObjPoint current){
    if (stop_line_roi.size() == 0)
        return false;
    if (before.x == -1)
        return false;
    return intersect(before, current, stop_line_roi[0], stop_line_roi[1]);
}

bool ROIHandler::isInUTurnROI(ObjPoint p1){
    return insidePolygon(p1, u_turn_roi);
}

bool ROIHandler::isInInterROI(ObjPoint p1){
    return insidePolygon(p1, intersection_roi) || insidePolygon(p1, intersection_roi2);
}

bool ROIHandler::isInCrossWalk(ObjPoint p1){
    return insidePolygon(p1, crosswalk_roi);
}

bool ROIHandler::isInNoPedZone(ObjPoint p1){
    return insidePolygon(p1, not_crosswalk_roi) || insidePolygon(p1, not_crosswalk_roi2);
}

int ROIHandler::isInTurnROI(ObjPoint p1){
    for (int i=0; i<2; i++){
        if (insidePolygon(p1, left_turn_roi[i]))
            return 21+i;
        if (insidePolygon(p1, right_turn_roi[i]))
            return 31+i;
    }
    if (insidePolygon(p1, straight_roi))
        return 11;
    return -1;
}