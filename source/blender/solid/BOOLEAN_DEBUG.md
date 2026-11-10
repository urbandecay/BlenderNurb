# Native Boolean diagnostics and replay

The first implementation milestone establishes a result contract and reproducible
kernel requests before expanding curved and NURBS solving. It does not extend
the supported surface combinations.

## Results

`boolean_bodies_result` returns a model only for `Success` or intentional `Empty`.
Invalid input, unsupported general curved dispatch, explicitly uncertain planar
classification, and other failures have distinct statuses. Older curved helper
errors still use `Failed` until those helpers expose typed failures. Messages
are not parsed to guess a status.

Input geometry is validated once, and the actual operand face sets must form
closed, consistently oriented boundaries. Output geometry and all output edge
uses are checked before success. A Union of nonempty inputs cannot successfully
return an empty boundary. These checks do not certify arbitrary set membership;
regressions also check volumes and known material points.

The older `boolean_bodies` API delegates to the same implementation. An engaged
empty model remains a successful result there, and successful calls clear stale
error text.

## Opt-in host diagnostics

Launch from the repository root, using the existing adjacent build directory:

```sh
BLENDER_SOLID_BOOLEAN_TRACE=1 \
BLENDER_SOLID_BOOLEAN_CAPTURE_DIR="$PWD/../solid-boolean-captures" \
../build_linux/bin/blender 2> ../solid-boolean-trace.log
```

Both settings are optional and disabled by default. The trace writes JSON lines
to stderr for native kernel calls and final operator publication. It records
the operation, algorithm path, result status, counts, source element IDs,
operator options and elapsed milliseconds. ID lists stop at 32 entries and
explicitly report truncation. The kernel includes at most six stage records.

The planar path measures preparation, partition/classification and reconstruction
separately. The older curved/planar helper owns all three internally, so its
`curved_planar_solve` timing is an aggregate of that helper. Publication time
includes the host work done by `Session::apply`; it is not an isolated tessellation
measurement. Operator and kernel times are elapsed operation times, not FPS.

The editor's analytic cylinder shortcut is labeled `analytic_cylinder`, with no
invented kernel stage measurements. Kernel failures are captured in `.solidbool`
files when the capture directory is set, up to 16 attempts per process. Each
request contains the complete native source passed to the kernel, both exact
face-ID lists and the operation; the trace's truncated lists are not used for
replay. Captures use native object coordinates and are independent of the view.
Invalid models that cannot be encoded report `unencodable`. Diagnostic I/O
failure never changes the Boolean outcome.

The editor-only cylinder shortcut does not yet produce kernel replay files.
Failures before operand extraction and final host publication failures also do
not produce a kernel failure capture. The original reported disappearing-body
scene still needs capture; deterministic fixtures are not a diagnosis of that
specific scene.

## Replay without Blender

```sh
cmake --build ../build_linux --target solid_boolean_replay -j 4
../build_linux/bin/solid_boolean_replay ../solid-boolean-captures/request.solidbool
```

Exit status is 0 for Success/Empty, 1 for an unsuccessful solve, and 2 for usage,
file I/O or malformed capture input. Replay does not modify the request or scene.
The checksummed, independently versioned diagnostic envelope is capped at 128 MiB
and one million IDs per operand. It is not a new `.blend` source schema.

## Regression checks

`solid_boolean_contract_test` covers set identities, known volumes and material
membership, complete/oriented operands, successful empty results, source
immutability, storage round trips and the current curved/curved rejection.
Its three-cylinder input and previously Booleaned curved body with a fresh
cylinder are explicit acceptance targets currently returning `Unsupported`.

`solid_boolean_replay_test` covers exact request round trips and replay, corrupted
checksums, truncated data and bounded count/length validation. Existing native
Boolean and geometry regressions continue to exercise the compatibility API.
