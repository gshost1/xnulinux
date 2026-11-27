#!/bin/bash
set -euo pipefail
echo "=== Ephemeral CI Job Hostname & Uptime ==="
hostname
uptime
cd ~/actions-runner
echo "RUNNER_VER=$(./bin/Runner.Listener --version)"
./externals/node20/bin/node -e "
  const { execSync } = require('child_process');
  console.log('Node:', process.version, process.arch);
  console.log(execSync('clang --version | head -n 1', {encoding:'utf8'}).trim());
  console.log(execSync('swift --version | head -n 1', {encoding:'utf8'}).trim());
  execSync('cd ~/t/hello && swift test', {stdio:'inherit'});
"
CE_MARKER="/var/tmp/ephemeral_cow_marker_$(date +%s).txt"
echo "dirty-cow-write" > "$CE_MARKER"
echo "Wrote ephemeral marker $CE_MARKER (must not persist to next CoW boot)"
ls -la "$CE_MARKER"
