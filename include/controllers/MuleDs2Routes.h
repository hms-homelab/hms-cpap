#pragma once
#ifdef BUILD_WITH_WEB
//
// SDD-065: a DreamStation 2 read through a Mule & Miner, from the Devices page
// (web port). The mode and the pairing are plain unit commands on
// POST /api/mule/command (config {"ble_machine"}, ds2_bond); these two need
// what this host keeps:
//
//   POST /api/mule/{serial}/ds2/read          read now, from the stored bookmarks:
//                                             202 {ok, result: "reading", req_id};
//                                             409 {ok: false, result: offline |
//                                             mode_off | not_paired | busy}
//   POST /api/mule/{serial}/ds2/import-from   {date: "YYYY-MM-DD"}: nights from that day
//
#include "mule/MuleDs2.h"

namespace hms_cpap::mule {

/// Registers the routes above; `ds2` outlives the app. Null: nothing.
void installDs2Routes(MuleDs2* ds2);

}  // namespace hms_cpap::mule

#endif  // BUILD_WITH_WEB
