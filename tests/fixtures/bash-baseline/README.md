# Bash Baseline Fixtures

`contracts.tsv` is the implementation-neutral U1 vector inventory. `mount.conf`
and `expected-results.tsv` are generated input and output bytes used by the
parity driver. They record stable contract classes rather than shell text,
runtime listings, or mutable temporary paths. `tests/parity_test.sh
--generate-fixtures` produces the complete directory and `--verify-fixtures`
rejects drift.

The immutable vectors record historical behavior with isolated HOME, XDG,
runtime, and fake-backend roots. They are evidence only: a later installed
native package uses the same data without retaining a sourceable Bash oracle.
