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

Docker and Podman use primary-only groups unless aggregate proof conclusively
reports a supported broader realization. SingularityCE and Apptainer retain
their native inherited caller groups. Selection policy is launcher state, not a
`ct_runtime.conf` or `ct_mount.conf` setting.

`tests/host_projection_runtime_test.sh` is the cumulative, opt-in host evidence
runner. It never pulls images or contacts a registry. Supply one already-local
image or absolute SIF path and a fresh work directory under the documented
root:

```sh
CT_HOST_PROJECTION_RUNTIME_TEST=1 \
  bash tests/host_projection_runtime_test.sh \
  --backend docker --image LOCAL_IMAGE \
  --work /tmp/mkchad-v1/host-root-projection-host/docker-RUN_ID
```

Every syntactically valid non-help invocation that can create its work directory
emits the same redacted `container-tools.host-projection-runtime/v1` JSON object
to stdout and `$work/report.json`. Disabled or unavailable prerequisites exit
77 and make no runtime support claim. Real runtime execution is an operator
handoff in this checkout; fixture results are not backend evidence.

`ct_instance_exec.sh` is the persistent SingularityCE/Apptainer variant for a
service that must retain its image mount after the launching command exits. It
requires a private absolute `--ct-instance-root`, serializes first use, and keys
the instance name to the runtime, host, active image and bootstrap identities,
and fixed bind profile:

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
