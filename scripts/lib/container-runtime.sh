# Detect docker or podman for manylinux container builds.
#
# Sets and exports CONTAINER_RUNTIME to "docker" or "podman".
# Override with the CONTAINER_RUNTIME environment variable.
#
# Usage (from scripts/*.sh):
#   source "$(dirname "$0")/lib/container-runtime.sh"
#   detect_container_runtime "$@"

_container_runtime_die() {
  if declare -F die >/dev/null 2>&1; then
    die "$@"
  else
    echo "error: $*" >&2
    exit 1
  fi
}

_container_runtime_reexec_cmd() {
  local arg
  local -a quoted=()
  for arg in "$@"; do
    quoted+=("$(printf '%q' "$arg")")
  done
  printf '%s' "${quoted[*]}"
}

detect_container_runtime() {
  if [[ -n "${CONTAINER_RUNTIME:-}" ]]; then
    command -v "$CONTAINER_RUNTIME" >/dev/null 2>&1 \
      || _container_runtime_die "CONTAINER_RUNTIME command not found: $CONTAINER_RUNTIME"
    "$CONTAINER_RUNTIME" info >/dev/null 2>&1 \
      || _container_runtime_die "CONTAINER_RUNTIME=$CONTAINER_RUNTIME is not usable (try: $CONTAINER_RUNTIME info)"
    export CONTAINER_RUNTIME
    return 0
  fi

  if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    CONTAINER_RUNTIME=docker
    export CONTAINER_RUNTIME
    return 0
  fi

  # Permission denied: retry the caller once with the docker group.
  if [[ -z "${_CONTAINER_RUNTIME_SG_REEXEC:-}" ]] \
    && command -v docker >/dev/null 2>&1 \
    && command -v sg >/dev/null 2>&1; then
    if sg docker -c "docker info" >/dev/null 2>&1; then
      export _CONTAINER_RUNTIME_SG_REEXEC=1
      exec sg docker -c "$(_container_runtime_reexec_cmd "$0" "$@")"
    fi
  fi

  if command -v podman >/dev/null 2>&1 && podman info >/dev/null 2>&1; then
    CONTAINER_RUNTIME=podman
    export CONTAINER_RUNTIME
    echo "note: using podman (docker unavailable)" >&2
    return 0
  fi

  _container_runtime_die "neither docker nor podman is available for container builds"
}
