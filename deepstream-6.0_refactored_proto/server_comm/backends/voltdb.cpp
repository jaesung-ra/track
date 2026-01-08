#include "voltdb.h"
#include "../../json/jsoncpp.cpp"
#include <iostream>
#include "rest.h"

VoltDB::VoltDB(const std::string &config_path) {
    // ConfigParser server_config(configPath);
    // db_host = server_config.getString("db_host");
    // db_port = server_config.getInt("db_port");
    
    // ifez IP
    // db_host = "192.168.6.62";
    // db_port = 8080;

    // 사내 테스트용 IP //
    db_host = "192.168.1.3";
    db_port = 7777;

    // ITS 2차 대개체 IP
    // db_host = "192.168.11.5";
    // db_port = 8080;

    if(logger == NULL){
        logger = getLogger("DS_server_log");
    }
}

std::string VoltDB::executeQuery(const std::string &query) {
    // executeQueryTimeOut is in rest.cpp
    return executeQueryTimeOut(db_host, db_port, query, 3L);
}

std::string VoltDB::getCamrID(const std::string &ip) {
    Json::Reader reader;
    Json::Value res;
    std::string query = "SELECT spot_camr_id FROM soitgcamrinfo WHERE edge_sys_2k_ip = '" + ip + "'";
    reader.parse(executeQuery(query), res);
    std::string spot_camr_id;
    spot_camr_id = res["results"][0]["data"][0][0].asString();
    std::cout << BLU << "getCamrID result: " << RESET << spot_camr_id << std::endl;
    return spot_camr_id;    
}

int VoltDB::getPhaseInfo(std::vector<int> &result, const std::string &spot_ints_id, int &LC_CNT) {
    Json::Reader reader;
    Json::Value res;
    std::string query = "SELECT LC_CNT";

    for (char ring : {'A', 'B'}) { 
        for (int i=1; i<=8; ++i) {
            query += ", " + std::string(1, ring) + "_RING_" + std::to_string(i) + "_PHAS_HR";
        }
    }
    query += " FROM SOITDSPOTINTSSTTS WHERE SPOT_INTS_ID = " + spot_ints_id;
    std::string response = executeQuery(query);
    // std::string response = executeQueryTimeOut("192.168.6.150", 8080, query, 3L);

   // Parse JSON
    if (!reader.parse(response, res)) {
        logger->error("[VoltDB SOITDSPOTINTSSTTS] JSON parsing failed. Raw response: {}", response);
        return -1;
    }

    // Check for valid structure and status
    if (!res.isMember("status") || res["status"].asInt() != 1) {
        logger->error("[VoltDB SOITDSPOTINTSSTTS] Query failed or bad status: {}", res.toStyledString());
        return -1;
    }

    if (!res.isMember("results") || !res["results"].isArray() || res["results"].empty()) {
        logger->error("[VoltDB SOITDSPOTINTSSTTS] No results in response: {}", res.toStyledString());
        return -1;
    }

    const auto &data = res["results"][0]["data"];
    if (!data.isArray() || data.empty()) {
        logger->error("[VoltDB SOITDSPOTINTSSTTS] Data array is empty.");
        return -1;
    }

    const auto &row = data[0];
    if (!row.isArray() || row.size() < 17) {
        logger->error("[VoltDB SOITDSPOTINTSSTTS] Row is malformed or incomplete.");
        return -1;
    }

    // Parse LC_CNT
    if (!row[0].isInt()) {
        logger->error("[VoltDB SOITDSPOTINTSSTTS] LC_CNT is not an int.");
        return -1;
    }

    LC_CNT = row[0].asInt();

    for (int i=1; i<=16; ++i){
        result.push_back(row[i].asInt());
    }
    return 0;
}

int VoltDB::getMvmtInfo(std::vector<int> &result, const std::string &spot_ints_id) {
    Json::Reader reader;
    Json::Value res;
    std::string query = "SELECT ";

    bool first = true;
    for (char ring : {'A', 'B'}) { 
        for (int i=1; i<=8; ++i) {
            if (!first)
                query += ",";
            query += " " + std::string(1, ring) + "_RING_" + std::to_string(i) + "_PHAS_MVMT_NO";
            first = false;
        }
    }
    query += " FROM SOITDINTSPHASINFO "
            "WHERE SPOT_INTS_ID = " + spot_ints_id +
            " AND OPER_SE_CD = '0' "
            "ORDER BY CLCT_DT DESC "
            "LIMIT 1";

    std::string response = executeQuery(query);
    // std::string response = executeQueryTimeOut("192.168.6.150", 8080, query, 3L);

   // Parse JSON
    if (!reader.parse(response, res)) {
        logger->error("[VoltDB SOITDINTSPHASINFO] JSON parsing failed. Raw response: {}", response);
        return -1;
    }

    // Check for valid structure and status
    if (!res.isMember("status") || res["status"].asInt() != 1) {
        logger->error("[VoltDB SOITDINTSPHASINFO] Query failed or bad status: {}", res.toStyledString());
        return -1;
    }

    if (!res.isMember("results") || !res["results"].isArray() || res["results"].empty()) {
        logger->error("[VoltDB SOITDINTSPHASINFO] No results in response: {}", res.toStyledString());
        return -1;
    }

    const auto &data = res["results"][0]["data"];
    if (!data.isArray() || data.empty()) {
        logger->error("[VoltDB SOITDINTSPHASINFO] Data array is empty.");
        return -1;
    }

    const auto &row = data[0];
    if (!row.isArray() || row.size() < 16) {
        logger->error("[VoltDB SOITDINTSPHASINFO] Row is malformed or incomplete.");
        return -1;
    }

    for (int i=0; i<16; ++i){
        result.push_back(row[i].asInt());
    }

    return 0;
}
