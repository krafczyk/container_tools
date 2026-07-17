# Container Tools

`ct_exec.sh` and `ct_shell.sh` normalize mounts and launch behavior across
Docker, Podman, SingularityCE, and Apptainer.

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
