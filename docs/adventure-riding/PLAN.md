# `feat/adventure-riding` — implementation plan

A fork of Valhalla that natively understands **curated adventure-riding networks** (TET, BDR, EuroVelo, …) as a routing-graph attribute. Costing models can be told to prefer (or avoid) such edges via a per-profile `use_adventure_riding` option, without any tile-format bit-field changes — using the existing `EdgeInfo::TaggedValue` extension point that the project's `CLAUDE.md` blesses for new per-edge data.

This document lives in the fork so the design and progress are versioned alongside the code. It is the single source of truth for what's done and what's next.

---

## Why a fork, not a service-side wrapper

Three reasons:

1. **iOS distribution.** The user's `valhalla-mobile` library (Swift/Kotlin wrappers around upstream Valhalla, vendored as a submodule at commit `72f459fc5`) ships a Valhalla binary inside an offline mobile app. A service-side corridor algorithm doesn't reach iOS; a fork does, because the iOS binary compiles from the same source tree.
2. **Per-profile costing is the natural fit.** Adventure-bike riders want trails *as part of the cost calculation*, not as post-hoc waypoint stitching. Valhalla's existing `use_living_streets`, `use_tracks` etc. all live in `DynamicCost`; `use_adventure_riding` is the same shape.
3. **Stock-tile fall-through.** Because the data is carried as a `TaggedValue`, stock-Valhalla tiles without the tag route as before. The fork is forward-compatible.

The user-facing model: **one fork (`epifanio/valhalla`), three consumers** — the server's tile-build Docker image, the iOS Swift package, the Android Kotlin lib.

---

## Constraints we are honouring

From the upstream project's `CLAUDE.md`:

> **Tile format is frozen.** … Extending `DirectedEdge` or `NodeInfo` is not an option — there are only 4 spare bits in `DirectedEdge` and 1 in `NodeInfo`. **The preferred way to add new per-edge data is via `TaggedValue` entries in `EdgeInfo`'s variable-length name/tag list.**

> **Every addition is multiplied at planet scale.** … Adding a `TaggedValue` to `EdgeInfo` is safe for the format but still grows every affected tile.

> **Costing functions are the hottest path.** `EdgeCost()`, `TransitionCost()`, `Allowed()` in `src/sif/` are called millions of times per request.

Implications we accept:

- No `DirectedEdge` / `NodeInfo` schema changes. All new per-edge data goes through `EdgeInfo::TaggedValue`.
- Only edges that are *actually* on an adventure-riding network pay storage. Empty by default.
- Costing read path must be branchless / cheap on the hot path — likely a single tag lookup gated on a bool, then a multiply.

---

## Schema

### `valhalla/baldr/graphconstants.h`

```cpp
enum class TaggedValue : uint8_t {
  // …existing 1..9 + ASCII '1','2'…
  kAdventureRiding = 10,    // NEW
};

enum class AdventureRidingClass : uint8_t {
  kNone     = 0,
  kTet      = 1,
  kEuroVelo = 2,
  kBdr      = 3,
  // up to 255 classes; reserve gaps for future networks
};
```

The tag's payload is a single byte holding an `AdventureRidingClass`. One tag entry per edge per network. An edge that's part of multiple networks gets multiple tag entries (TaggedValue is a `multimap<TaggedValue,std::string>`).

### `proto/descriptors/options.proto`

```protobuf
// Inside Costing.Options:
oneof has_use_adventure_riding {
  float use_adventure_riding = 97;  // multiplier 0..1; 1.0 neutral, <1 prefer
}
```

The `oneof` wrapping is so callers can distinguish "not set" (per-profile default applies) from "set to 0.0" (intentional strong preference).

---

## 6-layer implementation plan

Each layer is its own commit. Layers 1–3 are the scaffolding (this checkpoint). Layers 4–6 are the real work.

### ✅ Layer 1 — Constants (this commit)

Add `TaggedValue::kAdventureRiding` and `AdventureRidingClass` enum in `valhalla/baldr/graphconstants.h`. Pure additive; no behaviour change.

### ✅ Layer 2 — Request schema (this commit)

Add `use_adventure_riding` field to `Costing.Options` in `proto/descriptors/options.proto` (field number **97**, next free after `multimodal_start_end_max_distance = 96`).

### ✅ Layer 3 — Option parsing skeleton (this commit)

In `valhalla/sif/dynamiccost.h` + `src/sif/dynamiccost.cc`:
- Add `ranged_default_t<float> use_adventure_riding_` member.
- Initialise with `{0.f, kDefaultUseAdventureRiding, 1.f}` (default 1.0 = neutral).
- Wire `JSON_PBF_RANGED_DEFAULT(..., use_adventure_riding, ...)` in `ParseSharedOptions`.
- **No `EdgeCost()` effect yet.** This commit only opens the API surface.

### ⏳ Layer 4 — Tile-build write path (next commit)

The parser needs to recognise OSM `source=<network>` tags on ways and persist them through to the EdgeInfo tag list:

- **`lua/graph.lua`** — when `source=TET` (or any value from an allow-list configured per build), emit a new `kv["adventure_riding_class"] = <int>` value. Currently `lua/graph.lua` already emits 60+ `kv[]` fields; this is the same shape.
- **`src/mjolnir/pbfgraphparser.cc`** — add a `tag_handlers_["adventure_riding_class"]` that stashes the class id on the `OSMWay`.
- **`src/mjolnir/osmway.{h,cc}`** — add an `adventure_riding_class_` field (uint8_t, 1 byte) plus getter/setter. This file is a non-tile intermediate so it CAN add fields.
- **`src/mjolnir/graphbuilder.cc`** (where `EdgeInfo`'s tag list is built — search for the existing `TaggedValue::kLayer` write site for the exact pattern) — emit `encode_tag(TaggedValue::kAdventureRiding) + static_cast<char>(class)` into the EdgeInfo names list when the way carries a non-zero class.

Template to mirror: see `src/mjolnir/osmway.cc:1109` for the `kLayer` write pattern.

Config plumbing: the OSM-tag → class-id allow-list belongs in `valhalla.json`'s `mjolnir.adventure_riding_sources` (a map). Default empty. The per-country pipeline in `FastGIS` populates it (`source=TET` → 1, etc.).

### ⏳ Layer 5 — Costing read path (next commit)

For each profile we care about (`motorcycle` first, then `auto`, `bicycle`, `pedestrian`):

- In `EdgeCost(const DirectedEdge*, const GraphTile*, …)`:
  - Look up `EdgeInfo` from tile (already done by some costing models).
  - Query `edge_info.GetTags()` for `TaggedValue::kAdventureRiding`.
  - If present *and* `use_adventure_riding_ < 1.f`, multiply the computed cost by `(use_adventure_riding_)` (or by a curve like `(use_adventure_riding_)^2` if linear is too gentle).
  - Hot-path guard: skip the EdgeInfo fetch when `use_adventure_riding_ == 1.f` (neutral, no effect) — this is the steady-state cost for stock requests.

Template to read tagged values: `EdgeInfo::GetTags()` returns `const std::multimap<TaggedValue, std::string>&` (see `valhalla/baldr/edgeinfo.h:242`).

Cost-multiplier semantics:

| `use_adventure_riding` | Effect on tagged edge |
|---|---|
| 0.0 | strongest preference (cost × 0 → always pick if reachable) |
| 0.5 | mild preference (cost × 0.5) |
| **1.0** | **neutral / default (no effect)** |
| > 1.0 | not currently exposed; would become avoidance. Could add in v2 |

### ⏳ Layer 6 — Tests (next commit)

- `test/gurka/test_adventure_riding.cc` — a Gurka integration test:
  - Builds a 3-edge mini graph: A→B (highway=primary), B→C (highway=secondary), A→C (highway=track + source=TET).
  - Default request (no `use_adventure_riding` set): expects A→B→C (faster road route).
  - With `use_adventure_riding=0.0`: expects A→C (single trail edge wins).
- Unit test in `test/edgeinfo.cc` — round-trip a `kAdventureRiding` tagged value through encode/decode.

CMake target: `gurka_adventure_riding` — auto-discovered from filename per `CLAUDE.md` convention.

---

## Build + test loop (where to run it)

**Local laptop is too constrained.** Norway tile build + the full Valhalla test suite needs:

- ~12 GB RAM for tile build of a single mid-size country
- ~30 GB free disk for build artefacts + test tiles
- Full C++20 toolchain + boost + protobuf + prime_server + zmq + Lua + GEOS

The new EU-wide server [[eu-wide-test-server]] is the natural home. Build commands per upstream `CLAUDE.md`:

```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build . -j$(nproc)
# After Layer 6 lands:
cd build && cmake --build . -j$(nproc) --target gurka_adventure_riding && \
            ./test/gurka/gurka_adventure_riding
```

---

## iOS / Android integration (after server validation)

The fork plugs into `valhalla-mobile` via a submodule URL change:

```bash
cd /Users/epi/dev/valhalla-mobile
git submodule set-url src/valhalla https://github.com/epifanio/valhalla.git
cd src/valhalla && git checkout feat/adventure-riding && cd ..
git add .gitmodules src/valhalla
git commit -m "vendor epifanio/valhalla fork (adventure-riding)"
```

Then the iOS / Android wrappers' build scripts will pick up the fork automatically. The Swift/Kotlin bridge needs one new option exposed on the route-request type — single field add in the wrapper layer.

**Tile pack invalidation.** Existing offline tile packs distributed by FastGIS will work with the new binary (forward-compatible — they just don't have the new TaggedValue, so the costing option becomes a no-op for them). Packs rebuilt with the fork's tile builder gain the new behaviour.

---

## Per-country pipeline integration

The existing FastGIS TET augmentation pipeline (`scripts/tet/` on `feat/tet-augmentation-pipeline` branch) already produces a country PBF where TET trails carry `source=TET`. To activate adventure-riding in Valhalla:

1. Add to the country's `valhalla.json`:
   ```json
   "mjolnir": {
     "adventure_riding_sources": { "TET": 1, "EuroVelo": 2 }
   }
   ```
2. Rebuild that country's tiles with the forked `valhalla_build_tiles`.
3. Pack + distribute as today.

The augmentation script doesn't need changes — its `source=TET` tag is already what the parser will key on.

---

## Status

| Layer | Status | Commit |
|---|---|---|
| 1. Constants | ✅ done | `d4018d034` |
| 2. Proto schema | ✅ done | `d4018d034` |
| 3. Option parsing | ✅ done | `d4018d034` |
| 4. Tile-build write path | ✅ done | `831e5797e` |
| 5. Costing read path + config-driven allow-list | ✅ done | `987c1db53` (+ `2df0c2f55`) |
| 6. Tests (`gurka_adventure_riding` + edgeinfo round-trip) | ✅ done | `185c7efc3` (+ `af869d8f0`); EdgeInfo switch fix `cadafdd98` |

**All six layers landed.** What remains for the wider rollout (outside the scope of this fork's plan):

- **iOS/Android propagation** — point `epifanio/valhalla-mobile`'s `src/valhalla` submodule at this fork (see procedure earlier in the doc), expose `use_adventure_riding` on the Swift/Kotlin route-request wrapper.
- **Per-country pipeline integration** — flip `mjolnir.adventure_riding_sources = {"TET": 1, …}` in the FastGIS country build configs and rebuild tiles. The `scripts/tet/*` augmentation pipeline already writes `source=TET`; no augmentation-script changes needed.
- **Upstream rebase** — fork is currently on `Release 3.6.4` (`72f459fc5`). Rebase onto a newer upstream tag before the next iOS release if upstream has materially advanced.
