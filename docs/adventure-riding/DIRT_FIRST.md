# `use_dirt_first` — dirt-first routing axis

A fourth per-profile costing option on `feat/adventure-riding`, alongside the
three adventure-riding options: route A→B **on unpaved surfaces**, using
tarmac only as a connector.

```jsonc
{
  "costing": "motorcycle",
  "costing_options": {
    "motorcycle": {
      "use_dirt_first": 0.8,   // 0 = off (default) … 1 = full strength
      "use_trails": 1.0        // motorcycle only — see "Composition" below
    }
  }
}
```

## How it differs from `use_adventure_riding`

| | adventure-riding axis | dirt-first axis |
|---|---|---|
| Keys on | `TaggedValue::kAdventureRiding` in EdgeInfo (needs augmented tiles) | `DirectedEdge::surface()` (in **every** tile) |
| Tile rebuild | required (`source=TET` pipeline) | **none — works on stock tiles** |
| Hot path | guarded EdgeInfo fetch + tag scan | guarded 8-entry array lookup |
| Semantics | prefer a *curated network* | prefer a *surface type* |

Both default to no-op and cost stock requests nothing: the hot-path guard
(`dirt_first_active_`) means a request without the option never even reads
the table.

## Per-Surface multiplier table

Full-strength values (`kDirtFirstFullFactor` in `src/sif/dynamiccost.cc`);
at strength `d` each multiplier interpolates `1 + d * (full − 1)`:

| Surface | full-strength × | note |
|---|---|---|
| paved_smooth | 6.0 | tarmac = connector only |
| paved | 6.0 | |
| paved_rough | 4.0 | cobbles: still pavement |
| compacted | 0.3 | maintained gravel — prime material |
| dirt | 0.3 | **includes every untagged `highway=track`** |
| gravel | 0.35 | |
| path | 0.8 | rideable but not sought |
| impassable | 1.0 | `Allowed()`'s problem |

The 20× paved/dirt spread at full strength is deliberate: cost is
per-*second*, and untagged tracks carry a **5 km/h default speed**
(`lua/graph.lua`) — a ~10× time handicap against a 50 km/h connector road
that a 3×/0.4× spread can never overcome. The interpolation then gives a
useful gradient: at strength ~0.5 the route takes good gravel roads but
still skips 5 km/h tracks; only strength → 1 treats slow tracks as route
material outright. Out-of-range values snap to the **default (off)** per
`ranged_default_t` — not to the nearest bound.

## Why Surface is a safe key (Phase-0 ground truth, 2026-08-24)

Probed the live EU tileset (`/locate` verbose) against OSM ground truth
sampled from the Norway + Sweden PBFs (the same UNPAVED_SURFACES semantics
as FastGIS's unpaved-lines pmtiles):

| category | Norway | Sweden |
|---|---|---|
| tagged-unpaved ways → unpaved Surface bucket | 100% (80/80) | 100% (60/60) |
| untagged `highway=track` → dirt bucket | 98.7% | 100% (60/60, all `dirt`) |
| tagged-asphalt control → paved bucket | 98.8% | — |

The untagged-track default comes from `src/mjolnir/pbfgraphparser.cc`
(`Use::kTrack → Surface::kDirt`). The known false-paved class is untagged
`unclassified`/`residential` (default `paved_smooth`) that is actually
gravel — accepted for v1: those roads still work as connectors, they are
just not *sought*.

## Composition rules

- **motorcycle**: default `use_trails=0` adds `surface_factor_ *
  kSurfaceFactor[surface]` (up to +8×0.5 on gravel) which can outweigh the
  dirt-first discount. Pair `use_dirt_first` with `use_trails=1`. The server
  presets do this automatically.
- **auto**: no surface penalty; the axis works alone.
- `use_dirt_first` composes multiplicatively with the adventure-riding axis:
  a TET trail that is also gravel gets both discounts — which is the right
  answer for the "ride the TET, dirt-first" use case.

## Implementation

Same shared-DynamicCost shape as the AR axis:

- proto field **100** (`oneof has_use_dirt_first`), range [0,1] via
  `ranged_default_t`, default 0
- `DynamicCost::set_use_dirt_first()` collapses strength into the
  per-Surface table once at request-parse time
- `DirtFirstMultiplier(edge)` inline in `valhalla/sif/dynamiccost.h`;
  call site `factor *= DirtFirstMultiplier(edge);` in the four profiles'
  `EdgeCost()` (auto / motorcycle / bicycle / pedestrian), right after
  `AdventureRidingMultiplier`
- tests: `test/gurka/test_dirt_first.cc` (default-stays-paved, full-strength
  flip, zero==stock bitwise, untagged-track flip, out-of-range clamp)
