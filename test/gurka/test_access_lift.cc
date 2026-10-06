// FastGIS fork (tet-recovery WP-4): the access-ban lifts lift_motor_access_bans,
// lift_private_access and lift_node_access_bans. Each test builds a short
// way B-C (the stretch under test) with a long legal detour B-E-F-C around it,
// and checks which way motorcycle and auto take with the switches off and on.
#include "gurka.h"

#include <gtest/gtest.h>

using namespace valhalla;

namespace {

// B-1-C is the stretch under test (200 m). The legal detour B-E-F-C runs 60
// rows down and back (about 12 km of primary), so a lifted slow track, path or
// service road (and the 200 m grid between them) still wins on time.
std::string make_map() {
  std::string m = "\n    A----B1C----D\n";
  for (int i = 0; i < 60; ++i) {
    m += "         | |\n";
  }
  m += "         E-F\n";
  return m;
}
const std::string ascii_map = make_map();

gurka::map build(const std::string& name,
                 const std::map<std::string, std::string>& bc_tags,
                 const std::map<std::string, std::string>& node_tags = {}) {
  const gurka::ways ways = {
      {"AB", {{"highway", "primary"}}},
      {"B1", bc_tags},
      {"1C", bc_tags},
      {"CD", {{"highway", "primary"}}},
      {"BE", {{"highway", "primary"}}},
      {"EF", {{"highway", "primary"}}},
      {"FC", {{"highway", "primary"}}},
  };
  gurka::nodes nodes;
  if (!node_tags.empty()) {
    nodes = {{"1", node_tags}};
  }
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, 100);
  return gurka::buildtiles(layout, ways, nodes, {}, "test/data/gurka_access_lift_" + name);
}

const std::vector<std::string> kDetour = {"AB", "BE", "EF", "FC", "CD"};
const std::vector<std::string> kThrough = {"AB", "B1", "1C", "CD"};

std::unordered_map<std::string, std::string> opts(const std::string& costing,
                                                  const std::vector<std::string>& keys) {
  std::unordered_map<std::string, std::string> out;
  for (const auto& k : keys) {
    out["/costing_options/" + costing + "/" + k] = "1";
  }
  return out;
}

void expect_route(const gurka::map& map,
                  const std::string& costing,
                  const std::vector<std::string>& keys,
                  const std::vector<std::string>& path) {
  auto result =
      gurka::do_action(valhalla::Options::route, map, {"A", "D"}, costing, opts(costing, keys));
  gurka::assert::raw::expect_path(result, path);
}

const std::vector<std::string> kMotor = {"motorcycle", "auto"};
const std::vector<std::string> kAll = {"lift_motor_access_bans", "lift_private_access",
                                       "lift_node_access_bans"};

} // namespace

// A route that ends on the stretch itself: B -> 1. Without a lift loki snaps 1
// to the nearest edge the vehicle may use instead, so the path is not B1.
void expect_reach(const gurka::map& map,
                  const std::string& costing,
                  const std::vector<std::string>& keys,
                  bool reachable) {
  SCOPED_TRACE(costing);
  std::vector<std::string> names;
  try {
    auto result =
        gurka::do_action(valhalla::Options::route, map, {"B", "1"}, costing, opts(costing, keys));
    for (const auto& path : gurka::detail::get_paths(result)) {
      names.insert(names.end(), path.begin(), path.end());
    }
  } catch (const std::exception&) {
  }
  if (reachable) {
    EXPECT_EQ(names, std::vector<std::string>({"B1"}));
  } else {
    EXPECT_NE(names, std::vector<std::string>({"B1"}));
  }
}

TEST(AccessLift, MotorVehicleNoRoad) {
  const auto map = build("mv_no_road", {{"highway", "residential"}, {"motor_vehicle", "no"}});
  for (const auto& c : kMotor) {
    expect_route(map, c, {}, kDetour);
    expect_route(map, c, {"lift_private_access", "lift_node_access_bans"}, kDetour);
    expect_route(map, c, {"lift_motor_access_bans"}, kThrough);
  }
}

TEST(AccessLift, MotorVehicleNoTrack) {
  const auto map = build("mv_no_track", {{"highway", "track"}, {"motor_vehicle", "no"}});
  for (const auto& c : kMotor) {
    expect_reach(map, c, {}, false);
    expect_reach(map, c, {"lift_private_access", "lift_node_access_bans"}, false);
    expect_reach(map, c, {"lift_motor_access_bans"}, true);
  }
}

TEST(AccessLift, FootwayPathCyclewayBridleway) {
  for (const std::string hw : {"footway", "path", "cycleway", "bridleway"}) {
    SCOPED_TRACE(hw);
    const auto map = build("hw_" + hw, {{"highway", hw}});
    for (const auto& c : kMotor) {
      expect_reach(map, c, {}, false);
      expect_reach(map, c, {"lift_motor_access_bans"}, true);
    }
  }
}

TEST(AccessLift, StepsNeverLifted) {
  const auto map = build("steps", {{"highway", "steps"}});
  for (const auto& c : kMotor) {
    expect_reach(map, c, kAll, false);
    expect_route(map, c, kAll, kDetour);
  }
}

TEST(AccessLift, PrivateWay) {
  // access=private is destination_only: the router keeps it to the ends of a
  // route (the detour); the motor-ban lift alone does not change that.
  const auto map = build("private", {{"highway", "residential"}, {"access", "private"}});
  for (const auto& c : kMotor) {
    expect_route(map, c, {}, kDetour);
    expect_route(map, c, {"lift_motor_access_bans"}, kDetour);
    expect_route(map, c, {"lift_private_access"}, kThrough);
  }
}

TEST(AccessLift, DestinationWay) {
  const auto map = build("destination", {{"highway", "residential"}, {"access", "destination"}});
  for (const auto& c : kMotor) {
    expect_route(map, c, {}, kDetour);
    expect_route(map, c, {"lift_motor_access_bans"}, kDetour);
    expect_route(map, c, {"lift_private_access"}, kThrough);
  }
}

TEST(AccessLift, PrivateWayClosedToMotorVehicles) {
  // Closed to motor vehicles AND destination_only: level c only.
  const auto map = build("private_mv_no", {{"highway", "residential"},
                                           {"access", "private"},
                                           {"motor_vehicle", "no"}});
  for (const auto& c : kMotor) {
    expect_route(map, c, {}, kDetour);
    expect_route(map, c, {"lift_motor_access_bans"}, kDetour);
    expect_route(map, c, {"lift_private_access"}, kDetour);
    expect_route(map, c, {"lift_motor_access_bans", "lift_private_access"}, kThrough);
  }
}

TEST(AccessLift, GateClosedToMotorVehicles) {
  const auto map = build("gate_mv_no", {{"highway", "primary"}},
                         {{"barrier", "gate"}, {"motor_vehicle", "no"}});
  for (const auto& c : kMotor) {
    expect_route(map, c, {}, kDetour);
    expect_route(map, c, {"lift_motor_access_bans", "lift_private_access"}, kDetour);
    expect_route(map, c, {"lift_node_access_bans"}, kThrough);
  }
}

TEST(AccessLift, BollardStaysClosed) {
  const auto map = build("bollard", {{"highway", "primary"}}, {{"barrier", "bollard"}});
  for (const auto& c : kMotor) {
    expect_route(map, c, kAll, kDetour);
  }
}

TEST(AccessLift, AccessNoIsNotInTheGraph) {
  // A way no modelled mode may use (access=no) is dropped by the tile builder
  // (bridleways aside), so even all three lifts have nothing to open.
  const auto map = build("access_no", {{"highway", "residential"}, {"access", "no"}});
  for (const auto& c : kMotor) {
    expect_route(map, c, {}, kDetour);
    expect_route(map, c, kAll, kDetour);
  }
}

TEST(AccessLift, AccessNoFootYesIsAMotorBan) {
  const auto map =
      build("access_no_foot", {{"highway", "residential"}, {"access", "no"}, {"foot", "yes"}});
  for (const auto& c : kMotor) {
    expect_route(map, c, {}, kDetour);
    expect_route(map, c, {"lift_motor_access_bans"}, kThrough);
  }
}

TEST(AccessLift, OpenWayUnchanged) {
  // The switches never change a route that needs no lift.
  const auto map = build("open", {{"highway", "primary"}});
  for (const auto& c : kMotor) {
    expect_route(map, c, {}, kThrough);
    expect_route(map, c, kAll, kThrough);
  }
}

TEST(AccessLift, TraceRouteThroughClosedTrack) {
  // The enhance's matcher: a shape drawn along A-B-1-C-D.
  const auto map = build("trace_mv_no", {{"highway", "track"}, {"motor_vehicle", "no"}});
  for (const auto& c : kMotor) {
    auto lifted = gurka::do_action(valhalla::Options::trace_route, map, {"A", "B", "1", "C", "D"},
                                   c, opts(c, {"lift_motor_access_bans"}));
    gurka::assert::raw::expect_path(lifted, kThrough);
  }
}
