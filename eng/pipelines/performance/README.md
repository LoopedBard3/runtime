# Packaged CoreCLR artifacts for BCS

The ordinary Linux x64/arm64 and Windows x64/x86/arm64 lanes publish
`RuntimeDistribution_<linux|windows>_<arch>_Release_coreclr` in addition to the
unchanged raw `BuildArtifacts_*` artifact. Other lanes and upload guards are unchanged.

The additional artifact contains one same-name `.zip`. Its root holds original
files from `artifacts/packages/Release/Shipping`:

- `dotnet-runtime-<V>-<RID>.zip` on Windows or `.tar.gz` on Linux.
- `Microsoft.NETCore.App.Runtime.<RID>.<V>.nupkg`.
- `Microsoft.NETCore.App.Ref.<V>.nupkg`.
- `Microsoft.NETCore.App.Host.<RID>.<V>.nupkg`.
- `Microsoft.NETCore.App.Crossgen2.<RID>.<V>.nupkg`, when present.

`select_runtime_distribution.py` requires exactly one match per required file and
at most one Crossgen2 package, excluding symbol packages. Missing, empty, or
ambiguous inputs fail. The existing `clr+libs+host+packs` build produces these
outputs; no second build or pack invocation is needed.

The selector supplies a file list to `CopyFiles@2`, which stages the originals.
The existing `upload-artifact-step.yml` then archives that directory as a ZIP
without a root folder and publishes it. Inner archives are never extracted or
repacked, so their bytes, generated metadata, and internal Unix modes are preserved.
Packaging is trusted: the selector does not inspect metadata or validate assemblies.
There is no manifest, hash tree, or additional metadata. Consumers read versions,
TFMs, and framework requirements from the original packages.

Update runtime and performance together. Performance uploads this single ZIP
under the existing repository/SHA/configuration path alongside legacy artifacts.
Historical runtime revisions need updated or pinned producer templates; raw-only
builds have no fallback.

Run the small offline selection tests:

```text
python -m unittest discover -s eng/pipelines/performance/scripts -p test_select_runtime_distribution.py -v
```
