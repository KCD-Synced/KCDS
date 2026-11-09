#!/bin/sh
# KCDS server on Linux: listens on UDP $KCDS_SERVER_PORT (default 7777) on every
# network interface, with the Node.js resource host and the resources next to this script.
#
# Settings come, first to last, from the KCDS_SERVER_* variables, /opt/kcds/server.json (mount
# one there) and the defaults, as for the server packages:
#   KCDS_SERVER_PORT      UDP port (default 7777)
#   KCDS_SERVER_BIND      bind address (default 0.0.0.0 in the container)
#   KCDS_SERVER_LEVEL     level (default trosecko; KCDS_SERVER_MAP, its old name, still works)
#   KCDS_RESOURCES        resources folder (default ./resources)
#   KCDS_NODE             Node.js 22 or newer (default: node on PATH; the image has 26 LTS)
#   KCDS_SERVER_NAME, KCDS_SERVER_PUBLIC_ADDRESS, KCDS_MASTER_URL: server browser listing
#   KCDS_LOG_FILE         a log file besides `docker logs` (default off), e.g. /opt/kcds/logs/server.log,
#                         KCDS_LOG_MAX_MB (20) megabytes per file, KCDS_LOG_FILES (5) files kept
here=$(dirname "$(readlink -f "$0")")
cd "$here" || exit 1
node=${KCDS_NODE:-$(command -v node)}
if [ -z "$node" ]; then
  echo "Node.js 22 or newer is required (26 LTS recommended): https://nodejs.org/" >&2
  exit 1
fi
# A container is reached from outside: every interface unless the settings say otherwise.
if [ -z "$KCDS_SERVER_BIND" ] && ! grep -q '"bind"' "$here/server.json" 2>/dev/null; then
  export KCDS_SERVER_BIND=0.0.0.0
fi
if [ -z "$KCDS_SERVER_LEVEL" ] && [ -n "$KCDS_SERVER_MAP" ]; then
  export KCDS_SERVER_LEVEL="$KCDS_SERVER_MAP"
fi
# The container's output is its log (limit it with Docker's log options); a file only on request.
log_file=${KCDS_LOG_FILE:-off}
log_writer=
if [ "$log_file" != off ]; then
  log_pipe="${TMPDIR:-/tmp}/kcds-log-$$"
  rm -f "$log_pipe"
  if mkfifo "$log_pipe"; then
    "$node" "$here/runtime/Backend/scripting/runtimes/js/logging/log_file.cjs" "$log_file" \
      "${KCDS_LOG_MAX_MB:-20}" "${KCDS_LOG_FILES:-5}" <"$log_pipe" &
    log_writer=$!
    exec >"$log_pipe" 2>&1
    rm -f "$log_pipe"
  fi
fi
# Ends the script once the log has every line of the server.
finish() {
  if [ -n "$log_writer" ]; then
    exec >&- 2>&-
    wait "$log_writer"
  fi
  exit "$1"
}
# A stop signal reaches the server too (its whole group); this script then waits for the server's
# own clean stop (a trap runs only after the foreground command ends) and does not start it again.
stopping=0
trap 'stopping=1' TERM INT
# 75: a restart (KCDS-Admin or KCDS_RESTART_TIMES): the server starts again in the container.
while true; do
  # --config names a file that must be there; without one the defaults and variables apply.
  if [ -f "$here/server.json" ]; then
    set -- --config "$here/server.json"
  else
    set --
  fi
  "$here/kcd2s-server" "$@" --node "$node" \
    --resource-host "$here/runtime/Backend/scripting/runtimes/js/main.cjs" \
    --resources "${KCDS_RESOURCES:-$here/resources}"
  code=$?
  if [ "$stopping" -eq 1 ] || [ "$code" -ne 75 ]; then
    finish "$code"
  fi
done
