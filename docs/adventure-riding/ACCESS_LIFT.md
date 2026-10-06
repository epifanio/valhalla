# Access-ban lifts (FastGIS fork, tet-recovery WP-4)

Three costing options, all `false` by default. With all three off the engine
answers exactly as before. They exist for one FastGIS use: building
turn-by-turn on a rider's own imported track, at the rider's explicit risk
(owner Q1 and Q5, 2026-10-06; FastGIS `PLANNING/TODO_tet_overlay_recovery.md`
§3.6). FastGIS never sends them when it searches for a route. Unlike
`ignore_access`, they never lift every restriction at once.

| Option | Lifts | FastGIS level |
|---|---|---|
| `lift_motor_access_bans` | An edge closed to this vehicle by its access mask is usable when it is not `destination_only`, another mode may use it in that direction, and its use is not steps, an elevator, an escalator, a platform, a ferry, construction or transit. This covers an OSM ban (`motor_vehicle=no`, `motorcycle=no`), a path, footway, cycleway or bridleway, and a country default the tile builder baked in (Belgium's `highway=track`). A bridleway counts as open to another mode, because horses are not modelled. | b, c |
| `lift_private_access` | Together with the first option, also `destination_only` edges (`access=private`, `access=destination`). On its own, a `destination_only` edge is no longer kept to the ends of a route and pays no `destination_only_penalty`. | c |
| `lift_node_access_bans` | Gate nodes (`NodeType::kGate`, including lift gates) closed to this vehicle. Fixed bollards and sump busters stay closed. | c |

What they do not change:

- one-ways, turn restrictions and timed (conditional) access restrictions;
- the impassable-surface limit;
- costs and speeds. A lifted edge is costed and timed like any other edge of
  its class (an untagged track stays slow).

They are read in `DynamicCost` (`IsLiftedEdge`, `IsLiftedNode`), so they apply
in the router, in loki's candidate search and in the map matcher
(`trace_route`, `trace_attributes`) alike. The matcher already follows
`destination_only` edges and gates that are only `private` (they carry a
penalty, not a ban): FastGIS handles those stretches itself.

Tests: `test/gurka/test_access_lift.cc`.
