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

Out-of-range values snap to the **default (off)** per `ranged_default_t` —
not to the nearest bound.

## The speed floor (motor profiles)

Cost alone cannot make tracks routable: untagged tracks store a **5 km/h
default halved to 2 km/h** by the lua's unpaved rule (measured on the live
EU tiles — real untagged Norwegian tracks carry 2 km/h). That is a car
crawling, not a dirt bike: a 20× time handicap no sane cost table
overcomes, and every track ETA is pathological (10 km = 5 h).

So when dirt-first is active, motor profiles (motorcycle, auto) floor the
edge speed on unpaved surfaces via `DirtFirstSpeed()`:

| Surface | full-strength floor |
|---|---|
| compacted / dirt / gravel | 30 km/h |
| path | 15 km/h |
| paved* / impassable | none |

The floor scales with strength (`round(d × full)`), never *lowers* a speed
(tagged-fast edges keep theirs), and affects both routing and the reported
ETA — which is the honest number for a dirt bike. 30 km/h matches the
adventure-riding consensus pace for forestry track (the TET augmentation
uses `maxspeed=40` for curated trails). Bicycle and pedestrian profiles do
NOT get the floor: their unpaved slowdowns are real.

The gradient this buys at intermediate strengths: ~0.5 takes good gravel
roads and mild tracks; strength → 1 treats slow tracks as route material
outright.

## Presets (calibrated on NO+SE fixtures, 2026-08-24)

Measured with the fork service against the live EU tileset (motorcycle,
`use_tracks=1`, `use_trails=1`; %unpaved via trace_attributes on the
returned shape):

| fixture | 0 | 0.3 | 0.5 | 0.7 | 1.0 |
|---|---|---|---|---|---|
| SE Torsby→Sysslebäck %unpaved | 0 | 14.7 | 39.7 | 56.2 | 76.8 |
| NO Hedalen (TET-N-01 area) %unpaved | 15.9 | 37.9 | 37.9 | 37.9 | 83.3 |
| NO Lillehammer→Beitostølen %unpaved | 0 | 0 | 0 | 0 | 0 |
| SE Falun→Rättvik %unpaved | 0 | 0 | 0 | 2.8 | 2.8 |

Lillehammer→Beitostølen is the honest negative control: a mountain crossing
with no continuous unpaved alternative — dirt-first doesn't invent dirt.

Recommended rider presets (server + mobile). Every preset ships the
companions `use_tracks=1` and (motorcycle) `use_trails=1` — without them
stock track-avoidance (auto: 300 s track_penalty + track_factor;
motorcycle: surface_factor) suppresses the axis:

| preset | use_dirt_first | use_ferry | behaviour |
|---|---|---|---|
| Off | unset | caller's | stock routing |
| Light | 0.35 | 0.5 | gravel when it's roughly on the way |
| Moderate | 0.6 | 0.3 | seeks gravel corridors, accepts real detours |
| Strong | 1.0 | 0.2 | tarmac connector-only; slow tracks are route material |

`use_ferry` joined the companion set after the Bergen→Göteborg field report
(2026-08-24): the engine scales ferry edges like smooth tarmac
(`DirtFirstFerryMultiplier` — without it the ferry early-return in
motor/bicycle/pedestrian EdgeCost made long crossings free connectors), but
a high caller `use_ferry` (the app's fjord-touring 0.8) still keeps
ferry-dominant corridors on the boats. Lowering it with strength sends the
route overland; ferries remain costed, never blocked.

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
