# Selected-Root Host Execution

`container-tools host exec [--config PATH] [--profile NAME]
[--backend bubblewrap|proot|rewrite] [--allow-degraded=rewrite] [--verbose] --
COMMAND [ARG...]`
reads one strict TOML profile. Without a discovered profile it plans the
installed default: profile `host`, selected root `/host`, and exact semantic
manifest selector `/.container-tools-mount-plan`. A profile may instead name
another Linux root already visible inside the original container.

## Dispatch Boundary

After profile, manifest, path, cwd, environment, and executable planning,
`host exec` selects exactly one nested backend in fixed order: Bubblewrap,
PRoot, then rewrite. `--backend bubblewrap|proot|rewrite` forces one backend
and never falls back. A full-root profile may use rewrite only with
`--allow-degraded=rewrite`; `semantics = "rewrite"` is already eligible. A
failed selected launch is never retried with another backend. Host execution
and diagnosis never invoke Docker, Podman, SingularityCE, or Apptainer.

`container-tools host projection clear [--help]` clears managed cached
host-projection selections. It does not alter the immutable mount plan already
bound into a running container. After clearing, the next local foreground or
persistent launcher invocation performs a cold projection proof and publishes a
fresh mount plan. An absent cache succeeds; unexpected entries, symlinks, or
file types fail without deleting selection records. Clearing serializes with
cache readers and publishers and retains synchronization files.

Unknown, empty, missing, and duplicate `--backend` values fail with usage
status before configuration, manifest, allocation, or backend access.
`--verbose` writes one bounded backend-selection diagnostic to stderr
before dispatch.

Bubblewrap assembles a private tmpfs root from bounded selected-root entries,
preserves symlinks, and splits only branches needed by ordered requested
overlays. It recursively maps the current container's `/proc`, `/sys`, and
`/dev` rather than projected host kernel filesystems. Missing overlay target
ancestors and leaves are created only in private tmpfs branches; nested targets
under an earlier overlay must already exist in that overlay source. Existing
leaves must have a compatible type. Probing therefore cannot materialize paths
in the selected root or overlay sources. A root that cannot be represented
safely is a clean capability failure eligible for automatic PRoot fallback.
Bubblewrap uses its supported default non-CLOEXEC
descriptor inheritance through the trampoline and otherwise adds no namespaces
or isolation. When the caller supplies a high non-CLOEXEC descriptor, the
semantic probe verifies it before Bubblewrap becomes ready. PRoot
uses only `-r`, ordered `-b` entries including caller `/proc`, `-w`, the
internal trampoline, and payload argv; it is ineligible where container-tools
must enforce read-only root or projection access. Rewrite is deliberately weak:
it runs a validated native ELF or bounded shebang entry only and does not
provide descendant selected-root semantics. It emits a degradation warning
before dispatch that names the requested semantics, Bubblewrap and PRoot
selection outcomes, and the entry-point-only descendant limitation.

The parent resolves the running executable to a stable absolute regular-file
path, opens a descriptor that must still identify `/proc/self/exe`, and preserves
that verified static executable descriptor with a bounded build-identity control
record through nested launch. This avoids transporting procfs-origin descriptor
provenance through nested path translators. The child enters the
internal trampoline through `/proc/self/fd/N`, re-admits both descriptors with
`ct_executable_admit_trampoline`, verifies the planned cwd, one projection's
parent-captured device/inode/type identity, and one descendant
self-exec during a probe, then executes target argv directly. The
selected root therefore needs no container-tools metadata or trampoline file.

Before dispatch, status 125 identifies configuration, manifest, backend, or
trampoline setup failure; 126 identifies a target that exists but is not
executable or compatible; 127 identifies a target command that was not found.
The bounded stderr diagnostic names the correction without including the
selected profile, root, paths, manifest content, backend output, or payload.
Executable diagnostics distinguish not-found, inaccessible, incompatible,
dynamic-loader, and shebang-chain failures. Backend diagnostics distinguish a
missing tool, policy denial, probe timeout or failure, cleanup uncertainty, and
trampoline setup. After dispatch, command statuses and output pass through
unchanged without a container-tools suffix.

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
An unsupported grammar fails `host exec` or `host doctor` before a backend
probe or payload dispatch. Its single stderr diagnostic reports the expected
`ct-mount-plan-v1`, the actual observed discriminator when it consists solely
of printable ASCII, or `unknown` otherwise, and directs the caller to use a
compatible container-tools version or regenerate the mount plan. The operation
leaves the containing runtime instance running.

`bootstrap-internal` is never replayed. `generated-host-root` establishes root
evidence and is not replayed. Detected, explicit caller-data, and persistent-CWD
entries are overlays; none may target `/`. A full-root profile requires a
complete direct or fallback plan and at least one generated entry. Every
generated caller path identifies the selected lexical source beneath the outer
runtime's fixed `/host` projection. Its target path records the independently
canonicalized lower-root location, so ordinary host symlinks can make the two
suffixes differ. When they do, both visible paths must identify the same live
filesystem object.

## Executables And Probes

Bare commands resolve against profile `path`; commands containing `/` must be
absolute target-side paths. Resolution uses the composed lower-root and overlay
map. The planner follows ordinary symlinks, checks caller execute permission,
validates native ELF class/data/machine and interpreter records, and resolves at
most four absolute or supported GNU `env` shebang stages. Supported `env -S`
options, assignments, PATH changes, and command arguments are incorporated into
command lookup and rewrite argv construction; unsupported or ambiguous split
forms fail closed. Every resolved stage and a dynamic ELF's PT_INTERP loader
retain a descriptor through planning. Rewrite rejects set-id files and any
nonempty `security.capability` xattr on those descriptors, and fails closed when
capability inspection is ambiguous.

Backend probes use a fresh process-group supervisor. The supervisor remains an
unreaped group identity anchor after a probe leader exits, adopts and reaps
descendants including processes that create another group or session, and
preserves inherited terminal and non-close-on-exec descriptors. Deadline or
caller interruption performs bounded TERM-then-KILL cleanup through that owned
supervisor boundary. SIGINT and SIGTERM are restored and re-raised after
cleanup; unrelated processes are never signaled. Backend lookup uses the
caller's PATH before profile environment operations, resolves one absolute
caller-namespace executable for the selected probe and launch, and never lets a
profile PATH substitute that backend. Automatic selection falls through only
after a fully cleaned capability failure or timeout; setup, cleanup, or signal
restoration uncertainty exits 125.
Probe child stdout and stderr are redirected to `/dev/null`; backend probe
messages never appear in a host doctor JSON report or a host exec diagnostic.
After selection and final command construction, `host exec` replaces itself
with Bubblewrap, PRoot, or the rewrite entry point. The selected backend or
application directly inherits standard descriptors, process-group and terminal
ownership, signals, and shell job control; container-tools does not supervise
the final payload.

## Doctor

`container-tools host doctor [--config PATH] [--profile NAME] [--verbose]
--json` validates the same profile, selected root, and exact manifest before
reporting backend capability. Doctor probes internal backend capability but
never dispatches a user payload. Without `--json`, its human output is for
people and is not a machine interface.

### JSON Machine Contract

With `--json`, stdout is the closed `container-tools.host-doctor/v1` contract:
one compact UTF-8 JSON object followed by one newline, with no human prose on
stdout. The root object has exactly these required fields:

| Field | Type | Value and nullability |
| --- | --- | --- |
| `schema` | string | Always `container-tools.host-doctor/v1`. |
| `schema_version` | integer | Always `1`. |
| `profile` | string | Selected profile name; never null. |
| `architecture` | string | Package build architecture; never null. |
| `requested_semantics` | string | The selected profile's `full-root` or `rewrite` semantics; never null. |
| `mount_plan` | object | Required closed mount-plan object described below. |
| `backends` | array | Required array of exactly three closed backend objects in the fixed order below. |

`mount_plan` has exactly these required fields:

| Field | Type | Value and nullability |
| --- | --- | --- |
| `configured` | boolean | `true` when the selected profile names a mount plan; otherwise `false`. |
| `status` | string | One of the ten closed status values below; never null. |
| `digest` | string or null | The validated lowercase mount-plan digest when metadata was read successfully; otherwise `null`. |
| `strategy` | string or null | Validated manifest strategy (`direct`, `fallback`, or `none`) when metadata was read successfully; otherwise `null`. |
| `completeness` | string or null | Validated manifest completeness (`complete` or `partial`) when metadata was read successfully; otherwise `null`. |
| `detail` | string | A non-null machine-escaped diagnostic string. Current ready, partial, and none diagnoses use the empty string. |

The closed `mount_plan.status` values are:

| Status | Meaning |
| --- | --- |
| `disabled` | The profile does not configure a mount plan. |
| `ready` | A configured plan is complete, eligible, and usable for backend diagnosis. |
| `absent` | The configured plan is missing. |
| `partial` | The plan was parsed, but its completeness is `partial`. |
| `none` | The plan was parsed, but its strategy is `none`. |
| `malformed` | The plan cannot be read as the closed manifest grammar. |
| `future` | The plan uses an unsupported future manifest version. |
| `digest-mismatch` | The plan's declared digest does not match its content. |
| `semantic-invalid` | The plan is semantically invalid or incompatible with the selected profile. |
| `changed-during-read` | The exact configured plan changed or was replaced while being read. |

Each `backends` element has exactly these required fields:

| Field | Type | Value and nullability |
| --- | --- | --- |
| `name` | string | Fixed positional value: `bubblewrap`, then `proot`, then `rewrite`. Never null. |
| `installed` | boolean | Whether the backend executable is available. Rewrite is built in and reports `true`. |
| `eligible` | boolean | Whether the profile and invocation policy permit the backend. |
| `operational` | string | Exactly `yes`, `no`, or `not-probed`; never null. |
| `reason_code` | string | One of the nine closed reason codes below; never null. |
| `detail` | string | A non-null machine-escaped diagnostic string. The current backend implementation emits an empty string; it does not publish probe-output detail. |

The closed doctor `reason_code` values are `ready`, `not-installed`,
`incompatible-profile`, `mount-plan-unavailable`, `policy-denied`,
`probe-timeout`, `probe-failed`, `earlier-backend-ready`, and
`degradation-not-authorized`. `not-attempted` and `not-requested` are internal
execution-selection strings, not values produced by `host doctor`.

```json
{"schema":"container-tools.host-doctor/v1","schema_version":1,"profile":"host","architecture":"x86_64","requested_semantics":"full-root","mount_plan":{"configured":true,"status":"ready","digest":"0123456789012345678901234567890123456789012345678901234567890123","strategy":"direct","completeness":"complete","detail":""},"backends":[{"name":"bubblewrap","installed":true,"eligible":true,"operational":"yes","reason_code":"ready","detail":""},{"name":"proot","installed":true,"eligible":true,"operational":"not-probed","reason_code":"earlier-backend-ready","detail":""},{"name":"rewrite","installed":true,"eligible":false,"operational":"not-probed","reason_code":"degradation-not-authorized","detail":""}]}
```

Doctor does not probe backends when the mount plan is not `ready`; each backend
instead reports `operational: "not-probed"` and
`reason_code: "mount-plan-unavailable"`. Otherwise it probes in fixed
Bubblewrap, PRoot, rewrite order. After the first ready full-root backend,
later eligible backends report `earlier-backend-ready`; rewrite is a policy
capability and is not executed by doctor.

A complete diagnosis exits `0`, including when no backend is operational.
Invalid configuration or selected root, manifest I/O failure, an unrecognized
mount-plan read result, probe/setup/cleanup uncertainty, or inability to build
the report exits `125` with a stderr diagnostic naming the failed phase and a
recovery action. Before output begins, the
serializer constructs the full object, so allocation or serialization failure
emits no partial JSON. A strict no-partial-output guarantee for a failing stdout
write is not implemented: a short or failed stream write can leave partial JSON
before doctor returns `125`. `--verbose` writes its bounded fixed-order
selection diagnostic to stderr after profile/root validation and before
mount-plan assessment; JSON stdout remains machine-only on a successful write.
