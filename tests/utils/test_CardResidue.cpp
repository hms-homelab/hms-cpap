// SDD-002: unit tests for the full-card residue predicates. Kept in LOCKSTEP with
// the CpapDash cloud's test_ResidualSweep (isCpapEdf / residualSkip) so both the
// local OSCAR archive and the cloud backup classify card files identically.
#include <gtest/gtest.h>
#include "utils/CardResidue.h"

using hms_cpap::isCpapEdf;
using hms_cpap::residualSkip;

namespace {
constexpr uint64_t KB = 1024ull;
constexpr uint64_t MB = 1024ull * 1024ull;
}  // namespace

// ── isCpapEdf: only the analytical session EDFs ──────────────────────────────

TEST(IsCpapEdf, MatchesTheSessionTypes) {
    EXPECT_TRUE(isCpapEdf("20260617_120000_BRP.edf"));
    EXPECT_TRUE(isCpapEdf("20260617_120000_EVE.edf"));
    EXPECT_TRUE(isCpapEdf("20260617_120000_SAD.edf"));
    EXPECT_TRUE(isCpapEdf("20260617_120000_SA2.edf"));  // local includes SA2 oximetry
    EXPECT_TRUE(isCpapEdf("20260617_120000_PLD.edf"));
    EXPECT_TRUE(isCpapEdf("20260617_120000_CSL.edf"));
}

TEST(IsCpapEdf, IsCaseInsensitive) {
    EXPECT_TRUE(isCpapEdf("20260617_120000_brp.edf"));
    EXPECT_TRUE(isCpapEdf("20260617_120000_Csl.EDF"));
}

TEST(IsCpapEdf, RejectsResidueAndStr) {
    EXPECT_FALSE(isCpapEdf("20260617_120000_BRP.crc"));  // checksum, not analytical
    EXPECT_FALSE(isCpapEdf("STR.edf"));                   // root daily, not a DATALOG type
    EXPECT_FALSE(isCpapEdf("Identification.tgt"));
    EXPECT_FALSE(isCpapEdf("AGL.crc"));
}

// ── residualSkip: keep card metadata, drop junk + oversize ───────────────────

TEST(ResidualSkip, KeepsCardFiles) {
    EXPECT_FALSE(residualSkip("Identification.tgt", 1 * KB));
    EXPECT_FALSE(residualSkip("Identification.json", 1 * KB));
    EXPECT_FALSE(residualSkip("AGL.crc", 1 * KB));
    EXPECT_FALSE(residualSkip("DLL.log", 4 * KB));
    EXPECT_FALSE(residualSkip("Journal.dat", 8 * KB));
    EXPECT_FALSE(residualSkip("20260203_120000_BRP.crc", 1 * KB));
    // Brand-agnostic future metadata rides along (not on the denylist).
    EXPECT_FALSE(residualSkip("20260203_120000_BRP.md5", 1 * KB));
}

TEST(ResidualSkip, DropsMultimediaAndOfficeAndArchives) {
    EXPECT_TRUE(residualSkip("vacation.jpg", 2 * MB));
    EXPECT_TRUE(residualSkip("movie.mp4", 5 * MB));
    EXPECT_TRUE(residualSkip("taxes.xlsx", 100 * KB));
    EXPECT_TRUE(residualSkip("resume.docx", 100 * KB));
    EXPECT_TRUE(residualSkip("backup.zip", 1 * MB));
}

TEST(ResidualSkip, DropsOsJunkAndSidecars) {
    EXPECT_TRUE(residualSkip(".DS_Store", 6 * KB));
    EXPECT_TRUE(residualSkip("Thumbs.db", 6 * KB));
    EXPECT_TRUE(residualSkip("desktop.ini", 1 * KB));
    EXPECT_TRUE(residualSkip("ezshare.cfg", 1 * KB));
    EXPECT_TRUE(residualSkip("._AGL.tgt", 1 * KB));  // AppleDouble sidecar
}

TEST(ResidualSkip, DropsOversize) {
    EXPECT_TRUE(residualSkip("huge.dat", 21 * MB));
    // A non-junk extension is still dropped purely on the 20 MB cap.
    EXPECT_TRUE(residualSkip("Journal.dat", 25 * MB));
    // At/under the cap it is kept.
    EXPECT_FALSE(residualSkip("Journal.dat", 20 * MB));
}
