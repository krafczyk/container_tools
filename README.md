# Container Tools

`ct_exec.sh` and `ct_shell.sh` normalize foreground mounts and launch behavior
across Docker, Podman, SingularityCE, and Apptainer.

## C11 Package Bootstrap

The C11 package bootstrap installs one static `container-tools` executable,
five generated compatibility-script trampolines, and immutable package metadata
under a caller-selected prefix. It does not replace the current Bash callers in
this unit. Native behavior commands intentionally return exit status `70` with
a `not implemented` diagnostic until their corresponding migration units land.

The closed native command hierarchy is `exec`, `shell`, `instance exec`,
`instance identity`, `mount detect`, `mount args`, `runtime exec`, `buildx
exec`, `host exec`, `host doctor`, and `package verify`. `--help`, `--version`,
and `--version --json` are global. `package verify [--json]` validates the
static executable, generated release metadata, and all sibling trampolines
before reporting the immutable identity. Any missing or mixed package component
fails with exit status `78` before command behavior, state access, or backend
probing.

Install with CMake's normal prefix selection, for example:

```sh
cmake -S . -B /tmp/mkchad-v1/container-tools-c11/release -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/mkchad-v1/container-tools-c11/release
cmake --install /tmp/mkchad-v1/container-tools-c11/release --prefix /opt/container-tools
/opt/container-tools/bin/container-tools package verify --json
```

The executable and scripts resolve their sibling metadata, so a complete prefix
can move as a unit. Selection through `PATH` or an exact executable/script path
uses that selected prefix; mixing files from two prefixes is rejected. The
bootstrap is not a release-complete replacement for the retained Bash surface.

## Semantic Mount Plans

Every non-dry-run foreground launch on a local runtime endpoint and every
persistent instance start mounts one read-only, versioned semantic mount-plan
manifest at the stable in-container selector `/.container-tools-mount-plan`. A
future host bridge reads that bound file directly; it must not scan host state
and infer a plan hash. Automatic launches through an explicit remote
Docker/Podman endpoint retain the existing remote launch without a local-only
manifest bind; required host projection already rejects those endpoints.

The default host record path is
`${XDG_STATE_HOME:-$HOME/.local/state}/container-tools/mount-plans/v1/<64-hex-mount-plan-digest>.manifest`.
`CT_MOUNT_PLAN_STATE_ROOT` is a narrow test-state override. The state directory
must be absolute and cannot contain a colon, comma, or newline because every
supported backend must represent the resulting bind path without ambiguity.
The directory and every record are current-user-owned and private; records are
mode `0600` and published with a temporary file plus atomic rename. A matching
validated content-addressed record is reused. Malformed, mismatched, symlinked,
wrong-owner, or insecure records fail the launch rather than being replaced.
State creation, stale-temporary cleanup, locking, publication, and final
validation share one bounded process-group operation. Stale content-addressed
records may remain after an instance exits. A timed-out operation receives one
separate one-second bounded recovery attempt after its process group is dead;
later publishers also recover any private temporary that remains.

The closed NUL-delimited `ct-mount-plan-v1` grammar is:

```text
version, digest, backend, strategy, completeness, group_mode, entry_count,
entry_count * (role, caller_path, target_path, access, recursion)
```

Every scalar, including the final recursion field, is followed by NUL. The
digest is lowercase SHA-256 over the same sequence with the digest field
omitted: `version`, then `backend` through the final entry field. `entry_count`
is canonical decimal and is limited to 4096; the complete record is limited to
1 MiB. Backends are `docker`, `podman`, `singularity`, and `apptainer`.
Strategies are `direct`, `fallback`, and `none`; completeness is `complete` or
`partial`. Group modes are `numeric-supplementary`, `keep-groups`,
`primary-only`, `native-inherited`, and `none`.

Each ordered entry records a role, caller-visible canonical container path,
resolved selected-root target path, access (`inherit` or `read-only`), and
recursion (`non-recursive` or `runtime-default`). Roles are
`generated-host-root`, `detected-automatic`, `explicit`,
`bootstrap-internal`, and `persistent-automatic-cwd`. Generated entries use the
surviving post-conflict projection plan, `inherit`, and backend-specific
recursion. Detected, explicit, and persistent-CWD entries use the same canonical
caller and target path with `inherit` and `runtime-default`. The bootstrap entry
uses `/.container-tools-bootstrap` for both paths, `read-only`, and
`runtime-default`. Entries retain launcher assembly order.

The manifest excludes rendered argv, raw mountinfo, credentials, stat
identities, unrelated host-source presentation, its own stable bind, and the
read-only masks that prevent writable mount aliases from reaching its backing
directory. Those masks cover generated, detected, explicit, persistent-CWD, and
native runtime implicit HOME/CWD aliases. A conflicting destination or a caller
source inside private manifest state fails before backend dispatch.

The manifest bind is added only after the complete semantic plan is hashed and
published. Dry runs create no mount-plan state and add no manifest bind.
`/.container-tools-mount-plan` is reserved and cannot be a caller bind target,
including a lexical alias containing repeated separators, `.` or `..`.
Cross-backend caller bind paths cannot contain a colon, comma, or newline.
Persistent instance profiles include the resulting manifest path and bind, so a
plan or protocol change selects a new instance without circular hashing.

## Host-root projection

Foreground launchers default to `--ct-host-root auto`. They make a bounded cold
capability proof for an image/backend and cache only the selected strategy and,
for fallback, its selected path plan. A warm foreground launch consumes that
record without a runtime probe. Generated writable binds appear beneath `/host`
and are prepended without replacing image paths or caller-provided equivalent
`/host` mounts. Host kernel API filesystems are excluded.

Use `--ct-host-root required` to refuse the payload unless the current complete
projection is available. `--ct-host-root-refresh` ignores a warm record for one
launch and performs a new cold proof; a failed refresh does not consume the old
record. Automatic launches warn and retain the existing launch behavior when a
projection is unavailable or partial. Explicit remote Docker/Podman endpoint
selectors are intentionally unavailable because their daemon host is not local.
Top-level kernel API exclusions are complete by definition and do not produce
an incomplete-projection warning. When an ordinary tree such as `/run` contains
a nested kernel mount, fallback planning recursively splits only that mount's
ancestors and retains their safe file, socket, symlink, and directory siblings.
Branches the caller cannot list or traverse are omitted without warning because
they are already unavailable under caller authority. Cold entries that cannot
be resolved are treated the same way; losing a previously proven warm entry
still reports partial. Policy v6 invalidates older cached selections that
dropped these ordinary or caller-unavailable trees or reprojected a reserved
host mirror.
The reserved `/host` destination is never projected again as source data, so a
nested launcher does not create a recursive `/host/host` mirror.
Only Docker's explicit `default` context and local Unix `DOCKER_HOST` or
`CONTAINER_HOST` selectors are eligible; named Docker contexts, Podman
connections, machine selectors, SSH, and TCP endpoints are unavailable.
Persisted Docker and Podman selectors are read from their bounded client config
when the corresponding environment selector is unset. Cold Docker/Podman proof
also binds and verifies a private client nonce, so reaching a Unix socket alone
does not establish that its daemon sees this host filesystem.

Docker and Podman use primary-only groups unless aggregate proof conclusively
reports a supported broader realization. SingularityCE and Apptainer retain
their native inherited caller groups. Selection policy is launcher state, not a
`ct_runtime.conf` or `ct_mount.conf` setting.

`tests/host_projection_runtime_test.sh` is the cumulative, opt-in host evidence
runner. It never pulls images or contacts a registry. Supply one already-local
image or absolute SIF path and a fresh work directory under
`/tmp/mkchad-v1/container-tools-c11/host-projection-runtime`. Run one available
backend at a time:

```sh
CT_HOST_PROJECTION_RUNTIME_TEST=1 \
  bash tests/host_projection_runtime_test.sh \
  --backend docker --image "$CT_HOST_TEST_DOCKER_IMAGE" \
  --work /tmp/mkchad-v1/container-tools-c11/host-projection-runtime/docker-RUN_ID

CT_HOST_PROJECTION_RUNTIME_TEST=1 \
  bash tests/host_projection_runtime_test.sh \
  --backend podman --image "$CT_HOST_TEST_PODMAN_IMAGE" \
  --work /tmp/mkchad-v1/container-tools-c11/host-projection-runtime/podman-RUN_ID

CT_HOST_PROJECTION_RUNTIME_TEST=1 \
  bash tests/host_projection_runtime_test.sh \
  --backend singularity --image "$CT_HOST_TEST_SINGULARITY_SIF" \
  --work /tmp/mkchad-v1/container-tools-c11/host-projection-runtime/singularity-RUN_ID

CT_HOST_PROJECTION_RUNTIME_TEST=1 \
  bash tests/host_projection_runtime_test.sh \
  --backend apptainer --image "$CT_HOST_TEST_APPTAINER_SIF" \
  --work /tmp/mkchad-v1/container-tools-c11/host-projection-runtime/apptainer-RUN_ID
```

Every syntactically valid non-help invocation that can create its work directory
emits the same redacted `container-tools.host-projection-runtime/v1` JSON object
to stdout and `$work/report.json`. It records one cold selection, 20 warm
samples, a required-mode check, a refresh, normalized launcher operation counts,
the resolved backend executable digest, and exact run-owned cleanup. Docker and
Podman use the immutable local image ID;
native backends stage the supplied SIF beneath `--work` and hash both source and
staged bytes before and after the run. `--force-fallback` is available only for
Docker and Podman evidence: its tracing adapter rejects the direct probe before
creation, delegates the fallback and payload unchanged, and labels the report
as `forced-fallback` rather than a natural rejection.

Validate a returned real-runtime report against the currently reviewed launcher,
library, and test source manifest before accepting it. Validation requires the
locally installed `jq` parser; it never installs tools or contacts a network:

```sh
bash tests/host_projection_runtime_test.sh \
  --validate-report /tmp/mkchad-v1/host-root-projection-host/docker-RUN_ID/report.json
```

The validator accepts exactly one closed JSON object with the current source
commit and manifest, one of each case ID, backend-specific cold/refresh counts,
20 operation-free warm samples, required and persistent operation semantics,
and a passed host-evidence result. It rejects fixture reports, unknown fields,
empty operation evidence, and incomplete cleanup. Deterministic fake runs are explicitly labeled
`"evidence_kind":"fixture"` and may be checked only with the test seam
`--validate-fixture-report`; they never substantiate a backend claim. Exit `0`
means every applicable real-runtime case passed, `1` means a valid failed or
unacceptable report, `2` means malformed or invalid report input, and `77`
means the report is unavailable or `jq` is unavailable and makes no runtime
support claim. Docker and Podman mark persistent `HP-HOST-006` as an explicit skip;
SingularityCE and Apptainer execute and clean up their run-owned persistent
instance. This development environment currently has none of Docker, Podman,
SingularityCE, or Apptainer available, so all four real-runtime claims remain
unclaimed; deterministic fake-runtime results are not backend evidence.

Use `--bash-baseline` before the C11 transition to retain an exact-source Bash
report for one available runtime and immutable local image. It emits the closed
`container-tools.host-projection-runtime/bash-baseline-v1` schema with
`"evidence_kind":"bash-baseline"`; its existing case results contain the
semantic outcomes, operation classes, and cleanup result. Validate it with
`--validate-bash-baseline-report`. Fixture runtimes are rejected in this mode,
and an unavailable runtime reports `unavailable` without making a parity claim.

`ct_instance_exec.sh` is the persistent SingularityCE/Apptainer variant for a
service that must retain its image mount after the launching command exits. It
requires a private absolute `--ct-instance-root` without colon, comma, or
newline, serializes first use, and keys the instance name to the runtime, host,
active image and bootstrap identities, and fixed bind profile:

```sh
ct_instance_exec.sh --apptainer \
  --ct-instance-root "$HOME/.local/share/example/container-instances" \
  --ct-bind /host/data:/container/data \
  --ct-bootstrap /host/bin/container-env \
  -- image.sif command --argument
```

Later matching calls reuse the exact `instance://` profile. The helper does not
stop instances automatically: service shutdown and runtime-instance cleanup are
separate authority decisions. Stop an exact idle instance manually only after
accounting for every process and caller that uses it.

Persistent profiles are finalized only after automatic host-projection selection
and every generated, detected, explicit, bootstrap, and working-directory bind
has been assembled and the semantic mount-plan manifest has been published.
They include the semantic generated-bind tuple (not mount
or inode presentation), effective UID, primary GID, sorted supplementary GIDs,
the selected group mode, and the stable manifest bind. A warm matching call performs one profile-aware
liveness exec followed by its payload; it never reruns a capability probe or
adds a bind to an already-running instance. Profile mismatch refuses before the
payload and never stops an existing instance.

Instance creation records a private mode-`0600` pending name/profile/nonce
journal and binds it read-only at a fixed internal identity path. After the
instance verifies the pinned record, the host path is unlinked while the
instance mount retains the original bytes. Recovery adopts only an instance
that exposes the exact pending nonce; ambiguous liveness or list results retain
that journal. A structured empty backend instance list permits only a retry
with the recorded nonce, and recovery never signals or stops a runtime
instance. `CT_DRY_RUN` prints a representative persistent exec command without
creating an instance root, projection cache, or pending journal.

`/.container-tools-instance-identity` is reserved for this internal read-only
record and cannot be an explicit bind destination. Identity-transport version
changes select a new instance profile rather than attempting to adopt an
instance created under an older verification contract.

The cumulative runtime runner includes `HP-HOST-006`. Docker and Podman report
that persistent case as an explicit backend-inapplicable skip. SingularityCE
and Apptainer run the persistent profile/reuse case against the supplied local
runtime and report its actual pass or fail result; fixture success is not a
backend claim.

MkChad uses `ct_exec.sh` for `mkchad-opencode-server status`. This foreground
path does not create a persistent instance or runtime state. Its host wrapper
passes bounded runtime, selected-image, and existing-instance scalar evidence
to the image; only the canonical managed `neovim.sif` selector is eligible for
selected-image identity. PPC64LE images intentionally do not ship OpenCode and
therefore do not provide this OpenCode baseline manifest contract.

## Bootstrap hooks

Use `--ct-bootstrap HOST_PATH` when environment setup must run inside the
container after runtime-provided environment variables are available. The
bootstrap must be an absolute, readable executable. Container tools mounts it
read-only and invokes it with the requested command as positional arguments.
The bootstrap must finish with an equivalent of `exec "$@"`.

Bootstrap launches require `--` before the image so container-tools options
cannot be confused with image or command arguments:

```sh
ct_exec.sh --apptainer \
  --ct-bind /host/data:/container/data \
  --ct-env FEATURE=enabled \
  --ct-bootstrap /host/bin/container-env \
  -- image.sif command --argument
```

Interactive shells use the same hook. `/bin/sh` is the default container shell;
override it with `--ct-container-shell` when the image provides another shell:

```sh
ct_shell.sh --apptainer \
  --ct-bootstrap /host/bin/container-env \
  --ct-container-shell /bin/bash \
  -- image.sif
```

`--ct-env NAME=VALUE` provides backend-normalized environment injection. Values
accept shell-literal path and text characters; substitution and shell-control
characters are rejected rather than evaluated differently by native container
runtimes. All arguments after `--` are preserved as distinct arguments;
bootstrap execution does not use `eval` or reconstruct a command string.

## Runtime storage configuration

Container tools reads optional machine-local storage defaults from
`~/.config/ct_runtime.conf`. Override that path with `CT_RUNTIME_CFG`. The file
is data, not shell: use one `KEY=ABSOLUTE_PATH` assignment per line, with blank
lines and `#` comments allowed. It must be owned by the current user and not be
group- or world-writable. Unknown or duplicate keys fail closed.
`CT_DOCKER_BUILD_CACHE_DIR` cannot contain a comma because Buildx local-cache
descriptors use commas as field separators. A dotfile symlink is supported when
the file opened through it satisfies the same ownership, type, and mode checks.

```text
CT_SINGULARITY_CACHE_DIR=/data/container-cache/singularity
CT_SINGULARITY_TMP_DIR=/data/container-tmp/singularity
CT_DOCKER_BUILD_CACHE_DIR=/data/container-cache/docker-buildx
CT_DOCKER_BUILD_TMP_DIR=/data/container-tmp/docker-buildx
```

Configured directories are created on first use. Existing plain directories
must be current-user-owned and must not be group- or world-writable; safe
directories are tightened to mode `0700`, while symlinks and previously writable
directories fail closed. Ancestors must also be plain directories and cannot be
group- or world-writable unless they are sticky shared roots such as `/tmp`.
An explicit `SINGULARITY_CACHEDIR`,
`SINGULARITY_TMPDIR`, `APPTAINER_CACHEDIR`, `APPTAINER_TMPDIR`, or `TMPDIR`
environment value takes precedence and is not created or permission-modified.
SingularityCE and Apptainer launchers receive their native cache and
temporary-directory variables. Ordinary Docker and Podman launchers do not
apply build-storage defaults.

Neovim Docker build scripts use an architecture-specific Buildx local cache
below `CT_DOCKER_BUILD_CACHE_DIR` and use `CT_DOCKER_BUILD_TMP_DIR` as the
Docker client's build-time `TMPDIR`. Builds of one architecture serialize on a
bounded lock. Each build imports the current cache, exports to a fresh staging
generation, promotes that generation only after success, and removes the
superseded generation so `mode=max` cache blobs do not grow without bound.
`CT_DOCKER_BUILD_LOCK_TIMEOUT` changes the default 30-second lock wait;
`CT_RUNTIME_STORAGE_TIMEOUT` changes the default 10-second deadline for
individual storage metadata and mutation commands. Both overrides are positive
numbers of seconds.

Build integrations call `configure_docker_build_storage ARCHITECTURE`, pass the
resulting `DOCKER_BUILD_CACHE_ARGS` array to `docker buildx build`, arrange
`discard_docker_build_storage` on every exit path, and call
`commit_docker_build_storage` after a successful export. These functions return
nonzero for malformed configuration, unsafe paths, lock timeout, interrupted
cache state that cannot be recovered, or a missing Buildx cache index.

These Docker settings do not relocate persistent images and layers held by the
Docker daemon. Configure the daemon's `data-root` and daemon-start
`DOCKER_TMPDIR` separately when that storage must move; a client launcher cannot
safely change an already-running daemon's storage root.
