// Unit tests for FileUtils::isOximetryFile — recognizes ResMed oximetry signal
// files. SAD is the older-device suffix, SA2 the newer/ASV-device suffix; both
// carry SpO2 + pulse data and must be treated as oximetry.
#include <gtest/gtest.h>
#include "utils/FileUtils.h"

using hms_cpap::isOximetryFile;

TEST(IsOximetryFile, MatchesSadSuffix) {
    EXPECT_TRUE(isOximetryFile("20260208_233009_SAD.edf"));
}

TEST(IsOximetryFile, MatchesSa2Suffix) {
    // Newer/ASV ResMed devices write _SA2.edf instead of _SAD.edf.
    EXPECT_TRUE(isOximetryFile("20260208_233009_SA2.edf"));
}

TEST(IsOximetryFile, IsCaseInsensitive) {
    EXPECT_TRUE(isOximetryFile("20260208_233009_sad.edf"));
    EXPECT_TRUE(isOximetryFile("20260208_233009_Sa2.EDF"));
}

TEST(IsOximetryFile, RejectsOtherSignalTypes) {
    EXPECT_FALSE(isOximetryFile("20260208_233009_BRP.edf"));
    EXPECT_FALSE(isOximetryFile("20260208_233009_PLD.edf"));
    EXPECT_FALSE(isOximetryFile("20260208_233000_EVE.edf"));
    EXPECT_FALSE(isOximetryFile("20260208_233000_CSL.edf"));
    EXPECT_FALSE(isOximetryFile("STR.edf"));
}

TEST(IsOximetryFile, RejectsPartialOrWrongExtension) {
    // Must be the actual EDF signal file, not a lookalike token elsewhere.
    EXPECT_FALSE(isOximetryFile("sa2"));
    EXPECT_FALSE(isOximetryFile("sad_notes.txt"));
    EXPECT_FALSE(isOximetryFile("my_sa2_data.csv"));
    EXPECT_FALSE(isOximetryFile(""));
}
