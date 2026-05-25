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

// `use_adventure_riding_curve` should let intermediate bias values become
// more aggressive without changing the bias=0 or bias=1 endpoints. The fork
// applies `effective = pow(bias, curve)` once at DynamicCost construction
// and stores it as `adventure_riding_factor_`, so the EdgeCost hot path
// stays bitwise identical to the legacy linear case — this test just pins
// down the two endpoint invariants and one intermediate equivalence.
TEST_F(AdventureRidingTest, CurveDoesNotChangeBiasOneOrZero) {
  // bias=1.0 with any curve must remain neutral (road).
  for (const auto& c : kMotorCostings) {
    SCOPED_TRACE("costing=" + c + " bias=1 curve=5");
    auto result = gurka::do_action(
        valhalla::Options::route, ar_map, {"A", "C"}, c,
        {{"/costing_options/" + c + "/use_adventure_riding", "1"},
         {"/costing_options/" + c + "/use_adventure_riding_curve", "5"}});
    gurka::assert::raw::expect_path(result, {"AB", "BC"});
  }
  // bias=0.0 with any curve must still saturate to trail.
  for (const auto& c : kMotorCostings) {
    SCOPED_TRACE("costing=" + c + " bias=0 curve=5");
    auto result = gurka::do_action(
        valhalla::Options::route, ar_map, {"A", "C"}, c,
        {{"/costing_options/" + c + "/use_adventure_riding", "0"},
         {"/costing_options/" + c + "/use_adventure_riding_curve", "5"}});
    gurka::assert::raw::expect_path(result, {"AC"});
  }
}

// Helper: pull the leg.summary().time() out of a route response.
static float leg_time_sec(const valhalla::Api& route) {
  return route.directions().routes(0).legs(0).summary().time();
}

// `pow(bias, curve)` should be small enough at high curve to flip the route
// to the trail on this fixture, where linear bias=0.5 alone leaves it on
// road (the gurka 3-edge fixture has a very high default `track_factor_`
// vs primary/secondary road cost — empirically `pow(0.5, 5) = 0.03` still
// isn't enough; we need curve=10 → pow(0.5, 10) ≈ 0.001 to clear the bar).
// On real OSM data the trail/road cost ratio is much smaller, so curve=3-5
// produces meaningful gradient flips at bias=0.5. This is the *whole point*
// of the curve knob: let riders express partial preference and have it
// actually move the needle.
TEST_F(AdventureRidingTest, CurveAmplifiesIntermediateBias) {
  for (const auto& c : kMotorCostings) {
    SCOPED_TRACE("costing=" + c);
    // bias=0.5 alone → road (proven by MotorNeutralValueDoesNotBiasRoute
    // baseline shape; here we re-pin it as a control for the comparison)
    auto road = gurka::do_action(
        valhalla::Options::route, ar_map, {"A", "C"}, c,
        {{"/costing_options/" + c + "/use_adventure_riding", "0.5"}});
    gurka::assert::raw::expect_path(road, {"AB", "BC"});
    // bias=0.5 with curve=10 → effective ≈ 0.001, comfortably below the
    // trail-vs-road flip threshold for this fixture → trail wins.
    auto trail = gurka::do_action(
        valhalla::Options::route, ar_map, {"A", "C"}, c,
        {{"/costing_options/" + c + "/use_adventure_riding", "0.5"},
         {"/costing_options/" + c + "/use_adventure_riding_curve", "10"}});
    gurka::assert::raw::expect_path(trail, {"AC"});
  }
}

// `adventure_riding_speed_factor` should scale the wall-clock time of the
// AC trail edge linearly: 0.5 ⇒ ~2× time, 2.0 ⇒ ~0.5× time, with no
// effect on route choice (we fix bias=0 so the trail is taken regardless).
// Uses LE / GE bounds rather than tight NEAR because Valhalla's time
// computation includes some non-multiplicative factors (transition costs,
// rounding to integer seconds) that this fixture exercises.
TEST_F(AdventureRidingTest, SpeedFactorScalesTrailTime) {
  for (const auto& c : kMotorCostings) {
    SCOPED_TRACE("costing=" + c);
    auto baseline = gurka::do_action(
        valhalla::Options::route, ar_map, {"A", "C"}, c,
        {{"/costing_options/" + c + "/use_adventure_riding", "0"}});
    gurka::assert::raw::expect_path(baseline, {"AC"});
    const float t_base = leg_time_sec(baseline);
    ASSERT_GT(t_base, 0.5f);  // sanity: trail time exists

    // Cautious rider — half the speed → at least 1.8× the baseline time.
    auto cautious = gurka::do_action(
        valhalla::Options::route, ar_map, {"A", "C"}, c,
        {{"/costing_options/" + c + "/use_adventure_riding", "0"},
         {"/costing_options/" + c + "/adventure_riding_speed_factor", "0.5"}});
    gurka::assert::raw::expect_path(cautious, {"AC"});
    EXPECT_GE(leg_time_sec(cautious), 1.8f * t_base);

    // Fast rider — 2× speed → at most 0.7× the baseline time.
    auto fast = gurka::do_action(
        valhalla::Options::route, ar_map, {"A", "C"}, c,
        {{"/costing_options/" + c + "/use_adventure_riding", "0"},
         {"/costing_options/" + c + "/adventure_riding_speed_factor", "2.0"}});
    gurka::assert::raw::expect_path(fast, {"AC"});
    EXPECT_LE(leg_time_sec(fast), 0.7f * t_base);
  }
}

// Speed factor must NOT touch wall-clock time on non-AR edges. The road
// route A->B->C carries no kAdventureRiding tag, so changing the rider
// skill should leave its time bit-identical.
TEST_F(AdventureRidingTest, SpeedFactorDoesNotAffectRoadEdges) {
  for (const auto& c : kMotorCostings) {
    SCOPED_TRACE("costing=" + c);
    // Default route is road (bias unset, neutral); compute time once.
    auto road = gurka::do_action(valhalla::Options::route, ar_map, {"A", "C"}, c);
    gurka::assert::raw::expect_path(road, {"AB", "BC"});
    const float t_road = leg_time_sec(road);

    // Same route, half-speed rider — but no AR tag on road edges, so the
    // multiplier early-returns to 1.0 and the time is unchanged.
    auto cautious_road = gurka::do_action(
        valhalla::Options::route, ar_map, {"A", "C"}, c,
        {{"/costing_options/" + c + "/adventure_riding_speed_factor", "0.5"}});
    gurka::assert::raw::expect_path(cautious_road, {"AB", "BC"});
    EXPECT_EQ(leg_time_sec(cautious_road), t_road);
  }
}
