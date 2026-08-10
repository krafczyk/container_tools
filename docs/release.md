# Runtime Evidence Release Gate

`tests/container_tools_runtime_test.sh` validates one closed, redacted
`container-tools.runtime-evidence/v1` report. A report binds its source commit
and source manifest, image and runtime identity, manifest digest, semantic operation classes, selected-root outcomes,
signal result, and run-owned cleanup. Fixture reports test the schema only and
can never become native claims.

`--run-parity` requires an accepted retained U1 Bash report and the unchanged
immutable parity image. It refuses a missing, malformed, or mismatched baseline
with exit `77`; replacing the image with a final image cannot establish parity.
`--run-final-image` separately records in-container operability only, never Bash
parity. Host and in-container source revisions may differ when their mount-plan
grammars remain compatible.

Build release candidates from the final clean source commit. Record the exact
source commit in the workspace release manifest.

Run at most one real Docker, Podman, SingularityCE, or Apptainer suite at a
time. The current child runner fails closed as unavailable until a retained
image and release-specific selected-root harness are supplied. Validate a saved
claim with `--validate-report REPORT --claimable`; malformed evidence exits `2`
and unavailable or fixture evidence exits `77`.
