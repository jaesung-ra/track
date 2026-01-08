#ifndef MAKE_QUERY_H
#define MAKE_QUERY_H
#include <string>
namespace QueryMaker {
    using namespace std;

    inline string approachQueueQuery(const string &spot_camr_id, int prev_on_time, int now, int remain_queue, int max_queue, const std::string& img_path_nm, const std::string& img_file_nm) {
        // 성능 평가용 //
        int tmp = remain_queue;
        remain_queue = max_queue;
        max_queue = tmp;
        // 성능 평가용 //

        return "insert into soitgaprdqueu(spot_camr_id, stats_bgng_unix_tm, stats_end_unix_tm, "
               "rmnn_queu_lngt, max_queu_lngt, img_path_nm, img_file_nm, crt_unix_tm) values('" +
               spot_camr_id + "', " + to_string(prev_on_time) + ", " + to_string(now) + ", " +
               to_string(remain_queue) + ", " + to_string(max_queue) + ", '" + img_path_nm + "', '" + img_file_nm + "', (SINCE_EPOCH(Second,now())))";
    }

    inline string laneQueueQuery(const string &spot_camr_id, int lane, int prev_on_time, int now, int residu_cars, int max, const std::string& img_path_nm, const std::string& img_file_nm) {
        // 성능 평가용 //
        int tmp = residu_cars;
        residu_cars = max;
        max = tmp;
        // 성능 평가용 //
        
        return "insert into soitglanequeu(spot_camr_id, lane_no, stats_bgng_unix_tm, stats_end_unix_tm, "
               "rmnn_queu_lngt, max_queu_lngt, img_path_nm, img_file_nm, crt_unix_tm) values('" +
               spot_camr_id + "', " + to_string(lane) + ", " + to_string(prev_on_time) + ", " + to_string(now) + ", " +
               to_string(residu_cars) + ", " + to_string(max) + ", '" + img_path_nm + "', '" + img_file_nm + "', (SINCE_EPOCH(Second,now())))";
    }

    inline string realtimeQuery(int id, int turn_time, int stop_pass_time, int first_detected_time,
                                const string &label, int lane, int dir_out, double turn_pass_speed, double stop_pass_speed,
                                double avg_speed, int sensing_time, const string &image_name) {
        return to_string(id) + "," + to_string(stop_pass_time) + "," + label + "," +
               to_string(lane) + "," + to_string(dir_out) + "," + to_string(turn_time) + "," +
               to_string(turn_pass_speed) + "," + to_string(stop_pass_time) + "," +
               to_string(stop_pass_speed) + "," + to_string(avg_speed) + "," +
               to_string(first_detected_time) + "," + to_string(sensing_time) + "," + image_name;
    }

    inline string sqlRealtimeQuery(int id, int turn_time, int stop_pass_time, int first_detected_time,
                                   const string &label, int lane, int dir_out, double turn_pass_speed, double stop_pass_speed,
                                   double avg_speed, int sensing_time, const string &image_name) {
        return "INSERT INTO test_table (id, turn_sensing_date, stop_sensing_date, first_detected_time, "
               "label, lane, dir_out, turn_point_speed, stop_point_speed, interval_speed, sensing_time, image_name) "
               "VALUES (" + to_string(id) + ", " + to_string(turn_time) + ", " + to_string(stop_pass_time) + ", " +
               to_string(first_detected_time) + ", '" + label + "', " + to_string(lane) + ", " + to_string(dir_out) + ", " +
               to_string(turn_pass_speed) + ", " + to_string(stop_pass_speed) + ", " + to_string(avg_speed) + ", " +
               to_string(sensing_time) + ", '" + image_name + "');";
    }

    inline string approachStatsQuery(const string &cam_id, int time_type, int stats_start, int stats_end, int totl_trvl,
                                     double avg_stop, double avg_interval, int avg_density, int min_density, int max_density, double share) {
        return "insert into soitgaprdstats(spot_camr_id, hr_type_cd, stats_bgng_unix_tm, stats_end_unix_tm, "
               "totl_trvl, avg_stln_dttn_sped, avg_sect_sped, avg_trfc_dnst, min_trfc_dnst, max_trfc_dnst, "
               "avg_lane_ocpn_rt, crt_unix_tm) values('" + cam_id + "', '" + to_string(time_type) + "', " +
               to_string(stats_start) + ", " + to_string(stats_end) + ", " + to_string(totl_trvl) + ", " +
               to_string(avg_stop) + ", " + to_string(avg_interval) + ", " + to_string(avg_density) + ", " +
               to_string(min_density) + ", " + to_string(max_density) + ", " + to_string(share) + ", (SINCE_EPOCH(Second,now())))";
    }

    inline string turntypeStatsQuery(const string &cam_id, int dir, int time_type, int stats_start, int stats_end,
                                     int kncr1, int kncr2, int kncr3, int kncr4, int kncr5, int kncr6, double avg_stop, double avg_interval) {
        return "insert into soitgturntypestats(spot_camr_id, turn_type_cd, hr_type_cd, stats_bgng_unix_tm, stats_end_unix_tm, "
               "kncr1_trvl, kncr2_trvl, kncr3_trvl, kncr4_trvl, kncr5_trvl, kncr6_trvl, avg_stln_dttn_sped, avg_sect_sped, crt_unix_tm) values('" +
               cam_id + "', '" + to_string(dir) + "', '" + to_string(time_type) + "', " + to_string(stats_start) + ", " +
               to_string(stats_end) + ", " + to_string(kncr1) + ", " + to_string(kncr2) + ", " + to_string(kncr3) + ", " +
               to_string(kncr4) + ", " + to_string(kncr5) + ", " + to_string(kncr6) + ", " + to_string(avg_stop) + ", " +
               to_string(avg_interval) + ", (SINCE_EPOCH(Second,now())))";
    }

    inline string kncrStatsQuery(const string &cam_id, int time_type, const string &label, int stats_start, int stats_end,
                                 int count, double avg_stop, double avg_interval) {
        return "insert into soitgkncrstats(spot_camr_id, hr_type_cd, kncr_cd, stats_bgng_unix_tm, stats_end_unix_tm, "
               "totl_trvl, avg_stln_dttn_sped, avg_sect_sped, crt_unix_tm) values('" + cam_id + "', '" +
               to_string(time_type) + "', '" + label + "', " + to_string(stats_start) + ", " + to_string(stats_end) + ", " +
               to_string(count) + ", " + to_string(avg_stop) + ", " + to_string(avg_interval) + ", (SINCE_EPOCH(Second,now())))";
    }

    inline string laneStatsQuery(const string &cam_id, int time_type, int lane, int stats_start, int stats_end, int count,
                                 double avg_stop, double avg_interval, int avg_density, int min_density, int max_density, double share) {
        return "insert into soitglanestats(spot_camr_id, hr_type_cd, lane_no, stats_bgng_unix_tm, stats_end_unix_tm, "
               "totl_trvl, avg_stln_dttn_sped, avg_sect_sped, avg_trfc_dnst, min_trfc_dnst, max_trfc_dnst, ocpn_rt, crt_unix_tm) values('" +
               cam_id + "', '" + to_string(time_type) + "', " + to_string(lane) + ", " + to_string(stats_start) + ", " +
               to_string(stats_end) + ", " + to_string(count) + ", " + to_string(avg_stop) + ", " + to_string(avg_interval) + ", " +
               to_string(avg_density) + ", " + to_string(min_density) + ", " + to_string(max_density) + ", " +
               to_string(share) + ", (SINCE_EPOCH(Second,now())))";
    }

//     inline string unexpectedIncidentQuery(const string &cam_id, int id, int current_time, int end_unix_time,
//                                           int event_type, const string &image_path, const string &image_name, int prcs_unix_time) {
//         return "insert into soitgunacevet(spot_camr_id, trce_id, ocrn_unix_tm, end_unix_tm, evet_type_cd,"
//                "img_path_nm, img_file_nm, prcs_unix_tm, crt_unix_tm) values('" + cam_id + "', " + to_string(id) + ", " +
//                to_string(current_time) + ", " + to_string(end_unix_time) + ", '" + to_string(event_type) + "', '" +
//                image_path + "', '" + image_name + "', " + to_string(prcs_unix_time) + ", (SINCE_EPOCH(Second,now())))";
//     }

    inline string unexpectedIncidentQuery(int id, int current_time, int event_type) {
       return "soitgunacevet_S,/opt/nvidia/deepstream/deepstream-6.0/sources/objectDetector_GB/images/" 
              + std::to_string(id) + "_" +std::to_string(current_time) + "_" + std::to_string(event_type) + ".jpg";
       }

    inline string unexpectedIncidentUpdateQuery(const string &cam_id, int current_time, int id, int event_type) {
        return "update soitgunacevet set end_unix_tm = " + to_string(current_time) + ", prcs_unix_tm = " +
               to_string(current_time) + " - ocrn_unix_tm WHERE spot_camr_id = '" + cam_id +
               "' and trce_id = " + to_string(id) + " and evet_type_cd = '" + to_string(event_type) + "'";
    }

    inline string pedestrianQuery(const string &cam_id, int id, int current_time, const string &dir) {
        return "insert into soitgcwdtinfo(spot_camr_id, trce_id, dttn_unix_tm, drct_se_cd, crt_unix_tm) values('" +
               cam_id + "', " + to_string(id) + ", " + to_string(current_time) + ", '" + dir + "', (SINCE_EPOCH(Second,now())))";
    }
}

#endif // MAKE_QUERY_H
