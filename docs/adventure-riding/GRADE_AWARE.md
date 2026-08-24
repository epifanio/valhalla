# Grade-aware routing

Three motorcycle costing knobs that became possible once the tiles carried
elevation. All three are **neutral at their default and opt-in**: a request
that does not set them reaches the same route, byte for byte, as a build
without them. That is the property to preserve if you touch this code.

Companion to [DIRT_FIRST.md](DIRT_FIRST.md). Requires `additional_data.elevation`
at tile-build time (or a `valhalla_add_elevation` pass — see below).

## The knobs

| option | neutral | below neutral | above neutral |
|---|---|---|---|
| `use_hills` | **0.5** | avoid gradient | seek gradient |
| `use_tunnels` | **0.5** | prefer daylight | prefer the tunnel |
| `max_difficulty` | **1.0** | ceiling on rough ground | — |

### `use_hills`

A motorcycle is not a bicycle. Bicycle costing reads this field one-sidedly —
0 avoids climbs, 1 merely stops avoiding them — because nobody pedals uphill
for fun. A rider does: the pass IS the destination. So 0.5 is the neutral
midpoint here and both halves are live.

Two terms, and they are **not** interchangeable:

- **sustained gradient** (`|weighted_grade|`) is what decides whether the route
  goes over the pass. Cost is proportional to length, so scaling it by
  `|grade|` is a discount *per metre climbed*, which accumulates the way ascent
  does.
- **undulation** (`max_up_slope + |max_down_slope|`) is the hairpins, and is
  deliberately the smaller term. Built with undulation alone, `use_hills 1.0`
  sent Bergen→Oslo 30 km further for **40 m less** peak altitude: plenty of
  steep little ramps, no mountain.

Measured on the live EU graph by peak altitude and cumulative ascent sampled
against the DEM — kilometres do not tell you whether a route went over
anything:

```
                      neutral            avoid 0.0          seek 1.0
Trento->Belluno    144 km / 2026 m    114 km /  476 m    142 km / 1911 m
Innsbruck->Bolzano        2194 m             1376 m            2194 m
Bergen->Oslo         7025 m climb       4836 m climb      6229 m climb
Chur->Lugano         2340 m climb       2250 m climb      3146 m climb
Grimsel / Cenis / Sognefjell — one road only, correctly unchanged
Warsaw->Krakow (flat control)          — unchanged
```

*Avoid* is consistent everywhere and correctly inert where there is only one
road. *Seek* wins where there is a low-level bypass to reject (Chur→Lugano
takes the pass instead of the tunnel, +34% ascent) and is close to a no-op on
most alpine pairs **because the motorcycle profile already takes the pass at
neutral**. On a long route it can trade altitude for distance: a per-edge
multiplier expresses road CHARACTER; it cannot maximise a route-level quantity
like total ascent. Seeking "the mountain" as such wants a scored road.

### `use_tunnels`

"Skip the tunnel on a sunny day" is a **tunnel** preference, not an elevation
one — and it is far better posed than seeking altitude, because it is a
property of the single edge in front of you.

Valhalla already has `exclude_tunnels`, but it is a hard filter in `Allowed()`:
it can leave Gotthard, Mont Blanc or an urban underpass unroutable and fail the
request outright. This is the soft version — prefer daylight when there is a way
round, take the tunnel when there is not. Tunnel edges are rare, so penalising
them is a targeted inflation, not the global one that flattening a whole country
would be. Non-tunnel edges are pinned at 1.0.

```
Goschenen->Airolo, highway-friendly rider (use_highways 1.0):
  neutral 0.5    21.96 km   tunnel 16.93 km   <- the Gotthard tube
  avoid   0.25   30.76 km   tunnel  0.10 km   <- the Gotthard PASS
Chur->Bellinzona:
  neutral 0.5   116.17 km   tunnel 20.36 km
  avoid   0.0   123.62 km   tunnel  4.05 km
```

Note the default scenic profile (`use_highways 0.2`) **already** declines
motorway tunnels — Göschenen→Airolo takes the pass at neutral. This knob is for
the tunnels that remain: the long non-motorway ones where the tunnel is the main
road, above all in Norway.

### `max_difficulty`

The question behind the whole elevation programme: does the dirt score say how
**demanding** the riding is? A steep loose climb is not flat gravel.

```
difficulty = surface_roughness × (1 + climb_boost × min(1, max_up_slope / 12%))
```

The climb term **multiplies** the surface term rather than adding to it, so
steep TARMAC stays easy. Steep is not the same as difficult, and a rider who
asked for no rough stuff did not ask to avoid mountain passes — that is what
`use_hills` is for.

A **soft** ceiling: edges above it are penalised steeply rather than excluded,
so a request never becomes unroutable because the only way home is a rough
kilometre. It is a preference, not a guarantee — a route can still contain a
short stretch above the ceiling where there is no alternative.

```
dirt-first 0.8            ceiling 1.0 (neutral)      ceiling 0.3
                        km   mean  hard  %unpaved   km   mean  hard  %unpaved
Torsby (SE)         119.09   0.48  68.0     79.1   89.53  0.10  10.9    18.4
Rena->Koppang        69.02   0.20  15.1     39.0   58.55  0.00   0.0     0.0
Hedalen              32.99   0.27   9.8     37.4   33.01  0.12   5.1    17.0
```

⚠️ **`kSurfaceDifficulty` and `kDifficulty*` are duplicated** in
`app/services/routing/surface_profile.py` (FastGIS), which puts the same number
on the route the rider gets back. They MUST stay in step: if the number that
filtered the route and the number displayed on it disagree, neither is
explainable — "it says 0.7, I asked for 0.6, and it routed me there anyway".

And the badge is a **second** number beside "% unpaved", never folded into it.
"% unpaved" is a pure length ratio and stays one.

## Two traps, both paid for in this branch

1. **Pin the common case at 1.0.** The first cut of `use_hills` implemented
   "avoid" by *discounting flat edges* — the same preference on paper. But in a
   flat country that is a near-global discount, and Valhalla's transition
   penalties are plain seconds that do not scale with it. Warsaw→Kraków
   collapsed from 367 km to 285 km, the router buying motorway to dodge turns
   that had become relatively twice as dear. Nothing to do with hills. Discount
   or penalise the *rare* class; never move the baseline.

2. **`AStarCostFactor` must know the smallest factor `EdgeCost` can produce.**
   The heuristic has to under-estimate. Leave it at 1.0 while discounting edges
   below 1.0 and A\* over-estimates, prunes the mountain road it was asked for,
   and returns a *flatter* route the harder you ask for hills — Bergen→Oslo fell
   from a 1397 m peak to 1191 m at `use_hills 1.0`.
   **The dirt-first axis discounts well below 1.0 and does not adjust this
   either.** Unfixed: fixing it would move dirt-first routes, which have been
   field-verified.

## Getting elevation into the tiles

`valhalla_add_elevation` rewrites existing tiles in place — no rebuild from
PBF. But it is **broken upstream on a finished graph**; see the fix in
`src/mjolnir/elevationbuilder.cc` on this branch, and never run a stock binary
against a built tileset. skadi reads only square 3601² SRTM `.hgt`; the FastGIS
repo has the GLO-30 conversion pipeline in `scripts/terrain/`.
