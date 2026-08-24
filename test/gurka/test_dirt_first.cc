#include "gurka.h"
#include "test.h"

#include <gtest/gtest.h>

using namespace valhalla;

namespace {

// The profiles wired through DynamicCost::DirtFirstMultiplier. Keep in sync
// with src/sif/{auto,motorcycle,bicycle,pedestrian}cost.cc.
const std::vector<std::string> kMotorCostings = {"auto", "motorcycle"};

// Stock costing carries its own unpaved-avoidance machinery that composes
// with — and at defaults can outweigh — the dirt-first discount:
// motorcycle's use_trails=0 adds surface_factor_ * kSurfaceFactor on every
// unpaved edge, and auto's use_tracks=0 adds a 300 s track_penalty_ plus a
// large track_factor_ on Use::kTrack edges regardless of surface. A
// dirt-first request must neutralise both (the server presets send exactly
// these companions), so these tests do the same on control AND test
// requests — the only variable under test is the dirt-first axis itself.
std::unordered_map<std::string, std::string> dirt_first_options(const std::string& costing,
                                                                const std::string& strength) {
  std::unordered_map<std::string, std::string> opts;
  if (!strength.empty()) {
    opts["/costing_options/" + costing + "/use_dirt_first"] = strength;
  }
  opts["/costing_options/" + costing + "/use_tracks"] = "1";
  if (costing == "motorcycle") {
    opts["/costing_options/" + costing + "/use_trails"] = "1";
  }
  return opts;
}

float leg_time_sec(const valhalla::Api& route) {
  return route.directions().routes(0).legs(0).summary().time();
}

float total_time_sec(const valhalla::Api& route) {
  float t = 0.f;
  for (const auto& leg : route.directions().routes(0).legs()) {
    t += leg.summary().time();
  }
  return t;
}

} // namespace

// Two routes from A to C over the SAME highway class, so class factors and
// default speeds are identical and surface is the only differentiator:
//
//   A----B----C     asphalt (short)
//   |         |
//   D---------E     the long way round, unpaved
//
// The unpaved detour is strictly longer, so stock routing must take the
// asphalt and dirt-first routing must take the detour.
class DirtFirstTest : public ::testing::Test {
protected:
  static gurka::map gravel_map;
  static gurka::map track_map;

  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      A----B----C
      |         |
      |         |
      D---------E
    )";
    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 100);

    // Fixture 1: the detour is tagged surface=gravel.
    const gurka::ways gravel_ways = {
        {"AB", {{"highway", "unclassified"}, {"surface", "asphalt"}}},
        {"BC", {{"highway", "unclassified"}, {"surface", "asphalt"}}},
        {"AD", {{"highway", "unclassified"}, {"surface", "gravel"}}},
        {"DE", {{"highway", "unclassified"}, {"surface", "gravel"}}},
        {"EC", {{"highway", "unclassified"}, {"surface", "gravel"}}},
    };
    gravel_map = gurka::buildtiles(layout, gravel_ways, {}, {}, "test/data/gurka_dirt_first_gravel",
                                   {{"mjolnir.concurrency", "1"}});

    // Fixture 2: the detour is an UNTAGGED highway=track — no surface, no
    // tracktype, no smoothness. The parser defaults Use::kTrack to
    // Surface::kDirt (src/mjolnir/pbfgraphparser.cc), which is exactly what
    // makes dirt-first work across the huge untagged-track population in
    // Nordic OSM (98.7-100% ground-truth agreement, NO+SE probe 2026-08-24).
    const gurka::ways track_ways = {
        {"AB", {{"highway", "unclassified"}, {"surface", "asphalt"}}},
        {"BC", {{"highway", "unclassified"}, {"surface", "asphalt"}}},
        {"AD", {{"highway", "track"}}},
        {"DE", {{"highway", "track"}}},
        {"EC", {{"highway", "track"}}},
    };
    track_map = gurka::buildtiles(layout, track_ways, {}, {}, "test/data/gurka_dirt_first_track",
                                  {{"mjolnir.concurrency", "1"}});
  }
};

gurka::map DirtFirstTest::gravel_map = {};
gurka::map DirtFirstTest::track_map = {};

// Without the option the asphalt route wins: it is shorter and unpaved
// surfaces carry no discount.
TEST_F(DirtFirstTest, DefaultStaysOnAsphalt) {
  for (const auto& c : kMotorCostings) {
    SCOPED_TRACE("costing=" + c);
    auto result = gurka::do_action(valhalla::Options::route, gravel_map, {"A", "C"}, c,
                                   dirt_first_options(c, ""));
    gurka::assert::raw::expect_path(result, {"AB", "BC"});
  }
}

// Full strength inverts the preference: gravel at ~0.45x, asphalt at 3x —
// the longer unpaved detour wins.
TEST_F(DirtFirstTest, FullStrengthFlipsToGravel) {
  for (const auto& c : kMotorCostings) {
    SCOPED_TRACE("costing=" + c);
    auto result = gurka::do_action(valhalla::Options::route, gravel_map, {"A", "C"}, c,
                                   dirt_first_options(c, "1"));
    gurka::assert::raw::expect_path(result, {"AD", "DE", "EC"});
  }
}

// use_dirt_first=0 is the documented "off" value and must be bitwise
// identical to leaving the option unset: same path, same wall-clock time.
// Guards the dirt_first_active_ hot-path gate.
TEST_F(DirtFirstTest, ZeroStrengthIsExactlyStock) {
  for (const auto& c : kMotorCostings) {
    SCOPED_TRACE("costing=" + c);
    auto unset = gurka::do_action(valhalla::Options::route, gravel_map, {"A", "C"}, c,
                                  dirt_first_options(c, ""));
    auto zero = gurka::do_action(valhalla::Options::route, gravel_map, {"A", "C"}, c,
                                 dirt_first_options(c, "0"));
    gurka::assert::raw::expect_path(unset, {"AB", "BC"});
    gurka::assert::raw::expect_path(zero, {"AB", "BC"});
    EXPECT_EQ(leg_time_sec(zero), leg_time_sec(unset));
  }
}

// The untagged-track fixture: the detour edges carry NO surface information
// at all, yet dirt-first must still pick them up via the parser's
// Use::kTrack -> Surface::kDirt default. Control and test requests share
// identical companion options so the flip is attributable to the axis alone.
TEST_F(DirtFirstTest, UntaggedTrackCountsAsDirt) {
  for (const auto& c : kMotorCostings) {
    SCOPED_TRACE("costing=" + c);
    auto control = gurka::do_action(valhalla::Options::route, track_map, {"A", "C"}, c,
                                    dirt_first_options(c, ""));
    gurka::assert::raw::expect_path(control, {"AB", "BC"});

    auto dirt = gurka::do_action(valhalla::Options::route, track_map, {"A", "C"}, c,
                                 dirt_first_options(c, "1"));
    gurka::assert::raw::expect_path(dirt, {"AD", "DE", "EC"});
  }
}

// The speed floor: stock tiles store 5 km/h (halved to 2 for the dirt
// surface) on untagged tracks — a crawling car, not a dirt bike. With
// dirt-first at full strength the same forced track route (via D) must get
// dramatically faster wall-clock time from the 30 km/h floor. Uses a 5x
// bound (actual is ~10-15x) to stay robust to transition-cost noise.
TEST_F(DirtFirstTest, SpeedFloorMakesTrackTimeSane) {
  for (const auto& c : kMotorCostings) {
    SCOPED_TRACE("costing=" + c);
    auto stock = gurka::do_action(valhalla::Options::route, track_map, {"A", "D", "C"}, c,
                                  dirt_first_options(c, ""));
    auto floored = gurka::do_action(valhalla::Options::route, track_map, {"A", "D", "C"}, c,
                                    dirt_first_options(c, "1"));
    EXPECT_GE(total_time_sec(stock), 5.f * total_time_sec(floored));
  }
}

// Out-of-range values snap to the DEFAULT (0 = off) per ranged_default_t —
// not to the nearest bound. A bogus strength must not error, and must not
// accidentally give the caller full dirt-first either.
TEST_F(DirtFirstTest, OutOfRangeSnapsToOff) {
  for (const auto& c : kMotorCostings) {
    SCOPED_TRACE("costing=" + c);
    auto result = gurka::do_action(valhalla::Options::route, gravel_map, {"A", "C"}, c,
                                   dirt_first_options(c, "5"));
    gurka::assert::raw::expect_path(result, {"AB", "BC"});
  }
}
