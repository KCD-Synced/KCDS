#!/bin/sh
# KCDS server {{VERSION}}. Its settings are in server.json next to this file (see README.txt):
# port, bind address, level, name, players, password, KCDS-Admin and the rest. An environment
# variable wins over server.json (KCDS_SERVER_PORT, KCDS_SERVER_NAME, ...), a command line option
# over both (./kcd2s-server --port 7778 ...).
cd "$(dirname "$0")" || exit 1

NODE="$(command -v node)"
if [ -z "$NODE" ]; then
  echo "Node.js 22 or newer is required (26 LTS recommended): https://nodejs.org/"
  exit 1
fi

# The log: the terminal shows everything as before, and logs/server.log keeps it too, never more
# than KCDS_LOG_MAX_MB (20) megabytes per file and KCDS_LOG_FILES (5) files: a full server.log
# becomes server.log.1 and the oldest goes. KCDS_LOG_FILE names another file, "off" writes none.
log_file=${KCDS_LOG_FILE:-logs/server.log}
log_writer=
if [ "$log_file" != off ]; then
  log_pipe="${TMPDIR:-/tmp}/kcds-log-$$"
  rm -f "$log_pipe"
  if mkfifo "$log_pipe"; then
    "$NODE" "$PWD/runtime/Backend/scripting/runtimes/js/logging/log_file.cjs" "$log_file" \
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
# 75: a restart (KCDS-Admin or restartTimes): the server starts again.
while true; do
  ./kcd2s-server --config "$PWD/server.json" --node "$NODE" \
    --resource-host "$PWD/runtime/Backend/scripting/runtimes/js/main.cjs" --resources "$PWD/resources"
  code=$?
  if [ "$stopping" -eq 1 ] || [ "$code" -ne 75 ]; then
    finish "$code"
  fi
done
