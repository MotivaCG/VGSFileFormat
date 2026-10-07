# MINT reader and colour operators

Reader and despill subset of `Gracia4DGSConverter/mintfile.{h,cpp}`, copied from
`D:/Trabajos/THE4DSCANNER/GraciaConverter/Gracia4DGSConverter` on 2026-10-07.
Retains format-6 parsing, interpolation, SH reconstruction and the converter dictionary
based despill, including angular chroma limiting and skin recovery. Skin recovery now
uses SH appearance to protect neutral cloth from false skin classification. It does
not write MINT. `despillLogical` presents complete decoded VGS arrays to that same
operator in memory; no synthetic MINT container is emitted. The sampled fallback
retains the frame colour operator in `despillcolor.h`.

Licence: LICENSE.md. Keep the original colour operator's semantics when updating.

VGS decoding compiles sibling sources without writer code or signing keys.
The separate `editor_authoring` target owns logical MINT import, container writing
and signing. The editor links both because it is an authoring application.
