# Bash Baseline Fixtures

`contracts.tsv` is the implementation-neutral U1 vector inventory. `mount.conf`
and `expected-results.tsv` are generated input and output bytes used by the
parity driver. They record stable contract classes rather than shell text,
runtime listings, or mutable temporary paths. `tests/parity_test.sh
--generate-fixtures` produces the complete directory and `--verify-fixtures`
rejects drift.

The vectors are exercised against the current Bash entry points with isolated
HOME, XDG, runtime, and fake-backend roots. A later installed native package
uses the same driver and fixture directory; it must not copy `ct_library.sh`
into this tree.
