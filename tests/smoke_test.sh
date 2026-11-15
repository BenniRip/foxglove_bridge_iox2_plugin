#!/bin/bash
# The installed bridge loads the plugin through pluginlib and advertises a running publisher.
#
# Usage: smoke_test.sh <plugin build directory>
set -euo pipefail

BUILD_DIR=${1:?usage: smoke_test.sh <plugin build directory>}
SERVICE="foxglove_bridge_iox2_plugin_smoke/$$"
LOG=$(mktemp)
PUBLISHER=""
BRIDGE=""
cleanup() {
  [ -n "$BRIDGE" ] && kill "$BRIDGE" 2>/dev/null && wait "$BRIDGE" 2>/dev/null
  [ -n "$PUBLISHER" ] && kill "$PUBLISHER" 2>/dev/null && wait "$PUBLISHER" 2>/dev/null
  rm -f "$LOG"
}
trap cleanup EXIT

"$BUILD_DIR/iox2_test_publisher" "$SERVICE" "$BUILD_DIR/test_fbs/test_payload.bfbs" 1 >/dev/null &
PUBLISHER=$!

"$(ros2 pkg prefix foxglove_bridge)/lib/foxglove_bridge/foxglove_bridge" --ros-args \
  -p port:=0 \
  -p channel_source_plugins:="['iox2']" \
  -p iox2.plugin:=foxglove_bridge_iox2_plugin::Iox2Source \
  -p iox2.schema_dir:="$BUILD_DIR/test_fbs" \
  -p iox2.service_names:="['$SERVICE']" \
  -p iox2.topics:="['/smoke']" \
  -p iox2.schema_names:="['foxglove_bridge_iox2_plugin.test.TestPayload']" \
  -p iox2.bfbs_files:="['test_payload.bfbs']" \
  >"$LOG" 2>&1 &
BRIDGE=$!

for _ in $(seq 1 100); do
  grep -q 'Advertising new channel .*"/smoke"' "$LOG" && break
  sleep 0.1
done
cat "$LOG"

grep -q "Loaded channel source plugin instance 'iox2' (foxglove_bridge_iox2_plugin::Iox2Source)" "$LOG"
grep -q 'Advertising new channel .*"/smoke"' "$LOG"
echo "Smoke test passed"
