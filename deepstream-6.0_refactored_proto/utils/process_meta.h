#ifndef PROCESS_META_H
#define PROCESS_META_H
#include <string>
#include <deque>
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>
#include <atomic>
#include <sqlite3.h>
#include "make_query.h"
/* TEMPORARY NAMING !!*/
#define RED "\x1B[31m"
#define GRN "\x1B[32m"
#define YEL "\x1B[33m"
#define BLU "\x1B[34m"
#define MAG "\x1B[35m"
#define CYN "\x1B[36m"
#define WHT "\x1B[37m"
#define RESET "\x1B[0m"

typedef struct _Point
{
    double x;
    double y;
} ObjPoint;

typedef struct _box
{
    double top = -1;
    double height = -1;
    double left = -1;
    double width = -1;
    int frame;
} box;

typedef struct _obj_data{
    int image_count = 0;
    // 최초검지, 정지선통과, 회전roi 진입 timestamp
    int first_detected_time;
    int turn_time;
    int stop_pass_time;
    // flag to distinguish whether stop line is passed and entered turn roi
    bool turn_pass = false;
    bool stop_line_pass = false;

    int lane = 0;
    int dir_out;
    
    std::string label;
    std::string image_name;
    // speed related variables
    int num_speed = 0;  // how many times speed is calculated
    ObjPoint prev_pos = {-1, -1};  // coord of previous box (1 second ago)
    int prev_pos_time;
    ObjPoint last_pos = {-1, -1}; // coord of previous box (location of box in the last appeared frame)
    std::deque<ObjPoint> prev_vehi;
    double speed = 0.0;       
    double avg_speed;   // average of all calculated speed from the start
    double stop_pass_speed;
    double turn_pass_speed;

    bool illegal_wait = false;
    int stop_sec = 0;
    bool tail_gate = false;
    bool accident = false;
    int illegal_wait_start_cycle;
    bool illegal_wait_start_phase;
    bool move_reverse = false;
    bool jaywalk = false;
    int jaywalk_start = 0;

    bool cross_out = false;
    bool ped_pass = false;
    int ped_dir = 0;
    std::deque<ObjPoint> prev_ped;
    box current_box;
} obj_data;

extern std::atomic<bool> waiting_image_save;
extern std::atomic<int> residual_timestamp;
extern std::atomic<bool> current_phase;
extern std::atomic<int> current_cycle;
extern std::mutex sqlite_lock;

// 사내 테스트용 전역 플래그
extern bool wait_queue_enabled;
extern bool move_reverse_enabled;
extern bool illegal_wait_enabled;
extern bool realtime_enabled_2k;
extern bool realtime_enabled_4k;
extern bool pedestrian_enabled_2k;
extern bool pedestrian_jaywalk_enabled_2k;

inline std::string timestamp();
int getCurTime();
int fetchStats(std::map<int ,std::vector<int>>& density, int time_type, int time_window);
std::vector<std::map<int, int>> getPerLaneDensity();
int resetPerLane();
std::map<int, int> getPerLaneCount();
#endif 