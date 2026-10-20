# Encoding-0 PLY fitting

`plyfit.cpp` extracts the fitting and block assembly algorithms from
`encoder/tools/vgsplyseq.cpp` in the `TemporalPlyEncoderV4` worktree. It accepts
one chunk of trajectories rather than loading the take or discovering files.
The source CLI and its uncommitted changes are left untouched.

The editor supplies stable identity records, lifetimes, weighted samples and
median higher SH. The fitter uses the source tolerances and RD2's residual
rotation mode: position tolerance
0.5 mm, rotation tolerance 2 degrees, 16,384 position atoms, five colour/opacity
stages and five residual rotation stages. Small inputs use smaller books.
The returned permutation maps rank-ordered records back to preview identities.

OpenMP work is limited to eight threads. On MSVC x64 the AVX2/FMA search kernel
is isolated and selected only after CPU and OS-state checks; SSE2 remains the
fallback. Eigen retains its previous single-threaded execution. The editor
checks available system memory before fitting and spools fitted chunks to disk.

`plysequence_tests::realRd2ComparedWithNativeReferenceAndWithoutSidecars` uses
`PLY_RD2_REFERENCE` (the PLY directory beside its `.vgs`) to compare activity,
geometry and exported size. Set `PLY_RD2_ROUNDTRIP` to retain a reviewable export.
