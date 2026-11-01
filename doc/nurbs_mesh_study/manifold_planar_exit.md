# Native NURBS Manifold: planar exits

An inward Extrude Manifold can open a single selected cap through a parallel planar
exit face. Before the cap reaches that face it remains a pocket. At the exit plane,
the cap disappears, the extrusion walls stop at the exit, and the exit face gains
an inner trim loop. Continuing farther leaves the same opening. Every preview is
rebuilt from the original transform snapshot, so shortening the distance restores
the pocket; cancel, undo and redo use ordinary operator snapshots.

The kernel service `open_planar_extrusion_exit` operates on a private native geometry
candidate. It requires a closed, consistently oriented connected shell, a single
cap loop, and an exit with a convex straight outer boundary and polynomial bilinear
planar support. Positive rational boundary control hulls must lie strictly inside
the exit. Boundary contact and partial overlaps are outside this implementation.

The service translates the cap boundary back to its first supported exit plane,
updates the incident ruled walls, creates an affine exit chart, and projects copies
of the spatial boundary curves into that chart. The exit and walls share the same
edge identities. It removes only the cap face and validates boundary correspondence
before publishing. It does not convert the source into a display mesh or perform
a general solid Boolean. Curved exits, multiple selected caps, existing exit holes,
and openings crossing exit boundary edges need further intersection/trim operations.

Validation covers pockets, exact exit contact, overshoot, native boundary agreement,
consistent shared edge traversal, unchanged source inputs, modal preview retraction,
cancel, undo and redo. Viewport tests also ray cast through the opening to detect
leftover caps or exit fill triangles.
