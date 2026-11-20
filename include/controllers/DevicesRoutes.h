#pragma once
#ifdef BUILD_WITH_WEB
//
// The Devices page's routes (SDD-062), on the web port:
//
//   GET    /api/devices                    every device, one answer (devices/Devices.h);
//                                          ?fresh=1 also reads the mule manifest and the
//                                          bridge release feed now (the page's first load)
//   PUT    /api/devices/{kind}/{id}/name   {"name"}: a local name; "" clears it
//   DELETE /api/devices/{kind}/{id}        forget an away Mule & Miner or Sleep Bridge
//   GET    /api/source/status              the card source's reachability
//   GET    /api/oximetry/status            the O2 ring, as last talked to
//   GET    /api/garmin/status              SDD-078: the Garmin card's state
//   POST   /api/garmin/fetch               "Fetch from Garmin now" (an event, D2)
//   POST   /api/garmin/mfa                 {"code"}: the code Garmin asked for
//   POST   /api/airsense11/ble/pull        the machine's summary pulled now
//   DELETE /api/airsense11/ble/pairing     the saved AirSense 11 pairing removed
//   GET    /api/oscar                      SDD-080: the three Settings fields, the
//                                          program found, the check, the reader's state
//   PUT    /api/oscar                      {"program","data_dir","profile"}: checked
//                                          (D7), saved, and the check answered
//   POST   /api/oscar/read                 "Read now": one OSCAR read of the card
//
// The handlers only gather what each service reports and route; what they
// decide is in devices/Devices.h and the services, which the suite covers.
//
#include "devices/DevicesStore.h"

#include <json/json.h>

#include <memory>
#include <string>

namespace hms_cpap {
struct AppConfig;
class BurstCollectorService;
class GarminService;
class QueryService;
namespace mule {
class MuleService;
class FirmwareUpdater;
class MuleDs2;
}  // namespace mule
namespace sleepbridge {
class SleepBridgeService;
class ReleaseFeed;
}  // namespace sleepbridge
}  // namespace hms_cpap

namespace hms_cpap::devices {

struct RouteDeps {
    AppConfig*                       config{nullptr};
    BurstCollectorService*           burst{nullptr};
    std::shared_ptr<QueryService>    query;
    mule::MuleService*               mule{nullptr};
    mule::FirmwareUpdater*           updater{nullptr};
    mule::MuleDs2*                   mule_ds2{nullptr};   // SDD-065: each unit's "ds2_local"
    sleepbridge::SleepBridgeService* bridges{nullptr};
    sleepbridge::ReleaseFeed*        feed{nullptr};
    DevicesStore*                    store{nullptr};
    GarminService*                   garmin{nullptr};     // SDD-078
    int                              stream_port{8894};
    std::string                      config_path;         // SDD-080: PUT /api/oscar saves here
};

/// Registers the routes above. Must precede app().run(); everything in
/// `deps` outlives the app (null: that kind is off).
void installRoutes(const RouteDeps& deps);

/// The last Test of the card's address (GET /api/config/test-ezshare), kept
/// so the ez Share card can say it is a Mule & Miner in card mode (D11).
void rememberSourceProbe(const Json::Value& probe);

}  // namespace hms_cpap::devices

#endif  // BUILD_WITH_WEB
