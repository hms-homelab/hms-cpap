#pragma once
//
// SDD-073 D1: `hms_cpap --read-card <device> --card-root <folder> --out <file>
// [--progress <file>]`.
//
// The installer runs this once, elevated (the only step that needs it: the
// layout sectors are a raw read of the card), and gets back the same
// .cpapcard file CpapDash Desktop's Export for HMS CPAP writes (SDD-061): the
// card's files at their card paths, stored, and `baseline.cbl1` at the root,
// deflated. The device is opened read-only; nothing is ever written to it.
//
//   <device>     macOS /dev/diskNsM (read as /dev/rdiskN), Linux /dev/sdXN,
//                Windows a drive letter "E:" (read as its PhysicalDrive).
//   --card-root  where the card is mounted: its files are copied from there,
//                and a folder that is not a CPAP card is refused.
//   --out        the .cpapcard to write (written whole, then renamed).
//   --progress   a small JSON file rewritten as the read goes,
//                {"phase":"layout"|"files","done":N,"total":M}, which the
//                installer watches for changes (a file event, no clock). Its
//                last state is the answer below with "phase" "done" or
//                "refused" (an elevated run on Windows has no stdout).
//
// Prints one JSON line on stdout: {"ok":true,"out":...,"sectors":N,
// "files":N,"nights":N,"fat":"fat32",...} or {"ok":false,"error":"..."}.
// Exit 0 when written, 2 when refused, 1 for bad arguments.
//
#include <string>
#include <vector>

namespace hms_cpap::sleepbridge {

int runReadCard(const std::vector<std::string>& args);

}  // namespace hms_cpap::sleepbridge
