#pragma once
#ifdef BUILD_WITH_WEB
//
// SDD-013: the local sleep model's routes, on the web port.
//
//   GET    /api/local-model/status    what the Settings section shows
//   POST   /api/local-model/enable    the switch on: 409 with the reason while
//                                     no model is published; else 202 (on at
//                                     once, or the download queued)
//   POST   /api/local-model/disable   the switch off; the model file stays
//   POST   /api/local-model/file      "Load model from file" (multipart field
//                                     "file"): 409 while no model is published;
//                                     else 202, the hash checked on the worker
//
// The handlers only route; what they decide is in model/LocalModelService.h,
// which the suite covers.
//
namespace hms_cpap::model {

class LocalModelService;

/// Registers the routes above. Must precede app().run(); [service] outlives
/// the app (null: every route answers 404).
void installRoutes(LocalModelService* service);

}  // namespace hms_cpap::model

#endif  // BUILD_WITH_WEB
