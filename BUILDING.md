# Build notes

The current release executables were built with Open Watcom 2.0 using 32-bit DOS LE targets and bound with DOS/32A 26.1.2. Their hashes and validation references are recorded in `PACKAGE-MANIFEST.yaml`.

The build scripts used in the Codex development workspace refer to private sibling NF-DOS/NFLLM directories and a managed-process test harness. They are intentionally not copied into this snapshot because they contain workstation-specific paths. A public, independently reproducible build workflow has not yet been prepared; the source itself and verified release executables are included.

Build dependencies that must be acquired and configured separately include Open Watcom, DOS/32A bind tools, and a DOSBox-compatible test environment with NF-DOS V23. No compiler, extender utilities, NF-DOS image, DOSLFN driver, or private test data is included.
