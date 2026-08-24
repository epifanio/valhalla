#include "sif/motorcyclecost.h"
#include "baldr/directededge.h"
#include "baldr/graphconstants.h"
#include "baldr/nodeinfo.h"
#include "baldr/rapidjson_utils.h"
#include "proto_conversions.h"
#include "sif/costconstants.h"
#include "sif/osrm_car_duration.h"

#include <algorithm>
#include <cmath>

#ifdef INLINE_TEST
#include "test.h"
#include "worker.h"

#include <random>
#endif

using namespace valhalla::midgard;
using namespace valhalla::baldr;

namespace valhalla {
namespace sif {

// Default options/values
namespace {

// Other options
constexpr float kDefaultUseHighways = 0.5f; // Factor between 0 and 1
constexpr float kDefaultUseTolls = 0.5f;    // Factor between 0 and 1
constexpr float kDefaultUseTrails = 0.0f;   // Factor between 0 and 1
constexpr float kDefaultUseHills = 0.5f;    // Factor between 0 and 1 — 0.5 is neutral

// ── Hills axis ───────────────────────────────────────────────────────────────
// A motorcycle is not a bicycle. Bicycle costing reads use_hills one-sidedly —
// 0 avoids climbs, 1 merely stops avoiding them — because nobody pedals uphill
// for fun. A rider does: the mountain pass IS the destination. So here 0.5 is
// NEUTRAL, above it SEEKS elevation change and below it avoids it, which also
// means the value the app already sends (0.5) leaves routing bit-identical.
//
// Hilliness is measured from max_up_slope / max_down_slope, NOT weighted_grade.
// weighted_grade is the net grade across the edge, so a switchback that climbs
// 200 m and drops 200 m — precisely the road worth riding — reads as flat. The
// steepest slope actually encountered does not.
//
// FLAT IS PINNED AT 1.0 in both directions, the same invariant dirt-first holds
// for pavement. Seeking hills DISCOUNTS the hilly edges; avoiding them PENALIZES
// the hilly edges. Neither touches flat ground. The first cut had "avoid"
// discount flat edges instead — mathematically the same preference, but in a
// flat country it becomes a near-global discount, and transition penalties are
// plain seconds that do not scale with it. Measured: Warsaw→Kraków collapsed
// from 367 km to 285 km, the router buying motorway to dodge turns that had
// become relatively twice as dear. Nothing to do with hills.
//
// TWO terms, because "steep and changing elevation" is two different things and
// only one of them takes a rider up a mountain:
//
//   * SUSTAINED gradient — |weighted_grade|, the net grade across the edge.
//     Cost is proportional to length, so a discount scaled by |grade| is a
//     discount per metre climbed: it accumulates exactly the way ascent does.
//     This is the term that decides whether the route goes over the pass.
//   * UNDULATION — max_up + |max_down| within the edge. This is the hairpins
//     and the rollercoaster. It is deliberately the SMALLER term: on its own it
//     rewards short steep bits that gain no altitude at all. Measured with
//     undulation as the only term, use_hills 1.0 sent Bergen→Oslo 30 km further
//     for 40 m LESS peak altitude — plenty of steep, no mountain.
//
// weighted_grade is stored as a 4-bit bucket, grade * 0.6 + 6.5, so the table is
// indexed by the bucket directly.
constexpr uint32_t kHillsGradeBuckets = 16;
constexpr float kHillsFullGrade = 8.0f;    // percent of sustained grade = fully hilly
constexpr int kHillsFullUndulation = 20;   // up + |down| percent = fully rolling
constexpr uint32_t kHillsUndulationBuckets = 64;
// Full-strength effect on a fully hilly edge, per term.
constexpr float kHillsGradeDiscount = 0.60f;     // seek  -> 0.40
constexpr float kHillsUndulationDiscount = 0.15f; // seek  -> 0.85
constexpr float kHillsGradePenalty = 1.50f;      // avoid -> 2.50
constexpr float kHillsUndulationPenalty = 0.25f; // avoid -> 1.25

// ── Tunnels axis ─────────────────────────────────────────────────────────────
// "Skip the tunnel on a sunny day" is a TUNNEL preference, not an elevation
// one, and it is much better posed than trying to seek altitude: it is a
// property of the single edge in front of you. `exclude_tunnels` already
// exists but is a hard filter in Allowed(), so it can leave Gotthard or an
// urban underpass unroutable and fail the request. This is the soft version.
// Tunnel edges are rare, so penalising them is a TARGETED inflation, not the
// global one that flattening a whole country would be.
constexpr float kDefaultUseTunnels = 0.5f;
constexpr float kTunnelMaxDiscount = 0.25f; // prefer -> 0.75
constexpr float kTunnelMaxPenalty = 2.00f;  // avoid  -> 3.00

// ── Dirt difficulty ceiling ──────────────────────────────────────────────────
// The owner's original question: does the dirt score reflect how DEMANDING the
// riding is? A steep loose climb is not flat gravel. So difficulty is surface
// roughness AMPLIFIED BY CLIMB, and the rider sets a ceiling on it.
//
// Keep this table and kDifficulty* in step with the server-side badge in
// app/services/routing/surface_profile.py — the number that filters the route
// and the number shown on it must be the same number, or neither is
// explainable.
constexpr float kSurfaceDifficulty[] = {
    0.00f, // kPavedSmooth
    0.05f, // kPaved
    0.15f, // kPavedRough
    0.30f, // kCompacted
    0.45f, // kGravel
    0.60f, // kDirt
    0.80f, // kPath
    1.00f, // kImpassable
};
// Climb doubles the difficulty of loose ground at its worst. Note it multiplies
// the surface term rather than adding to it, so a steep TARMAC road stays easy
// — steep is not the same as difficult, and a rider who asked for "no rough
// stuff" did not ask to avoid mountain passes. That is what use_hills is for.
constexpr float kDifficultySlopeBoost = 1.0f;
constexpr int kDifficultyFullSlope = 12; // percent of climb = fully steep
constexpr uint32_t kDifficultySlopeBuckets = 32;
// Penalty per unit of difficulty above the ceiling. Steep enough to be a filter
// in practice, soft enough that the only way home is still the way home.
constexpr float kDifficultyRefusal = 20.0f;
constexpr float kDefaultMaxDifficulty = 1.0f; // neutral: nothing is too hard

constexpr Surface kMinimumMotorcycleSurface = Surface::kImpassable;

// Default turn costs
constexpr float kTCStraight = 0.5f;
constexpr float kTCSlight = 0.75f;
constexpr float kTCFavorable = 1.0f;
constexpr float kTCFavorableSharp = 1.5f;
constexpr float kTCCrossing = 2.0f;
constexpr float kTCUnfavorable = 2.5f;
constexpr float kTCUnfavorableSharp = 3.5f;
constexpr float kTCReverse = 9.5f;
constexpr float kTCRamp = 1.5f;
constexpr float kTCRoundabout = 0.5f;

// Turn costs based on side of street driving
constexpr float kRightSideTurnCosts[] = {kTCStraight,       kTCSlight,  kTCFavorable,
                                         kTCFavorableSharp, kTCReverse, kTCUnfavorableSharp,
                                         kTCUnfavorable,    kTCSlight};
constexpr float kLeftSideTurnCosts[] = {kTCStraight,         kTCSlight,  kTCUnfavorable,
                                        kTCUnfavorableSharp, kTCReverse, kTCFavorableSharp,
                                        kTCFavorable,        kTCSlight};

// Valid ranges and defaults
constexpr ranged_default_t<float> kUseHighwaysRange{0, kDefaultUseHighways, 1.0f};
constexpr ranged_default_t<float> kUseTollsRange{0, kDefaultUseTolls, 1.0f};
constexpr ranged_default_t<float> kUseTrailsRange{0, kDefaultUseTrails, 1.0f};
constexpr ranged_default_t<float> kUseHillsRange{0, kDefaultUseHills, 1.0f};
constexpr ranged_default_t<float> kUseTunnelsRange{0, kDefaultUseTunnels, 1.0f};
constexpr ranged_default_t<float> kMaxDifficultyRange{0, kDefaultMaxDifficulty, 1.0f};
constexpr ranged_default_t<uint32_t> kMotorcycleSpeedRange{10, baldr::kMaxAssumedSpeed,
                                                           baldr::kMaxSpeedKph};

constexpr float kHighwayFactor[] = {
    1.0f, // Motorway
    0.5f, // Trunk
    0.0f, // Primary
    0.0f, // Secondary
    0.0f, // Tertiary
    0.0f, // Unclassified
    0.0f, // Residential
    0.0f  // Service, other
};

constexpr float kMaxTrailBiasFactor = 8.0f;

constexpr float kSurfaceFactor[] = {
    0.0f, // kPavedSmooth
    0.0f, // kPaved
    0.0f, // kPaveRough
    0.1f, // kCompacted
    0.2f, // kDirt
    0.5f, // kGravel
    1.0f  // kPath
};

BaseCostingOptionsConfig GetBaseCostOptsConfig() {
  BaseCostingOptionsConfig cfg{};
  // override defaults
  cfg.disable_rail_ferry_ = true;
  return cfg;
}

const BaseCostingOptionsConfig kBaseCostOptsConfig = GetBaseCostOptsConfig();

} // namespace

/**
 * Derived class providing dynamic edge costing for "direct" auto routes. This
 * is a route that is generally shortest time but uses route hierarchies that
 * can result in slightly longer routes that avoid shortcuts on residential
 * roads.
 */
class MotorcycleCost : public DynamicCost {
public:
  /**
   * Construct motorcycle costing. Pass in cost type and costing_options using protocol buffer(pbf).
   * @param  costing specified costing type.
   * @param  costing_options pbf with request costing_options.
   */
  MotorcycleCost(const Costing& costing_options);

  virtual ~MotorcycleCost();

  /**
   * Does the costing method allow multiple passes (with relaxed hierarchy
   * limits).
   * @return  Returns true if the costing model allows multiple passes.
   */
  virtual bool AllowMultiPass() const override {
    return true;
  }

  /**
   * Checks if access is allowed for the provided directed edge.
   * This is generally based on mode of travel and the access modes
   * allowed on the edge. However, it can be extended to exclude access
   * based on other parameters such as conditional restrictions and
   * conditional access that can depend on time and travel mode.
   * @param  edge                        Pointer to a directed edge.
   * @param  is_dest                     Is a directed edge the destination?
   * @param  pred                        Predecessor edge information.
   * @param  tile                        Current tile.
   * @param  edgeid                      GraphId of the directed edge.
   * @param  current_time                Current time (seconds since epoch). A value of 0
   *                                     indicates the route is not time dependent.
   * @param  tz_index                    timezone index for the node
   * @param  destonly_access_restr_mask  Mask containing access restriction types that had a
   * local traffic exemption at the start of the expansion. This mask will be mutated by eliminating
   * flags for locally exempt access restriction types that no longer exist on the passed edge
   *
   * @return Returns true if access is allowed, false if not.
   */
  virtual bool Allowed(const baldr::DirectedEdge* edge,
                       const bool is_dest,
                       const EdgeLabel& pred,
                       const graph_tile_ptr& tile,
                       const baldr::GraphId& edgeid,
                       const uint64_t current_time,
                       const uint32_t tz_index,
                       uint8_t& restriction_idx,
                       uint8_t& destonly_access_restr_mask) const override;

  /**
   * Checks if access is allowed for an edge on the reverse path
   * (from destination towards origin). Both opposing edges (current and
   * predecessor) are provided. The access check is generally based on mode
   * of travel and the access modes allowed on the edge. However, it can be
   * extended to exclude access based on other parameters such as conditional
   * restrictions and conditional access that can depend on time and travel
   * mode.
   * @param  edge                        Pointer to a directed edge.
   * @param  pred                        Predecessor edge information.
   * @param  opp_edge                    Pointer to the opposing directed edge.
   * @param  tile                        Current tile.
   * @param  edgeid                      GraphId of the opposing edge.
   * @param  current_time                Current time (seconds since epoch). A value of 0
   *                                     indicates the route is not time dependent.
   * @param  tz_index                    timezone index for the node
   * @param  destonly_access_restr_mask  Mask containing access restriction types that had a
   * local traffic exemption at the start of the expansion. This mask will be mutated by eliminating
   * flags for locally exempt access restriction types that no longer exist on the passed edge
   *
   * @return  Returns true if access is allowed, false if not.
   */
  virtual bool AllowedReverse(const baldr::DirectedEdge* edge,
                              const EdgeLabel& pred,
                              const baldr::DirectedEdge* opp_edge,
                              const graph_tile_ptr& tile,
                              const baldr::GraphId& opp_edgeid,
                              const uint64_t current_time,
                              const uint32_t tz_index,
                              uint8_t& restriction_idx,
                              uint8_t& destonly_access_restr_mask) const override;

  /**
   * Only transit costings are valid for this method call, hence we throw
   * @param edge
   * @param departure
   * @param curr_time
   * @return
   */
  virtual Cost EdgeCost(const baldr::DirectedEdge*,
                        const baldr::TransitDeparture*,
                        const uint32_t) const override {
    throw std::runtime_error("MotorcycleCost::EdgeCost does not support transit edges");
  }

  /**
   * Get the cost to traverse the specified directed edge. Cost includes
   * the time (seconds) to traverse the edge.
   * @param  edge      Pointer to a directed edge.
   * @param  tile      Current tile.
   * @param  time_info Time info about edge passing.
   * @return  Returns the cost and time (seconds)
   */
  virtual Cost EdgeCost(const baldr::DirectedEdge* edge,
                        const baldr::GraphId& edgeid,
                        const graph_tile_ptr& tile,
                        const baldr::TimeInfo& time_info,
                        uint8_t& flow_sources) const override;

  /**
   * Returns the cost to make the transition from the predecessor edge.
   * Defaults to 0. Costing models that wish to include edge transition
   * costs (i.e., intersection/turn costs) must override this method.
   * @param  edge          Directed edge (the to edge)
   * @param  node          Node (intersection) where transition occurs.
   * @param  pred          Predecessor edge information.
   * @param  tile          Pointer to the graph tile containing the to edge.
   * @param  reader_getter Functor that facilitates access to a limited version of the graph reader
   * @return Returns the cost and time (seconds)
   */
  virtual Cost
  TransitionCost(const baldr::DirectedEdge* edge,
                 const baldr::NodeInfo* node,
                 const EdgeLabel& pred,
                 const graph_tile_ptr& tile,
                 const std::function<LimitedGraphReader()>& reader_getter) const override;

  /**
   * Returns the cost to make the transition from the predecessor edge
   * when using a reverse search (from destination towards the origin).
   * @param  idx                Directed edge local index
   * @param  node               Node (intersection) where transition occurs.
   * @param  pred               the opposing current edge in the reverse tree.
   * @param  edge               the opposing predecessor in the reverse tree
   * @param  tile               Graphtile that contains the node and the opp_edge
   * @param  edge_id            Graph ID of opp_pred_edge to get its tile if needed
   * @param  reader_getter      Functor that facilitates access to a limited version of the graph
   * reader
   * @param  has_measured_speed Do we have any of the measured speed types set?
   * @param  internal_turn      Did we make an turn on a short internal edge.
   * @return  Returns the cost and time (seconds)
   */
  virtual Cost TransitionCostReverse(const uint32_t idx,
                                     const baldr::NodeInfo* node,
                                     const baldr::DirectedEdge* pred,
                                     const baldr::DirectedEdge* edge,
                                     const graph_tile_ptr& tile,
                                     const GraphId& pred_id,
                                     const std::function<LimitedGraphReader()>& reader_getter,
                                     const bool has_measured_speed,
                                     const InternalTurn /*internal_turn*/) const override;

  /**
   * Get the cost factor for A* heuristics. This factor is multiplied
   * with the distance to the destination to produce an estimate of the
   * minimum cost to the destination. The A* heuristic must underestimate the
   * cost to the destination. So a time based estimate based on speed should
   * assume the maximum speed is used to the destination such that the time
   * estimate is less than the least possible time along roads.
   */
  virtual float AStarCostFactor() const override {
    // The heuristic must UNDER-estimate, so it has to know the smallest factor
    // EdgeCost can produce. Seeking hills discounts steep edges below 1.0; leave
    // this at 1.0 and A* over-estimates, prunes the mountain road it was asked
    // to find, and returns a FLATTER route the harder you ask for hills —
    // measured on Bergen→Oslo, which dropped from a 1397 m peak to 1191 m at
    // use_hills 1.0 before this line existed.
    return kSpeedFactor[top_speed_] * min_linear_cost_factor_ * hills_min_factor_;
  }

  /**
   * Get the current travel type.
   * @return  Returns the current travel type.
   */
  virtual uint8_t travel_type() const override {
    return static_cast<uint8_t>(VehicleType::kMotorcycle);
  }

  /**
   * Function to be used in location searching which will
   * exclude and allow ranking results from the search by looking at each
   * edges attribution and suitability for use as a location by the travel
   * mode used by the costing method. It's also used to filter
   * edges not usable / inaccessible by automobile.
   */
  bool Allowed(const baldr::DirectedEdge* edge,
               const graph_tile_ptr& tile,
               uint16_t disallow_mask = kDisallowNone) const override {
    bool allow_closures = (!filter_closures_ && !(disallow_mask & kDisallowClosure)) ||
                          !(flow_mask_ & kCurrentFlowMask);
    return DynamicCost::Allowed(edge, tile, disallow_mask) && !edge->bss_connection() &&
           (allow_closures || !tile->IsClosed(edge));
  }
  // Hidden in source file so we don't need it to be protected
  // We expose it within the source file for testing purposes
public:
  VehicleType type_;     // Vehicle type: car (default), motorcycle, etc
  float toll_factor_;    // Factor applied when road has a toll
  float surface_factor_; // How much the surface factors are applied when using trails
  float highway_factor_; // Factor applied when road is a motorway or trunk

  // Hills axis. hills_active_ guards the hot path so a request at the neutral
  // 0.5 — which is every request the app sends today — is bitwise identical to
  // a build without this axis, not even a table read.
  bool hills_active_ = false;
  float hills_grade_factor_[kHillsGradeBuckets];
  float hills_undulation_factor_[kHillsUndulationBuckets];
  // Smallest product the two tables can return, for the A* heuristic above.
  float hills_min_factor_ = 1.0f;

  // Tunnels axis. Neutral (0.5) is not even a branch.
  bool tunnels_active_ = false;
  float tunnel_factor_ = 1.0f;

  // Dirt difficulty ceiling. Neutral (1.0) is not even a table read.
  bool difficulty_active_ = false;
  float difficulty_factor_[8][kDifficultySlopeBuckets];

  /**
   * Soft tunnel preference. Non-tunnel edges are pinned at 1.0 in both
   * directions, so a route with no tunnel in it is bitwise unchanged.
   */
  inline float TunnelMultiplier(const baldr::DirectedEdge* edge) const {
    if (!tunnels_active_ || !edge->tunnel()) {
      return 1.0f;
    }
    return tunnel_factor_;
  }

  /**
   * Soft ceiling on how hard the ground is allowed to get, where difficulty is
   * surface roughness amplified by climb. Edges at or below the rider's ceiling
   * are untouched at exactly 1.0 — this only ever makes the too-hard ones dear,
   * never the acceptable ones cheap, so it cannot drag the whole cost landscape
   * the way a global discount would.
   */
  inline float DifficultyMultiplier(const baldr::DirectedEdge* edge) const {
    if (!difficulty_active_) {
      return 1.0f;
    }
    const int up = edge->max_up_slope();
    const uint32_t slope = up <= 0 ? 0u
                                   : (static_cast<uint32_t>(up) >= kDifficultySlopeBuckets
                                          ? kDifficultySlopeBuckets - 1
                                          : static_cast<uint32_t>(up));
    return difficulty_factor_[static_cast<uint32_t>(edge->surface())][slope];
  }

  /**
   * Multiplier for the edge's steepest slope. DISCOUNT ONLY, never above 1.0:
   * seeking hills discounts the hilly edges, avoiding them discounts the flat
   * ones. Inflating instead would distort the transition penalties, which are
   * plain seconds and do not scale with the factor — the mistake that sent a
   * dirt-first route over two ferries (see DirtFirstMultiplier).
   */
  inline float HillsMultiplier(const baldr::DirectedEdge* edge) const {
    if (!hills_active_) {
      return 1.0f;
    }
    const int total = edge->max_up_slope() - edge->max_down_slope();
    const uint32_t u = total <= 0 ? 0u
                                  : (static_cast<uint32_t>(total) >= kHillsUndulationBuckets
                                         ? kHillsUndulationBuckets - 1
                                         : static_cast<uint32_t>(total));
    return hills_grade_factor_[edge->weighted_grade()] * hills_undulation_factor_[u];
  }
};

// Constructor
MotorcycleCost::MotorcycleCost(const Costing& costing)
    : DynamicCost(costing, TravelMode::kDrive, kMotorcycleAccess) {

  const auto& costing_options = costing.options();

  // Vehicle type is motorcycle
  type_ = VehicleType::kMotorcycle;

  // Get the base costs
  get_base_costs(costing);

  // Preference to use highways. Is a value from 0 to 1
  // Factor for highway use - use a non-linear factor with values at 0.5 being neutral (factor
  // of 0). Values between 0.5 and 1 slowly decrease to a maximum of -0.125 (to slightly prefer
  // highways) while values between 0.5 to 0 slowly increase to a maximum of kMaxHighwayBiasFactor
  // to avoid/penalize highways.
  float use_highways = costing_options.use_highways();
  if (use_highways >= 0.5f) {
    float f = (0.5f - use_highways);
    highway_factor_ = f * f * f;
  } else {
    float f = 1.0f - (use_highways * 2.0f);
    highway_factor_ = kMaxHighwayBiasFactor * (f * f);
  }

  // Set toll factor based on preference to use tolls (value from 0 to 1).
  // Toll factor of 0 would indicate no adjustment to weighting for toll roads.
  // use_tolls = 1 would reduce weighting slightly (a negative delta) while
  // use_tolls = 0 would penalize (positive delta to weighting factor).
  float use_tolls = costing_options.use_tolls();
  toll_factor_ = use_tolls < 0.5f ? (2.0f - 4 * use_tolls) : // ranges from 2 to 0
                     (0.5f - use_tolls) * 0.03f;             // ranges from 0 to -0.015

  // Set the surface factor based on the use trails value - this is a
  // preference to use trails/tracks/bad surface types (a value from 0 to 1).
  float use_trails = costing_options.use_trails();

  // Factor for trail use - use a non-linear factor with values at 0.5 being neutral (factor
  // of 0). Values between 0.5 and 1 slowly decrease to a maximum of -0.125 (to slightly prefer
  // trails) while values between 0.5 to 0 slowly increase to a maximum of the surfact_factor_
  // to avoid/penalize trails.
  // modulates surface factor based on use_trails
  if (use_trails >= 0.5f) {
    float f = (0.5f - use_trails);
    surface_factor_ = f * f * f;
  } else {
    float f = 1.0f - use_trails * 2.0f;
    surface_factor_ = static_cast<uint32_t>(kMaxTrailBiasFactor * (f * f));
  }

  // Hills. Strength is the distance from the neutral 0.5, so both halves of
  // the slider reach full effect at their end and neither does anything at
  // the middle. GEOMETRIC interpolation (pow), matching the dirt-first axis:
  // linear interpolation collapses the mid-slider gradient, so "moderate"
  // ends up doing nothing at all.
  // Explicit presence check: use_hills has a `oneof has_use_hills` wrapper, so an
  // unset field reads back as the proto default 0.0 — which on this axis is FULL
  // AVOID, not neutral. Callers that build a Costing directly instead of going
  // through ParseMotorcycleCostOptions (map matching, internal costing) would
  // otherwise silently get hill-avoidance nobody asked for.
  const float use_hills =
      costing_options.has_use_hills() ? costing_options.use_hills() : kDefaultUseHills;
  const float strength = std::abs(use_hills - 0.5f) * 2.0f;
  const bool seek = use_hills > 0.5f;
  hills_active_ = strength > 0.0f;
  hills_min_factor_ = 1.0f;
  float min_grade = 1.0f, min_undulation = 1.0f;
  for (uint32_t i = 0; i < kHillsGradeBuckets; ++i) {
    // Undo the 4-bit bucketing: bucket = grade * 0.6 + 6.5.
    const float grade_pct = (static_cast<float>(i) - 6.5f) / 0.6f;
    const float hilliness = std::min(1.0f, std::abs(grade_pct) / kHillsFullGrade);
    const float full = seek ? (1.0f - kHillsGradeDiscount * hilliness)
                            : (1.0f + kHillsGradePenalty * hilliness);
    hills_grade_factor_[i] = hills_active_ ? std::pow(full, strength) : 1.0f;
    min_grade = std::min(min_grade, hills_grade_factor_[i]);
  }
  for (uint32_t i = 0; i < kHillsUndulationBuckets; ++i) {
    const float rolling =
        std::min(1.0f, static_cast<float>(i) / static_cast<float>(kHillsFullUndulation));
    const float full = seek ? (1.0f - kHillsUndulationDiscount * rolling)
                            : (1.0f + kHillsUndulationPenalty * rolling);
    hills_undulation_factor_[i] = hills_active_ ? std::pow(full, strength) : 1.0f;
    min_undulation = std::min(min_undulation, hills_undulation_factor_[i]);
  }
  hills_min_factor_ = min_grade * min_undulation;

  // Tunnels. Same shape as hills: 0.5 neutral, geometric interpolation either
  // side so a mid-slider value is not inert.
  const float use_tunnels =
      costing_options.has_use_tunnels() ? costing_options.use_tunnels() : kDefaultUseTunnels;
  const float tunnel_strength = std::abs(use_tunnels - 0.5f) * 2.0f;
  tunnels_active_ = tunnel_strength > 0.0f;
  if (tunnels_active_) {
    const float full = use_tunnels > 0.5f ? (1.0f - kTunnelMaxDiscount) : (1.0f + kTunnelMaxPenalty);
    tunnel_factor_ = std::pow(full, tunnel_strength);
  }

  // Difficulty ceiling. Defaults to 1.0, at which nothing can exceed it (the
  // score is capped at 1) — so the default is an exact no-op, guarded anyway.
  const float max_difficulty = costing_options.has_max_difficulty()
                                   ? costing_options.max_difficulty()
                                   : kDefaultMaxDifficulty;
  difficulty_active_ = max_difficulty < 1.0f;
  for (uint32_t sfc = 0; sfc < 8; ++sfc) {
    for (uint32_t i = 0; i < kDifficultySlopeBuckets; ++i) {
      const float steep =
          std::min(1.0f, static_cast<float>(i) / static_cast<float>(kDifficultyFullSlope));
      const float difficulty =
          std::min(1.0f, kSurfaceDifficulty[sfc] * (1.0f + kDifficultySlopeBoost * steep));
      const float excess = difficulty - max_difficulty;
      difficulty_factor_[sfc][i] =
          (difficulty_active_ && excess > 0.0f) ? (1.0f + kDifficultyRefusal * excess) : 1.0f;
    }
  }
}

// Destructor
MotorcycleCost::~MotorcycleCost() {
}

// Check if access is allowed on the specified edge.
bool MotorcycleCost::Allowed(const baldr::DirectedEdge* edge,
                             const bool is_dest,
                             const EdgeLabel& pred,
                             const graph_tile_ptr& tile,
                             const baldr::GraphId& edgeid,
                             const uint64_t current_time,
                             const uint32_t tz_index,
                             uint8_t& restriction_idx,
                             uint8_t& destonly_access_restr_mask) const {
  // Check access, U-turn, and simple turn restriction.
  // Allow U-turns at dead-end nodes.
  if (!IsAccessible(edge) || (!pred.deadend() && pred.opp_local_idx() == edge->localedgeidx()) ||
      ((pred.restrictions() & (1 << edge->localedgeidx())) && !ignore_turn_restrictions_) ||
      (edge->surface() > kMinimumMotorcycleSurface) || IsUserAvoidEdge(edgeid) ||
      (!allow_destination_only_ && !pred.destonly() && edge->destonly()) ||
      (pred.closure_pruning() && IsClosed(edge, tile)) || CheckExclusions<true>(edge, pred)) {
    return false;
  }

  return DynamicCost::EvaluateRestrictions(access_mask_, edge, is_dest, tile, edgeid, current_time,
                                           tz_index, restriction_idx, destonly_access_restr_mask);
}

// Checks if access is allowed for an edge on the reverse path (from
// destination towards origin). Both opposing edges are provided.
bool MotorcycleCost::AllowedReverse(const baldr::DirectedEdge* edge,
                                    const EdgeLabel& pred,
                                    const baldr::DirectedEdge* opp_edge,
                                    const graph_tile_ptr& tile,
                                    const baldr::GraphId& opp_edgeid,
                                    const uint64_t current_time,
                                    const uint32_t tz_index,
                                    uint8_t& restriction_idx,
                                    uint8_t& destonly_access_restr_mask) const {
  // Check access, U-turn, and simple turn restriction.
  // Allow U-turns at dead-end nodes.
  if (!IsAccessible(opp_edge) || (!pred.deadend() && pred.opp_local_idx() == edge->localedgeidx()) ||
      ((opp_edge->restrictions() & (1 << pred.opp_local_idx())) && !ignore_turn_restrictions_) ||
      (opp_edge->surface() > kMinimumMotorcycleSurface) || IsUserAvoidEdge(opp_edgeid) ||
      (!allow_destination_only_ && !pred.destonly() && opp_edge->destonly()) ||
      (pred.closure_pruning() && IsClosed(opp_edge, tile)) ||
      CheckExclusions<false>(opp_edge, pred)) {
    return false;
  }

  return DynamicCost::EvaluateRestrictions(access_mask_, opp_edge, false, tile, opp_edgeid,
                                           current_time, tz_index, restriction_idx,
                                           destonly_access_restr_mask);
}

Cost MotorcycleCost::EdgeCost(const baldr::DirectedEdge* edge,
                              const baldr::GraphId& edgeid,
                              const graph_tile_ptr& tile,
                              const baldr::TimeInfo& time_info,
                              uint8_t& flow_sources) const {
  auto edge_speed = fixed_speed_ == baldr::kDisableFixedSpeed
                        ? tile->GetSpeed(edge, flow_mask_, time_info.second_of_week, false,
                                         &flow_sources, time_info.seconds_from_now)
                        : fixed_speed_;

  auto final_speed = std::min(edge_speed, top_speed_);
  // Dirt-first: floor the crawling-car default speeds on unpaved surfaces.
  final_speed = DirtFirstSpeed(edge, final_speed);

  float sec = (edge->length() * kSpeedFactor[final_speed]);
  // Rider-skill speed scaler on adventure-riding edges. No-op early return
  // when the rider hasn't requested a change.
  sec /= AdventureRidingSpeedMultiplier(edge, tile);

  if (shortest_) {
    return Cost(edge->length(), sec);
  }

  // Special case for travel on a ferry
  if (edge->use() == Use::kFerry) {
    // Use the edge speed (should be the speed of the ferry). Dirt-first
    // scales ferries like smooth tarmac so they can't become a cheap
    // detour around penalized roads.
    return {sec * ferry_factor_ * DirtFirstFerryMultiplier(), sec};
  }

  float factor = kDensityFactor[edge->density()] +
                 highway_factor_ * kHighwayFactor[static_cast<uint32_t>(edge->classification())] +
                 surface_factor_ * kSurfaceFactor[static_cast<uint32_t>(edge->surface())];
  factor += SpeedPenalty(edge, tile, time_info, flow_sources, edge_speed);
  if (edge->toll()) {
    factor += toll_factor_;
  }

  if (edge->use() == Use::kTrack) {
    factor *= track_factor_;
  } else if (edge->use() == Use::kLivingStreet) {
    factor *= living_street_factor_;
  } else if (edge->use() == Use::kServiceRoad) {
    factor *= service_factor_;
  }
  if (IsClosed(edge, tile)) {
    // Add a penalty for traversing a closed edge
    factor *= closure_factor_;
  }

  factor *= EdgeFactor(edgeid);
  factor *= AdventureRidingMultiplier(edge, tile);
  factor *= DirtFirstMultiplier(edge);
  factor *= HillsMultiplier(edge);
  factor *= TunnelMultiplier(edge);
  factor *= DifficultyMultiplier(edge);

  return {sec * factor, sec};
}

// Returns the time (in seconds) to make the transition from the predecessor
Cost MotorcycleCost::TransitionCost(
    const baldr::DirectedEdge* edge,
    const baldr::NodeInfo* node,
    const EdgeLabel& pred,
    const graph_tile_ptr& /*tile*/,
    const std::function<LimitedGraphReader()>& /*reader_getter*/) const {
  // Get the transition cost for country crossing, ferry, gate, toll booth,
  // destination only, alley, maneuver penalty
  uint32_t idx = pred.opp_local_idx();
  Cost c = base_transition_cost(node, edge, &pred, idx);
  c.secs += OSRMCarTurnDuration(edge, node, idx);

  const auto stopimpact = edge->stopimpact(idx);
  const auto turntype = edge->turntype(idx);
  // Transition time = turncost * stopimpact * densityfactor
  if (stopimpact > 0 && !shortest_) {
    float turn_cost;
    if (edge->edge_to_right(idx) && edge->edge_to_left(idx)) {
      turn_cost = kTCCrossing;
    } else {
      turn_cost = (node->drive_on_right()) ? kRightSideTurnCosts[static_cast<uint32_t>(turntype)]
                                           : kLeftSideTurnCosts[static_cast<uint32_t>(turntype)];
    }

    if ((edge->use() != Use::kRamp && pred.use() == Use::kRamp) ||
        (edge->use() == Use::kRamp && pred.use() != Use::kRamp)) {
      turn_cost += kTCRamp;
      if (edge->roundabout())
        turn_cost += kTCRoundabout;
    }

    float seconds = turn_cost;
    bool has_left =
        (turntype == baldr::Turn::Type::kLeft || turntype == baldr::Turn::Type::kSharpLeft);
    bool has_right =
        (turntype == baldr::Turn::Type::kRight || turntype == baldr::Turn::Type::kSharpRight);
    bool has_reverse = turntype == baldr::Turn::Type::kReverse;

    bool is_turn = has_left || has_right || has_reverse;
    // Separate time and penalty when traffic is present. With traffic, edge speeds account for
    // much of the intersection transition time (TODO - evaluate different elapsed time settings).
    // Still want to add a penalty so routes avoid high cost intersections.
    if (is_turn) {
      seconds *= stopimpact;
    }

    AddUturnPenalty(idx, node, edge, has_reverse, has_left, has_right, false, InternalTurn::kNoTurn,
                    seconds);

    // Apply density factor and stop impact penalty if there isn't traffic on this edge or you're not
    // using traffic
    if (!pred.has_measured_speed()) {
      if (!is_turn)
        seconds *= stopimpact;
      seconds *= kTransDensityFactor[node->density()];
    }
    c.cost += seconds;
  }
  return c;
}

// Returns the cost to make the transition from the predecessor edge
// when using a reverse search (from destination towards the origin).
// pred is the opposing current edge in the reverse tree
// edge is the opposing predecessor in the reverse tree
Cost MotorcycleCost::TransitionCostReverse(
    const uint32_t idx,
    const baldr::NodeInfo* node,
    const baldr::DirectedEdge* pred,
    const baldr::DirectedEdge* edge,
    const graph_tile_ptr& /*tile*/,
    const GraphId& /*pred_id*/,
    const std::function<LimitedGraphReader()>& /*reader_getter*/,
    const bool has_measured_speed,
    const InternalTurn /*internal_turn*/) const {

  // Motorcycles should be able to make uturns on short internal edges; therefore, InternalTurn
  // is ignored for now.
  // TODO: do we want to update the cost if we have flow or speed from traffic.

  // Get the transition cost for country crossing, ferry, gate, toll booth,
  // destination only, alley, maneuver penalty
  Cost c = base_transition_cost(node, edge, pred, idx);
  c.secs += OSRMCarTurnDuration(edge, node, pred->opp_local_idx());

  const auto stopimpact = edge->stopimpact(idx);
  const auto turntype = edge->turntype(idx);
  // Transition time = turncost * stopimpact * densityfactor
  if (stopimpact > 0 && !shortest_) {
    float turn_cost;
    if (edge->edge_to_right(idx) && edge->edge_to_left(idx)) {
      turn_cost = kTCCrossing;
    } else {
      turn_cost = (node->drive_on_right()) ? kRightSideTurnCosts[static_cast<uint32_t>(turntype)]
                                           : kLeftSideTurnCosts[static_cast<uint32_t>(turntype)];
    }

    if ((edge->use() != Use::kRamp && pred->use() == Use::kRamp) ||
        (edge->use() == Use::kRamp && pred->use() != Use::kRamp)) {
      turn_cost += kTCRamp;
      if (edge->roundabout())
        turn_cost += kTCRoundabout;
    }

    float seconds = turn_cost;

    bool has_left =
        (turntype == baldr::Turn::Type::kLeft || turntype == baldr::Turn::Type::kSharpLeft);
    bool has_right =
        (turntype == baldr::Turn::Type::kRight || turntype == baldr::Turn::Type::kSharpRight);
    bool has_reverse = turntype == baldr::Turn::Type::kReverse;

    bool is_turn = has_left || has_right || has_reverse;
    // Separate time and penalty when traffic is present. With traffic, edge speeds account for
    // much of the intersection transition time (TODO - evaluate different elapsed time settings).
    // Still want to add a penalty so routes avoid high cost intersections.
    if (is_turn) {
      seconds *= stopimpact;
    }

    AddUturnPenalty(idx, node, edge, has_reverse, has_left, has_right, false, InternalTurn::kNoTurn,
                    seconds);

    // Apply density factor and stop impact penalty if there isn't traffic on this edge or you're not
    // using traffic
    if (!has_measured_speed) {
      if (!is_turn)
        seconds *= stopimpact;
      seconds *= kTransDensityFactor[node->density()];
    }
    c.cost += seconds;
  }
  return c;
}

void ParseMotorcycleCostOptions(const rapidjson::Document& doc,
                                const std::string& costing_options_key,
                                Costing* c,
                                google::protobuf::RepeatedPtrField<CodedDescription>& warnings) {
  c->set_type(Costing::motorcycle);
  c->set_name(Costing_Enum_Name(c->type()));
  auto* co = c->mutable_options();

  rapidjson::Value dummy;
  const auto& json = rapidjson::get_child(doc, costing_options_key.c_str(), dummy);

  ParseBaseCostOptions(json, c, kBaseCostOptsConfig, warnings);
  JSON_PBF_RANGED_DEFAULT(co, kUseHighwaysRange, json, "/use_highways", use_highways, warnings);
  JSON_PBF_RANGED_DEFAULT(co, kUseTollsRange, json, "/use_tolls", use_tolls, warnings);
  JSON_PBF_RANGED_DEFAULT(co, kUseTrailsRange, json, "/use_trails", use_trails, warnings);
  JSON_PBF_RANGED_DEFAULT(co, kUseHillsRange, json, "/use_hills", use_hills, warnings);
  JSON_PBF_RANGED_DEFAULT(co, kUseTunnelsRange, json, "/use_tunnels", use_tunnels, warnings);
  JSON_PBF_RANGED_DEFAULT(co, kMaxDifficultyRange, json, "/max_difficulty", max_difficulty,
                          warnings);
  JSON_PBF_RANGED_DEFAULT(co, kMotorcycleSpeedRange, json, "/top_speed", top_speed, warnings);
}

cost_ptr_t CreateMotorcycleCost(const Costing& costing_options) {
  return std::make_shared<MotorcycleCost>(costing_options);
}

} // namespace sif
} // namespace valhalla

/**********************************************************************************************/

#ifdef INLINE_TEST

using namespace valhalla;
using namespace sif;

namespace {

class TestMotorcycleCost : public MotorcycleCost {
public:
  TestMotorcycleCost(const Costing& costing_options) : MotorcycleCost(costing_options){};

  using MotorcycleCost::alley_penalty_;
  using MotorcycleCost::country_crossing_cost_;
  using MotorcycleCost::destination_only_penalty_;
  using MotorcycleCost::ferry_transition_cost_;
  using MotorcycleCost::gate_cost_;
  using MotorcycleCost::maneuver_penalty_;
  using MotorcycleCost::service_factor_;
  using MotorcycleCost::service_penalty_;
  using MotorcycleCost::toll_booth_cost_;
};

TestMotorcycleCost* make_motorcyclecost_from_json(const std::string& property, float testVal) {
  std::stringstream ss;
  ss << R"({"costing": "motorcycle", "costing_options":{"motorcycle":{")" << property << R"(":)"
     << testVal << "}}}";
  Api request;
  ParseApi(ss.str(), valhalla::Options::route, request);
  return new TestMotorcycleCost(request.options().costings().find(Costing::motorcycle)->second);
}

template <typename T>
std::uniform_real_distribution<T>* make_distributor_from_range(const ranged_default_t<T>& range) {
  T rangeLength = range.max - range.min;
  return new std::uniform_real_distribution<T>(range.min - rangeLength, range.max + rangeLength);
}

TEST(MotorcycleCost, testMotorcycleCostParams) {
  constexpr unsigned testIterations = 250;
  constexpr unsigned seed = 0;
  std::mt19937 generator(seed);
  std::shared_ptr<std::uniform_real_distribution<float>> distributor;
  std::shared_ptr<TestMotorcycleCost> ctorTester;

  const auto& defaults = kBaseCostOptsConfig;

  // maneuver_penalty_
  distributor.reset(make_distributor_from_range(defaults.maneuver_penalty_));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(make_motorcyclecost_from_json("maneuver_penalty", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->maneuver_penalty_,
                test::IsBetween(defaults.maneuver_penalty_.min, defaults.maneuver_penalty_.max));
  }

  // alley_penalty_
  distributor.reset(make_distributor_from_range(defaults.alley_penalty_));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(make_motorcyclecost_from_json("alley_penalty", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->alley_penalty_,
                test::IsBetween(defaults.alley_penalty_.min, defaults.alley_penalty_.max));
  }

  // destination_only_penalty_
  distributor.reset(make_distributor_from_range(defaults.dest_only_penalty_));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(
        make_motorcyclecost_from_json("destination_only_penalty", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->destination_only_penalty_,
                test::IsBetween(defaults.dest_only_penalty_.min, defaults.dest_only_penalty_.max));
  }

  // gate_cost_ (Cost.secs)
  distributor.reset(make_distributor_from_range(defaults.gate_cost_));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(make_motorcyclecost_from_json("gate_cost", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->gate_cost_.secs,
                test::IsBetween(defaults.gate_cost_.min, defaults.gate_cost_.max));
  }

  // gate_penalty_ (Cost.cost)
  distributor.reset(make_distributor_from_range(defaults.gate_penalty_));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(make_motorcyclecost_from_json("gate_penalty", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->gate_cost_.cost,
                test::IsBetween(defaults.gate_penalty_.min, defaults.gate_penalty_.max));
  }

  // toll_booth_cost_ (Cost.secs)
  distributor.reset(make_distributor_from_range(defaults.toll_booth_cost_));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(make_motorcyclecost_from_json("toll_booth_cost", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->toll_booth_cost_.secs,
                test::IsBetween(defaults.toll_booth_cost_.min, defaults.toll_booth_cost_.max));
  }

  // tollbooth_penalty_ (Cost.cost)
  distributor.reset(make_distributor_from_range(defaults.toll_booth_penalty_));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(make_motorcyclecost_from_json("toll_booth_penalty", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->toll_booth_cost_.cost,
                test::IsBetween(defaults.toll_booth_penalty_.min,
                                defaults.toll_booth_penalty_.max + defaults.toll_booth_cost_.def));
  }

  // country_crossing_cost_ (Cost.secs)
  distributor.reset(make_distributor_from_range(defaults.country_crossing_cost_));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(
        make_motorcyclecost_from_json("country_crossing_cost", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->country_crossing_cost_.secs,
                test::IsBetween(defaults.country_crossing_cost_.min,
                                defaults.country_crossing_cost_.max));
  }

  // country_crossing_penalty_ (Cost.cost)
  distributor.reset(make_distributor_from_range(defaults.country_crossing_penalty_));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(
        make_motorcyclecost_from_json("country_crossing_penalty", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->country_crossing_cost_.cost,
                test::IsBetween(defaults.country_crossing_penalty_.min,
                                defaults.country_crossing_penalty_.max +
                                    defaults.country_crossing_cost_.def));
  }

  // ferry_cost_ (Cost.secs)
  distributor.reset(make_distributor_from_range(defaults.ferry_cost_));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(make_motorcyclecost_from_json("ferry_cost", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->ferry_transition_cost_.secs,
                test::IsBetween(defaults.ferry_cost_.min, defaults.ferry_cost_.max));
  }

  // service_penalty_
  distributor.reset(make_distributor_from_range(defaults.service_penalty_));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(make_motorcyclecost_from_json("service_penalty", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->service_penalty_,
                test::IsBetween(defaults.service_penalty_.min, defaults.service_penalty_.max));
  }

  // service_factor_
  distributor.reset(make_distributor_from_range(defaults.service_factor_));
  for (unsigned i = 0; i < testIterations; ++i) {
    ctorTester.reset(make_motorcyclecost_from_json("service_factor", (*distributor)(generator)));
    EXPECT_THAT(ctorTester->service_factor_,
                test::IsBetween(defaults.service_factor_.min, defaults.service_factor_.max));
  }

  /*
   // use_ferry
   distributor.reset(make_distributor_from_range(defaults.use_ferry_));
   for (unsigned i = 0; i < testIterations; ++i) {
     ctorTester.reset(make_motorcyclecost_from_json("use_ferry", (*distributor)(generator)));
EXPECT_THAT(ctorTester->use_ferry , test::IsBetween(defaults.use_ferry_.min,
defaults.use_ferry_.max));
   }

    // use_highways
    distributor.reset(make_distributor_from_range(kUseHighwaysRange));
    for (unsigned i = 0; i < testIterations; ++i) {
      ctorTester.reset(make_motorcyclecost_from_json("use_highways", (*distributor)(generator)));
EXPECT_THAT(ctorTester->use_highways , test::IsBetween(kUseHighwaysRange.min, kUseHighwaysRange.max));
    }

     // use_trails
     distributor.reset(make_distributor_from_range(kUseTrailsRange));
     for (unsigned i = 0; i < testIterations; ++i) {
       ctorTester.reset(make_motorcyclecost_from_json("use_trails", (*distributor)(generator)));
EXPECT_THAT(ctorTester->use_trails , test::IsBetween(kUseTrailsRange.min, kUseTrailsRange.max));
     }

   // use_tolls
   distributor.reset(make_distributor_from_range(kUseTollsRange));
   for (unsigned i = 0; i < testIterations; ++i) {
     ctorTester.reset(make_motorcyclecost_from_json("use_tolls", (*distributor)(generator)));
EXPECT_THAT(ctorTester->use_tolls , test::IsBetween(kUseTollsRange.min, kUseTollsRange.max));
   }
   **/
}
} // namespace

#endif
