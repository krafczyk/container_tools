# Container Tools

`ct_exec.sh` and `ct_shell.sh` normalize foreground mounts and launch behavior
across Docker, Podman, SingularityCE, and Apptainer.

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
still reports partial. Policy v5 invalidates older cached selections that
dropped these ordinary or caller-unavailable trees.
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
image or absolute SIF path and a fresh work directory under the documented
root. Run one available backend at a time:

```sh
CT_HOST_PROJECTION_RUNTIME_TEST=1 \
  bash tests/host_projection_runtime_test.sh \
  --backend docker --image "$CT_HOST_TEST_DOCKER_IMAGE" \
  --work /tmp/mkchad-v1/host-root-projection-host/docker-RUN_ID

CT_HOST_PROJECTION_RUNTIME_TEST=1 \
  bash tests/host_projection_runtime_test.sh \
  --backend podman --image "$CT_HOST_TEST_PODMAN_IMAGE" \
  --work /tmp/mkchad-v1/host-root-projection-host/podman-RUN_ID

CT_HOST_PROJECTION_RUNTIME_TEST=1 \
  bash tests/host_projection_runtime_test.sh \
  --backend singularity --image "$CT_HOST_TEST_SINGULARITY_SIF" \
  --work /tmp/mkchad-v1/host-root-projection-host/singularity-RUN_ID

CT_HOST_PROJECTION_RUNTIME_TEST=1 \
  bash tests/host_projection_runtime_test.sh \
  --backend apptainer --image "$CT_HOST_TEST_APPTAINER_SIF" \
  --work /tmp/mkchad-v1/host-root-projection-host/apptainer-RUN_ID
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
has been assembled. They include the semantic generated-bind tuple (not mount
or inode presentation), effective UID, primary GID, sorted supplementary GIDs,
and the selected group mode. A warm matching call performs one profile-aware
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
