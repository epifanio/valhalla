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
| paved_smooth | 1.0 | **stock — never inflated** |
| paved | 1.0 | |
| paved_rough | 0.7 | cobbles have character; mild preference |
| compacted | 0.05 | maintained gravel — prime material |
| dirt | 0.05 | **includes every untagged `highway=track`** |
| gravel | 0.06 | |
| path | 0.15 | rideable but rough; sought less than a good track |
| impassable | 1.0 | neutral — never sought |

⚠️ **Never put a value above 1.0 in this table.** Penalizing paved instead of
discounting dirt looks equivalent — same dirt:paved ratio — but is not.
Valhalla adds transition costs (maneuver, gate, toll booth, country crossing)
in *seconds*, unscaled by the surface factor. An earlier table used paved 6.0
/ dirt 0.3, which made those penalties 6× cheaper in relative terms and
distorted every non-surface decision: Bergen→Göteborg at strength 1.0 came
back **shorter and faster than stock** (810 km / 11.8 h vs 967 km / 14.0 h) —
a twistier *tarmac* path bought with maneuver penalties that no longer
mattered. Pinning paved at 1.0 leaves the whole paved network behaving
exactly as stock, so the axis only ever moves the dirt-vs-paved decision.

Interpolation is **geometric**: `pow(full, strength)`. Linear interpolation
collapses the mid-strength gradient once paved is pinned at 1.0 — at
strength 0.6 dirt would land on 0.43, which against the ~4× time handicap of
a 30 km/h track vs an 80 km/h road leaves dirt *more* expensive than tarmac,
i.e. "Moderate" would do nothing. `pow()` keeps each step a constant fraction
of the full discount and still gives an exact no-op at strength 0.

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

The floor does NOT scale with strength — it corrects bogus *data*, not a
preference (a rider on the mildest setting still isn't doing 2 km/h on a
forestry track, and their ETA should say so). It never *lowers* a speed
(tagged-fast edges keep theirs), and affects both routing and the reported
ETA — which is the honest number for a dirt bike. 30 km/h matches the
adventure-riding consensus pace for forestry track (the TET augmentation
uses `maxspeed=40` for curated trails). Bicycle and pedestrian profiles do
NOT get the floor: their unpaved slowdowns are real.

The gradient this buys at intermediate strengths: ~0.5 takes good gravel
roads and mild tracks; strength → 1 treats slow tracks as route material
outright.

## Presets (re-calibrated on the geometric curve, 2026-08-24)

% unpaved of the returned route, motorcycle with the companions below,
measured against the live EU tileset:

| fixture | 0 | 0.2 | 0.35 | 0.5 | 0.6 | 0.8 | 1.0 |
|---|---|---|---|---|---|---|---|
| SE Torsby→Sysslebäck | 8.9 | 18.4 | 18.4 | 66.6 | **82.6** | 79.1 | 70.8 |
| NO Hedalen local | 10.3 | 30.4 | 37.4 | 37.1 | 37.1 | 37.4 | 37.4 |
| SE Falun→Rättvik | 0 | 0 | 0 | 3.4 | 47.6 | 50.9 | **60.4** |
| SE Karlstad→Falun | 5.9 | 9.5 | 24.7 | **42.0** | 36.0 | 39.6 | 39.6 |
| NO Lillehammer→Beitostølen | 23.0 | 23.0 | 23.0 | 23.0 | 23.0 | 23.0 | 23.0 |

Lillehammer→Beitostølen is the honest negative control: flat at every
strength, because that mountain crossing has no unpaved *alternative* —
dirt-first doesn't invent dirt. The curve is not strictly monotonic (a
bigger discount can select a different corridor whose connectors are paved);
0.5–0.6 is where most fixtures make their jump.

| preset | use_dirt_first | behaviour |
|---|---|---|
| Off | unset | stock routing |
| Light | 0.35 | gravel when it's roughly on the way |
| Moderate | 0.6 | seeks gravel corridors, accepts real detours |
| Strong | 1.0 | tarmac connector-only; slow tracks are route material |

Companions every active preset must send — without them stock
track-avoidance (auto: 300 s `track_penalty` + `track_factor`; motorcycle:
`surface_factor`) suppresses the axis:
`use_tracks=1`, `use_trails=1` (motorcycle), `avoid_bad_surfaces=0`.

**`use_ferry` is deliberately NOT a companion.** A preset briefly forced it
down after a Bergen→Göteborg field report came back as a ferry chain.
Measurement killed the idea: the override changes nothing where dirt-first
works (Torsby returns 82.6% unpaved at `use_ferry` 0.8, 0.5 and 0.3 alike)
and is the *only* thing that moves a route where dirt-first is inert
(Bergen→Göteborg is 0.0% unpaved at every strength). It was never biasing
surface — it was silently overriding the rider's own ferry preference on
transit routes. The loophole that prompted it is fixed structurally instead
(paved pinned at 1.0 ⇒ ferries cost exactly what stock costs them).

## How far dirt-first reaches

Achievable unpaved share decays with trip length, because unpaved networks
are *local* — there is no continuous gravel corridor across a continent.
Measured at Strong:

| route | length | unpaved |
|---|---|---|
| Torsby→Sysslebäck | 117 km | 70.8% |
| Karlstad→Falun | 282 km | 39.6% |
| Oslo→Trondheim | 638 km | 12.2% |
| Bergen→Göteborg | 974 km | ~1% |

This is a property of the road network, not a tuning failure:
`disable_hierarchy_pruning=true` changes **nothing** (verified — identical
routes and shares at every distance above), so it is not an artifact of
Valhalla's hierarchical A* skipping local edges. Dirt-first is a ride-scale
feature; on a long transit it correctly reports that no unpaved corridor
exists rather than inventing one. Clients should say so — FastGIS shows
"No unpaved roads on this route" instead of a bare 0%.

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
