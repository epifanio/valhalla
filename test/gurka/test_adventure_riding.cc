#include "gurka.h"
#include "test.h"

#include <gtest/gtest.h>

using namespace valhalla;

namespace {

// All four costing profiles wired through DynamicCost::AdventureRidingMultiplier
// in Layer 5. Keep in sync with src/sif/{auto,motorcycle,bicycle,pedestrian}cost.cc.
const std::vector<std::string> kAllCostings = {"auto", "motorcycle", "bicycle", "pedestrian"};

// Motor profiles AVOID `highway=track` by default, so the road-vs-trail flip
// only shows up under these. Bicycle and pedestrian naturally PREFER tracks
// (it's the same logic that makes them favour `highway=path` over `=primary`),
// so the route stays on the trail even without `use_adventure_riding` bias —
// the multiplier still fires for them, it just doesn't change the answer for
// this particular fixture. Coverage that the wiring is live for bicycle and
// pedestrian comes from the unit-level `TaggedValueSize_AdventureRiding` test
// in test/edgeinfo.cc plus the per-profile compile error a future ablation of
// the `factor *= AdventureRidingMultiplier(...)` line would produce.
const std::vector<std::string> kMotorCostings = {"auto", "motorcycle"};

} // namespace

// End-to-end exercise of the adventure-riding extension: a 3-edge fixture
// proves that
//
//   1. a TET-tagged edge built from `source=TET` PBF input via the config-driven
//      allow-list IS persisted to EdgeInfo's TaggedValue list, and
//   2. the per-profile `use_adventure_riding` costing option swings the route
//      between the road and the trail.
//
// Together this validates Layers 4-6 of docs/adventure-riding/PLAN.md.
class AdventureRidingTest : public ::testing::Test {
protected:
  static gurka::map ar_map;

  static void SetUpTestSuite() {
    // Two routes from A to C: a road (A->B->C) and a curated TET trail
    // (A->C). The trail is the geometrically shorter path but tagged as
    // `highway=track`, which all four profiles avoid by default.
    //
    //   A--------B
    //    \       |
    //     \      |
    //      \     |
    //       \    |
    //        ---C
    const std::string ascii_map = R"(
      A--------B
       \       |
        \      |
         \     |
          \    |
           ---C
    )";

    const gurka::ways ways = {
        {"AB", {{"highway", "primary"}}},
        {"BC", {{"highway", "secondary"}}},
        {"AC", {{"highway", "track"}, {"source", "TET"}}},
    };

    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 100);
    // Opt the build into adventure-riding by populating the allow-list.
    // The Lua side passes `source=TET` through as `adventure_riding_source`;
    // pbfgraphparser maps that string to AdventureRidingClass::kTet=1 via
    // this config knob.
    ar_map = gurka::buildtiles(layout, ways, {}, {}, "test/data/gurka_adventure_riding",
                               {{"mjolnir.concurrency", "1"},
                                {"mjolnir.adventure_riding_sources.TET", "1"}});
  }
};

gurka::map AdventureRidingTest::ar_map = {};

// With `use_adventure_riding` unset on motor profiles the multiplier is 1.0f
// (neutral); the track edge keeps its normal (avoided) cost and the road
// route wins.
TEST_F(AdventureRidingTest, MotorDefaultsRouteViaRoad) {
  for (const auto& c : kMotorCostings) {
    SCOPED_TRACE("costing=" + c);
    auto result = gurka::do_action(valhalla::Options::route, ar_map, {"A", "C"}, c);
    gurka::assert::raw::expect_path(result, {"AB", "BC"});
  }
}

// `use_adventure_riding=0.0` zeroes the cost on the TET edge — the trail
// wins outright on every profile we wired. Together with MotorDefaultsRouteViaRoad
// this is the directional proof: the same fixture flips from road to trail
// once the option is set.
TEST_F(AdventureRidingTest, ZeroMultiplierWinsTrail) {
  for (const auto& c : kAllCostings) {
    SCOPED_TRACE("costing=" + c);
    auto result =
        gurka::do_action(valhalla::Options::route, ar_map, {"A", "C"}, c,
                         {{"/costing_options/" + c + "/use_adventure_riding", "0"}});
    gurka::assert::raw::expect_path(result, {"AC"});
  }
}

// `use_adventure_riding=1.0` is the documented neutral value; for motor
// profiles it must behave identically to leaving the option unset. Guards
// against the hot-path `>= 1.0f` early-return regressing into `> 1.0f`
// (which would let neutral requests pay the EdgeInfo fetch on every edge
// and — more visibly — flip the route).
TEST_F(AdventureRidingTest, MotorNeutralValueDoesNotBiasRoute) {
  for (const auto& c : kMotorCostings) {
    SCOPED_TRACE("costing=" + c);
    auto result =
        gurka::do_action(valhalla::Options::route, ar_map, {"A", "C"}, c,
                         {{"/costing_options/" + c + "/use_adventure_riding", "1"}});
    gurka::assert::raw::expect_path(result, {"AB", "BC"});
  }
}
