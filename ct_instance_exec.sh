#!/bin/sh
# Generated package-relative compatibility trampoline. SPDX-License-Identifier: Apache-2.0 OR MIT
CT_PACKAGE_IDENTITY='@CONTAINER_TOOLS_BUILD_IDENTITY@'
CT_COMPATIBILITY_SCRIPT='ct_instance_exec.sh'

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P) || exit 78
exec "$script_dir/container-tools" --internal-compat "$CT_PACKAGE_IDENTITY" \
  "$CT_COMPATIBILITY_SCRIPT" "$@"
