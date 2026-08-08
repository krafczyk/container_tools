# Selected-Root Host Execution

`container-tools host exec [--config PATH] [--profile NAME] -- COMMAND [ARG...]`
reads one strict TOML profile. Without a discovered profile it plans the
installed default: profile `host`, selected root `/host`, and exact semantic
manifest selector `/.container-tools-mount-plan`. A profile may instead name
another Linux root already visible inside the original container.

## Current Boundary

The U6 implementation validates configuration, exact manifest consumption,
layered path order, cwd and environment planning, executable format, and the
bounded probe-supervisor contract. A valid plan currently returns status 125
with `nested backend unavailable`. Bubblewrap, PRoot, rewrite dispatch, and
doctor output are U7 work. Host operations never invoke an outer container
runtime.

Before dispatch, status 125 identifies configuration, manifest, or setup
failure; 126 identifies a target that exists but is not executable or
compatible; 127 identifies a target command that was not found. Once U7 adds
the dispatch boundary, command statuses pass through unchanged.

## Profiles

Profiles use `version = 1`, `default_profile`, and `[profiles.NAME]`. Each
profile requires `root`, `root_access`, `semantics`, `cwd_unmapped`, and
`path`. `mount_plan`, `environment_remove`, `environment`, and `projections`
are optional. Unknown fields, malformed enums, non-normalized paths, duplicate
targets, and excessive counts fail before a backend probe.

Discovery order is explicit `--config`, `CONTAINER_TOOLS_HOST_CONFIG`,
`$XDG_CONFIG_HOME/container-tools/host.toml`, then
`$HOME/.config/container-tools/host.toml`. The current directory is never a
discovery source. The first existing malformed candidate fails without falling
through. Ordinary symlinks and caller permissions remain authoritative.

The lower `root` maps to target `/`. Manifest-derived caller-data overlays and
configured projections are merged into one parent-before-child order.
Unrelated manifest entries precede configured projections and declaration
order is retained within each source. Cwd and executable mapping use the
longest composed prefix. Environment operations remove configured names, set
the profile-derived `PATH`, then apply configured string values. A configured
PATH element is one absolute path and cannot contain `:`.

## Exact Manifests

Omitting `mount_plan` disables semantic-manifest consumption. A configured path
is required and read exactly; container-tools never scans state directories.
The shared `ct-mount-plan-v1` parser verifies bounds, framing, digest, enums,
and every tuple. In-place changes and path replacement during the read fail.

`bootstrap-internal` is never replayed. `generated-host-root` establishes root
evidence and is not replayed. Detected, explicit caller-data, and persistent-CWD
entries are overlays; none may target `/`. A full-root profile requires a
complete direct or fallback plan and at least one generated entry. Every
generated caller path must equal the normalized join of the outer runtime's
fixed `/host` projection and its target path; the selected lower root remains
independent.

## Executables And Probes

Bare commands resolve against profile `path`; commands containing `/` must be
absolute target-side paths. Resolution uses the composed lower-root and overlay
map. The planner follows ordinary symlinks, checks caller execute permission,
validates native ELF class/data/machine and interpreter records, and resolves at
most four absolute or supported GNU `env` shebang stages. Supported `env -S`
options, assignments, and PATH changes are incorporated into command lookup;
unsupported or ambiguous split forms fail closed. The original command
descriptor remains open until planning finishes.

Backend probes use a fresh process-group supervisor. The supervisor remains an
unreaped group identity anchor after a probe leader exits, adopts and reaps
descendants including processes that create another group or session, and
preserves inherited terminal and non-close-on-exec descriptors. Deadline or
caller interruption performs bounded TERM-then-KILL cleanup through that owned
supervisor boundary. SIGINT and SIGTERM are restored and re-raised after
cleanup; unrelated processes are never signaled.
