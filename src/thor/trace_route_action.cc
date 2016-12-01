#include <prime_server/prime_server.hpp>
using namespace prime_server;

#include <memory>

#include <valhalla/midgard/logging.h>
#include <valhalla/baldr/geojson.h>
#include <valhalla/baldr/pathlocation.h>
#include <valhalla/baldr/errorcode_util.h>
#include <valhalla/meili/map_matcher.h>
#include <valhalla/proto/trippath.pb.h>

#include "thor/service.h"
#include "thor/route_matcher.h"
#include "thor/map_matcher.h"
#include "thor/trippathbuilder.h"

using namespace valhalla;
using namespace valhalla::baldr;
using namespace valhalla::sif;
using namespace valhalla::thor;

namespace valhalla {
namespace thor {

/*
 * The trace_route action takes a GPS trace and turns it into a route result.
 */
worker_t::result_t thor_worker_t::trace_route(const boost::property_tree::ptree &request,
    const std::string &request_str, const bool header_dnt) {
  parse_locations(request);
  parse_shape(request);
  parse_costing(request);

  //get time for start of request
  auto s = std::chrono::system_clock::now();
  worker_t::result_t result { true };
  // Forward the original request
  result.messages.emplace_back(request_str);

  // If the exact points from a prior route that was run agains the Valhalla road network,
  //then we can traverse the exact shape to form a path by using edge-walking algorithm
  odin::TripPath trip_path = route_match();
  if (trip_path.node().size() == 0) {
    //If no Valhalla route match, then use meili map matching
    //to match to local route network. No shortcuts are used and detailed
    //information at every intersection becomes available.
    trip_path = map_match();
  }
  result.messages.emplace_back(trip_path.SerializeAsString());
  // Get processing time for thor
  auto e = std::chrono::system_clock::now();
  std::chrono::duration<float, std::milli> elapsed_time = e - s;
  // TODO determine what to log
  //log request if greater than X (ms)
  if (!header_dnt
      && (elapsed_time.count() / correlated.size()) > long_request) {
    LOG_WARN(
        "thor::trace_route elapsed time (ms)::"
            + std::to_string(elapsed_time.count()));
    LOG_WARN("thor::trace_route exceeded threshold::" + request_str);
    midgard::logging::Log("valhalla_thor_long_request_trace_route",
                          " [ANALYTICS] ");
  }
  return result;
}


/*
 * Returns trip path using an “edge-walking” algorithm.
 * This is for use when the input shape is exact shape from a prior Valhalla route.
 * This will walk the input shape and compare to Valhalla edge’s end node positions to
 * form the list of edges. It will return no nodes if path not found.
 *
 */
odin::TripPath thor_worker_t::route_match() {
  odin::TripPath trip_path;
  std::vector<PathInfo> path_infos;
  if (RouteMatcher::FormPath(mode_costing, mode, reader, shape, correlated, path_infos)) {
    // Empty through location list
    std::vector<baldr::PathLocation> through_loc;

    // Form the trip path based on mode costing, origin, destination, and path edges
    trip_path = thor::TripPathBuilder::Build(reader, mode_costing, path_infos,
                                             correlated.front(),
                                             correlated.back(), through_loc);

  }
  return trip_path;
}

// Form the path from the map-matching results. This path gets sent to TripPathBuilder.
// PathInfo is primarily a list of edge Ids but it also include elapsed time to the end
// of each edge. We will need to use the existing costing method to form the elapsed time
// the path. We will start with just using edge costs and will add transition costs.
odin::TripPath thor_worker_t::map_match() {
  odin::TripPath trip_path;
  // Call Meili for map matching to get a collection of pathLocation Edges
  // Create a matcher
  std::shared_ptr<meili::MapMatcher> matcher;
  try {
    matcher.reset(matcher_factory.Create(config));
  } catch (const std::invalid_argument& ex) {
    //return jsonify_error({400, 499}, request_info, std::string(ex.what()));
    throw std::runtime_error(std::string(ex.what()));
  }

  std::vector<meili::Measurement> sequence;
  for (const auto& coord : shape) {
    sequence.emplace_back(coord, gps_accuracy, search_radius);
  }

  // Create the vector of matched path results
  std::vector<meili::MatchResult> results;
  for (size_t i = 0; i < sequence.size(); i++) {
    results = (matcher->OfflineMatch(sequence));
  }

  // Form the path edges based on the matched points
  std::vector<PathInfo> path_edges = MapMatcher::FormPath(matcher.get(),
                                                          results, mode_costing,
                                                          mode);

  // Set origin and destination from map matching results
  auto first_result_with_state = std::find_if(
      results.begin(), results.end(),
      [](const meili::MatchResult& result) {return result.HasState();});
  auto last_result_with_state = std::find_if(
      results.rbegin(), results.rend(),
      [](const meili::MatchResult& result) {return result.HasState();});
  if ((first_result_with_state != results.end())
      && (last_result_with_state != results.rend())) {
    baldr::PathLocation origin = matcher->mapmatching().state(
        first_result_with_state->stateid()).candidate();
    baldr::PathLocation destination = matcher->mapmatching().state(
        last_result_with_state->stateid()).candidate();

    // Empty through location list
    std::vector<baldr::PathLocation> through_loc;

    // Form the trip path based on mode costing, origin, destination, and path edges
    trip_path = thor::TripPathBuilder::Build(matcher->graphreader(),
                                                  mode_costing, path_edges,
                                                  origin, destination,
                                                  through_loc);
  } else {
    throw baldr::valhalla_exception_t { 400, 442 };
  }
  return trip_path;
}

}
}
