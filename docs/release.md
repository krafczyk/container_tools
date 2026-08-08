# Runtime Evidence Release Gate

`tests/container_tools_runtime_test.sh` validates one closed, redacted
`container-tools.runtime-evidence/v1` report. A report binds its source commit
and source manifest, package archive and release identity, image and runtime
digests, manifest digest, semantic operation classes, selected-root outcomes,
signal result, and run-owned cleanup. Fixture reports test the schema only and
can never become native claims.

`--run-parity` requires an accepted retained U1 Bash report and the unchanged
immutable parity image. It refuses a missing, malformed, or mismatched baseline
with exit `77`; replacing the image with a final image cannot establish parity.
`--run-final-image` separately requires a redacted in-container
`container-tools.release/v1` identity and records operability only, never Bash
parity. Host and in-container releases may differ within one product major.

Use the retained U10 x86_64 musl archive only with its exact digest:

```sh
archive=/tmp/mkchad-v1/container-tools-c11/u10-final-output-2b2b3d7/container-tools-1.0.0-dev-x86_64-musl-2b2b3d73b141b2abd4cdee41f5b06e0934e0ac26.tar.gz
sha256=9961bd25f97d837c55796f0439c5e6fe317515af01aed0d5eca34f6ef46d21ff
```

Run at most one real Docker, Podman, SingularityCE, or Apptainer suite at a
time. The current child runner fails closed as unavailable until a retained
image and release-specific selected-root harness are supplied. Validate a saved
claim with `--validate-report REPORT --claimable`; malformed evidence exits `2`
and unavailable or fixture evidence exits `77`.
