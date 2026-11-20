#pragma once

#include <string>

namespace hms_cpap::datamove {

/**
 * SDD-067 D16, D18: `hms_cpap --move-data <from> <to> [--port N]`.
 *
 * Moves the data directory (utils/DataDirMove.h), prints each progress line
 * to standard output, which lands in the add-on's Log tab, and serves only
 * the progress page on the web port while it runs:
 *
 *   api/data-move/status?after=<seq>   the progress as JSON, answered as soon
 *                                      as there is a state newer than <seq>
 *   anything else                      the progress page
 *
 * The status request is held until the move has news, so the page follows the
 * move by its events rather than asking on a clock. Returns the process exit
 * code: 0 when the move is complete, 1 when it is not.
 */
int runMoveDataMode(const std::string& from, const std::string& to, int port);

/// The self-contained progress page, <base href="/"> for the Ingress rewrite.
const std::string& progressPage();

}  // namespace hms_cpap::datamove
