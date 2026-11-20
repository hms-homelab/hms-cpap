#include <gtest/gtest.h>
#include "web/ArchiveChart.h"
using namespace hms_cpap;

namespace {
Json::Value point(const char* t,double flow) {
    Json::Value p;p["timestamp"]=t;p["flow_avg"]=flow;return p;
}
Json::Value base() {
    Json::Value b;b["timestamps"]=Json::Value(Json::arrayValue);b["timestamps"].append("2026-11-07 22:00:00");
    for(const auto& f:archive_chart::fields()){b[f]=Json::Value(Json::arrayValue);b[f].append(Json::nullValue);}
    b["flow_avg"][0]=99.0;return b;
}
}
TEST(ArchiveChart, RecordedMinuteAlwaysWinsAndUnknownChannelsStayNull) {
    auto b=base();Json::Value a;a["points"].append(point("2026-11-07 22:00:00",1));a["points"].append(point("2026-11-07 22:01:00",2));
    auto r=archive_chart::mergeSignals(b,a);
    ASSERT_EQ(r["timestamps"].size(),2);EXPECT_EQ(r["flow_avg"][0].asDouble(),99);EXPECT_EQ(r["flow_avg"][1].asDouble(),2);
    EXPECT_TRUE(r["respiratory_rate"][1].isNull());EXPECT_EQ(r["point_source"][1].asString(),"bluetooth_archive");
    EXPECT_EQ(b["timestamps"].size(),1);
}
TEST(ArchiveChart, GapsAreNullAndReplayDoesNotDuplicate) {
    auto b=base();Json::Value a;a["points"].append(point("2026-11-07 22:02:00",2));
    Json::Value gap;gap["timestamp"]="2026-11-07 22:01:00";a["points"].append(gap);
    auto r=archive_chart::mergeSignals(b,a);auto repeat=archive_chart::mergeSignals(r,a);
    EXPECT_EQ(r["timestamps"].size(),3);EXPECT_TRUE(r["flow_avg"][1].isNull());EXPECT_EQ(repeat,r);
}
TEST(ArchiveChart, SummaryOnlyRowUsesMachineValuesAndNeverFabricatesEvents) {
    Json::Value summary;summary["sleep_day"]="2026-11-07";summary["duration_minutes_str"]="480";summary["ahi_str"]="1.25";
    Json::Value a;a["points"].append(point("2026-11-07 22:00:00",1));a["points"].append(point("2026-11-07 22:01:00",2));
    auto r=archive_chart::summaryRow(summary,a);EXPECT_TRUE(r["archive_only"].asBool());
    EXPECT_DOUBLE_EQ(std::stod(r["duration_hours"].asString()),8);EXPECT_DOUBLE_EQ(std::stod(r["ahi"].asString()),1.25);
    EXPECT_TRUE(r["total_events"].isNull());EXPECT_TRUE(r["events"].empty());
    summary["ahi_str"]=Json::nullValue;EXPECT_TRUE(archive_chart::summaryRow(summary,a).isNull());
}
TEST(ArchiveChart, RemovedNightsAndDateValidationRemainExplicit) {
    EXPECT_TRUE(archive_chart::removed("2026-11-07",{"20261107"}));
    EXPECT_FALSE(archive_chart::dateValid("../../secret"));EXPECT_FALSE(archive_chart::dateValid("2026-99-07"));
}
