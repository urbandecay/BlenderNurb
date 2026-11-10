# Native Solid Boolean implementation

The native Solid model owns its vertices, curves, edges, coedges, faces and
surfaces. Solid Boolean operations read and write that model directly. The
Blender mesh is a derived display and selection adapter; no external geometry
kernel runs at the operation boundary.

## Adapted edge interval partition helper

`boolean_pave.cc` adapts `BOPDS_PaveBlock::Update` from Open CASCADE Technology
8.0.1:

`src/ModelingAlgorithms/TKBO/BOPDS/BOPDS_PaveBlock.cxx`

Reference file SHA-256:
`b9acb497674b10a632d3dc06f8a0033eef00558a4c637e6470b29d9fafd80052`.

The helper collects endpoint and intersection markers, sorts them by curve
parameter, and creates adjacent edge intervals with their source edge identity.
It uses Solid IDs and standard containers. It also checks invalid parameter
ranges, rejects unreconciled coincident vertices, removes exact duplicate
markers, and preserves descending edge traversal and repeated periodic seam
occurrences. The helper itself does not call or link the OCCT kernel.

The original copyright, LGPL 2.1 license and OCCT exception are retained in the
source and the accompanying license texts.

## Native Boolean coverage and remaining work

`boolean.cc` uses the intervals to split shared boundaries before cancelling
interior edges and assembling planar faces. `intersections.cc` computes native
line/conic intersections with planes while retaining original curve parameters.
The planar face builder, cylinder chart helpers, classifier and sewing helpers
provide more pieces of the native pipeline, but do not yet form a general
curved-surface Boolean operator.

General curved operations still need intersection data shared across both
operands, bounded intersection curves, face splitting in surface parameter
space, coincident-region classification, and shell reconstruction with result
history. Other surface families remain unsupported until those stages are
implemented and tested end to end.

The native planar path and existing cylinder special cases remain available.
Helper coverage does not imply that arbitrary curved Booleans are available in
the UI.
