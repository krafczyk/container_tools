# Native Bash Baseline Evidence

This directory retains only accepted redacted
`container-tools.host-projection-runtime/bash-baseline-v1` reports and their
immutable local image identities. Each report is validated by
`tests/host_projection_runtime_test.sh --validate-bash-baseline-report` before
it can support a future C11 parity claim.

No report is checked in for this workspace because Docker, Podman,
SingularityCE, and Apptainer are unavailable. That absence is an explicit
unclaimed native combination, not fixture evidence.
