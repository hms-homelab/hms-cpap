#pragma once
#include <json/json.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hms_cpap::archive_chart {
inline const std::vector<std::string>& fields() {
    static const std::vector<std::string> f={"flow_avg","flow_max","flow_min","pressure_avg","pressure_max","pressure_min",
        "respiratory_rate","tidal_volume","minute_ventilation","ie_ratio","flow_limitation","leak_rate","mask_pressure",
        "epr_pressure","snore_index","target_ventilation","therapy_pressure"};
    return f;
}
inline bool dateValid(const std::string& s) {
    if(s.size()!=10 || s[4]!='-' || s[7]!='-')return false;
    for(size_t i=0;i<s.size();++i)if(i!=4&&i!=7&&(s[i]<'0'||s[i]>'9'))return false;
    return s.substr(0,4)>="2020"&&s.substr(0,4)<="2100"&&s.substr(5,2)>="01"&&s.substr(5,2)<="12"&&s.substr(8,2)>="01"&&s.substr(8,2)<="31";
}
inline std::string minuteKey(std::string s) {
    if(s.size()<16)return {};
    if(s[10]=='T')s[10]=' ';
    return s.substr(0,16);
}
inline bool removed(const std::string& date,const std::vector<std::string>& nights) {
    std::string key;for(char c:date)if(c!='-')key+=c;
    return std::find(nights.begin(),nights.end(),key)!=nights.end();
}
inline Json::Value load(const std::filesystem::path& root, const std::string& device) {
    try {
        namespace fs=std::filesystem;
        const auto path = root / "archive_charts.json";
        if(!fs::exists(root/"archive_charts.enabled")||fs::is_symlink(path)||!fs::exists(path)||fs::file_size(path)>8*1024*1024)return {};
        std::ifstream in(path);Json::Value value;Json::CharReaderBuilder builder;std::string errors;
        if(!Json::parseFromStream(builder,in,&value,&errors)||value["schema"].asInt()!=1||value["device_id"].asString()!=device||!value["nights"].isObject())return {};
        size_t count=0;
        for(const auto& date:value["nights"].getMemberNames()) {
            const auto& day=value["nights"][date];
            if(!dateValid(date)||!day["points"].isArray()||day["points"].size()>1600)return {};
            std::string previous;
            for(const auto& p:day["points"]) {
                if(!p["timestamp"].isString())return {};
                const auto t=p["timestamp"].asString();
                if(t.size()!=19||!dateValid(t.substr(0,10))||t[10]!=' '||t[13]!=':'||t.substr(16)!=":00")return {};
                if(!previous.empty()&&t<=previous)return {};
                previous=t;
                for(const auto& field:fields())if(!p[field].isNull()&&(!p[field].isNumeric()||!std::isfinite(p[field].asDouble())))return {};
                if(++count>100000)return {};
            }
        }
        return value;
    } catch(...) {return {};}
}
inline Json::Value day(const Json::Value& cache,const std::string& date) {
    return dateValid(date)&&cache.isObject()?cache["nights"].get(date,Json::Value()):Json::Value();
}
inline Json::Value mergeSignals(const Json::Value& base,const Json::Value& archive) {
    if(!archive["points"].isArray())return base;
    std::set<std::string> occupied;
    std::vector<Json::Value> rows;
    for(Json::ArrayIndex i=0;i<base["timestamps"].size();++i) {
        Json::Value row;row["timestamp"]=base["timestamps"][i];row["point_source"]="recorded";
        occupied.insert(minuteKey(row["timestamp"].asString()));
        for(const auto& field:fields())row[field]=i<base[field].size()?base[field][i]:Json::Value();
        rows.push_back(row);
    }
    size_t added=0;
    for(const auto& point:archive["points"]) {
        const auto key=minuteKey(point["timestamp"].asString());
        if(occupied.count(key))continue;
        Json::Value row=point;bool measured=false;
        for(const auto& field:fields())measured=measured||!row[field].isNull();
        row["point_source"]=measured?"bluetooth_archive":"gap";
        if(measured)++added;
        rows.push_back(row);occupied.insert(key);
    }
    if(!added)return base;
    std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return a["timestamp"].asString()<b["timestamp"].asString();});
    Json::Value result=base;result["timestamps"]=Json::Value(Json::arrayValue);result["point_source"]=Json::Value(Json::arrayValue);
    for(const auto& field:fields())result[field]=Json::Value(Json::arrayValue);
    for(const auto& row:rows) {
        result["timestamps"].append(row["timestamp"]);result["point_source"].append(row["point_source"]);
        for(const auto& field:fields())result[field].append(row[field]);
    }
    result["archive_note"]=archive.get("archive_note","Stored Bluetooth chart supplement; original/live data take priority.");
    return result;
}
inline bool number(const Json::Value& value,double& n) {
    try {
        if(value.isNull())return false;
        if(value.isNumeric())n=value.asDouble();
        else if(value.isString()){size_t end=0;const auto text=value.asString();n=std::stod(text,&end);if(end!=text.size())return false;}
        else return false;
        return std::isfinite(n);
    }catch(...){return false;}
}
inline Json::Value summaryRow(const Json::Value& summary,const Json::Value& archive) {
    double minutes=0,ahi=0;
    if(!number(summary["duration_minutes_str"],minutes)||!number(summary["ahi_str"],ahi)||minutes<0||minutes>1500||ahi<0||ahi>300||!archive["points"].isArray()||archive["points"].empty())return {};
    Json::Value row;row["sleep_day"]=summary["sleep_day"];row["session_start"]=archive["points"][0]["timestamp"];
    row["session_end"]=archive["points"][archive["points"].size()-1]["timestamp"];
    row["duration_seconds"]=std::to_string(minutes*60);row["duration_hours"]=std::to_string(minutes/60);row["ahi"]=std::to_string(ahi);
    row["index_kind"]="ahi";row["index_source"]="machine_summary";row["archive_only"]=true;row["night_state"]="complete";
    row["events"]=Json::Value(Json::arrayValue);row["therapy_mode"]="0";row["oximetry_only"]="0";
    for(const auto* field:{"total_events","obstructive_apneas","central_apneas","hypopneas","reras","avg_spo2","avg_heart_rate"})row[field]=Json::nullValue;
    return row;
}
}
