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
    exit "$code"
  fi
done
