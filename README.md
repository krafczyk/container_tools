# Container Tools

`ct_exec.sh` and `ct_shell.sh` normalize foreground mounts and launch behavior
across Docker, Podman, SingularityCE, and Apptainer.

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
