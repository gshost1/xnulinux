#!/usr/bin/env bash
# Verification suite for xnu-runnerd.
#   xnu-runnerd-mock-test.sh           local: syntax checks + --dry-run state machine, queue, retry, telemetry,
#                                      signal teardown, GitHub API client against a local fake server
#   xnu-runnerd-mock-test.sh --live    also run 2 real ephemeral CoW jobs against /var/tmp/golden (on the host VM)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
D="$HERE/xnu-runnerd"
LIVE=0
GOLDEN_LIVE=/var/tmp/golden
while [ $# -gt 0 ]; do
  case "$1" in
    --live) LIVE=1; shift;;
    --golden) GOLDEN_LIVE=${2:?}; shift 2;;
    -h|--help) sed -n '2,5p' "$0"; exit 0;;
    *) echo "unknown option: $1" >&2; exit 2;;
  esac
done

T="$(mktemp -d "${TMPDIR:-/tmp}/xnu-runnerd-test.XXXXXX")"
trap 'chmod -R u+w "$T" 2>/dev/null; rm -rf "$T"' EXIT
export PYTHONPYCACHEPREFIX="$T/pycache"
PASS=0
FAIL=0
ok()   { PASS=$((PASS + 1)); echo "ok   - $*"; }
bad()  { FAIL=$((FAIL + 1)); echo "FAIL - $*"; }
check() { local name="$1"; shift; if "$@"; then ok "$name"; else bad "$name"; fi; }

# validate JSONL: $1 file, $2 python expression over `rows` (list of dicts) that must be true
jcheck() {
  python3 - "$1" "$2" <<'PY'
import json, sys
REQUIRED = ["job_id", "timestamp_utc", "mode", "boot_attempts", "boot_s", "job_s", "teardown_s", "total_s",
            "tx_dropped_before", "tx_dropped_after", "tx_dropped_delta", "golden_mutated", "exit_code", "status",
            "guest_ip", "cpus", "ram_mb", "ram_scratch_mb"]
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
for r in rows:
    missing = [k for k in REQUIRED if k not in r]
    if missing:
        print("missing fields:", missing, file=sys.stderr); sys.exit(1)
ok = eval(sys.argv[2], {"rows": rows})
if not ok:
    print(json.dumps(rows, indent=1), file=sys.stderr)
sys.exit(0 if ok else 1)
PY
}

new_env() {  # fresh sandbox: read-only golden, shm, logs
  local d="$T/$1"
  mkdir -p "$d/golden" "$d/shm" "$d/logs" "$d/q"
  truncate -s 64M "$d/golden/mac.img"
  head -c 4096 /dev/urandom > "$d/golden/keystore.bin"
  chmod 444 "$d/golden/mac.img"
  echo "$d"
}
dry() {  # dry <sandbox> args...
  local d="$1"; shift
  python3 "$D" --dry-run --golden "$d/golden" --shm-dir "$d/shm" --log-dir "$d/logs" \
    --telemetry-log "$d/t.jsonl" --boot-timeout 5 "$@"
}
shm_empty() { [ -z "$(ls -A "$1/shm")" ]; }
logto() { local f="$1"; shift; "$@" > "$f" 2>&1; }  # check "name" logto FILE cmd...: output to FILE, verdict to stdout

echo "# syntax"
check "py_compile xnu-runnerd" python3 -m py_compile "$D"
for f in "$HERE"/ci-ephemeral-run.sh "$HERE"/ci-gh-runner-job.sh "$HERE"/ci-multilang-bench.sh "$0"; do
  [ -f "$f" ] && check "bash -n $(basename "$f")" bash -n "$f"
done
COPY="$HERE/../../bench/gh-repo/ci-multilang-bench.sh"
[ -f "$COPY" ] && check "bench/gh-repo/ci-multilang-bench.sh identical" cmp -s "$HERE/ci-multilang-bench.sh" "$COPY"
check "executable bits" test -x "$D" -a -x "$HERE/ci-multilang-bench.sh"
UNIT="$HERE/xnu-runnerd.service"
check "service unit keys" bash -c "grep -q '^Description=xnulinux Ephemeral Headless CI Runner Fleet Daemon$' '$UNIT' &&
  grep -q '^After=network-online.target xnulinux-fki.service$' '$UNIT' && grep -q '^Wants=network-online.target$' '$UNIT' &&
  grep -q '^EnvironmentFile=-/etc/xnu-runnerd.env$' '$UNIT' && grep -q '^Restart=on-failure$' '$UNIT' &&
  grep -q '^RestartSec=5$' '$UNIT' && grep -q '^TimeoutStopSec=45$' '$UNIT'"

echo "# cli"
check "--help lists all modes" bash -c "python3 '$D' --help | grep -q 'mock-queue,github-jit,github-reg'"
check "--help mentions token policy" bash -c "python3 '$D' --help | tr -s ' \n' ' ' | grep -q 'never from the command line'"
check "--token on CLI rejected" bash -c "out=\$(! python3 '$D' --mode github-jit --repo a/b --token x --golden '$T' --dry-run 2>&1) && printf '%s' \"\$out\" | grep -q 'unrecognized arguments: --token'"
check "mock-queue needs a job source" bash -c "! python3 '$D' --mode mock-queue --dry-run 2>/dev/null"

echo "# dry-run: --count 3 with first-boot failure injection"
E="$(new_env count)"
cat > "$E/golden/mac.conf" <<'EOF'
ROOT_UUID=11111111-2222-3333-4444-555555555555
BOOT_UUID=AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE
MACOS=27.2
EOF
printf '#!/bin/bash\nset -e\necho "job $XNU_RUNNERD_JOB_ID scratch=$CI_SCRATCH_DIR args=$*"\ntest "$1" = "arg-one" -a "$2" = "arg two"\ntest -d "$BENCH_WORK" -a -d "$CARGO_TARGET_DIR" -a -d "$GOCACHE"\n' > "$E/job.sh"
G0="$(stat -c '%i %s %Y' "$E/golden/mac.img")"
if XNU_RUNNERD_DRY_FAIL_FIRST=1 dry "$E" --mode mock-queue --job-script "$E/job.sh" --job-args 'arg-one "arg two"' --count 3 > "$E/out" 2>&1; then
  ok "count run exit 0"
else
  bad "count run exit 0"; tail -n 30 "$E/out"
fi
check "job-args passed cleanly" test "$(grep -c 'args=arg-one arg two' "$E/out")" = 3
check "mac.conf UUIDs propagated to boot_env" bash -c "python3 -c '
import importlib.machinery, importlib.util, sys
loader = importlib.machinery.SourceFileLoader(\"xr\", sys.argv[1])
spec = importlib.util.spec_from_loader(\"xr\", loader); m = importlib.util.module_from_spec(spec); loader.exec_module(m)
p = m.build_parser(); a = p.parse_args([\"--mode\", \"mock-queue\", \"--job-script\", sys.argv[2], \"--golden\", sys.argv[3]])
m.validate(a, p); s = m.Supervisor(a, m.DryHost(a)); env = s.boot_env(m.Resources(\"j1\", sys.argv[3]))
assert env[\"ROOT_UUID\"] == \"11111111-2222-3333-4444-555555555555\"
assert env[\"BOOT_UUID\"] == \"AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE\"
' '$D' '$E/job.sh' '$E/golden'"
check "3 telemetry lines" jcheck "$E/t.jsonl" 'len(rows) == 3'
check "all ok, golden_mutated false, tx_dropped_delta 0" jcheck "$E/t.jsonl" \
  'all(r["status"] == "ok" and r["golden_mutated"] is False and r["tx_dropped_delta"] == 0 and r["exit_code"] == 0 for r in rows)'
check "retry: job 1 boot_attempts == 2, others 1" jcheck "$E/t.jsonl" \
  '[r["boot_attempts"] for r in rows] == [2, 1, 1]'
check "scratch env set up" jcheck "$E/t.jsonl" 'all(r["ram_scratch_ok"] is True and r["ram_scratch_mb"] == 1536 for r in rows)'
check "retry logged as early boot failure" grep -q 'boot attempt 1/2 failed: early boot failure' "$E/out"
check "snapshot table format" grep -qE "dmsetup create xnu-runnerd-cow-[0-9-]+ --table '0 131072 snapshot /dev/loop[0-9]+ /dev/loop[0-9]+ N 16'" "$E/out"
check "base loop attached read-only" grep -q -- '--read-only .*golden/mac.img' "$E/out"
check "no /dev/shm leftovers" shm_empty "$E"
check "golden stat unchanged" test "$G0" = "$(stat -c '%i %s %Y' "$E/golden/mac.img")"
check "no sudo/ssh in dry-run" bash -c "! grep -qE '(^| )(sudo|ssh) ' '$E/logs'/*-boot.log"

echo "# dry-run: --queue-dir"
E="$(new_env queue)"
printf '#!/bin/bash\necho a\n' > "$E/q/10-a.sh"
printf '#!/bin/bash\necho b fails\nexit 3\n' > "$E/q/20-b.sh"
printf '#!/bin/bash\necho not executable\n' > "$E/q/30-c.sh"
printf '#!/bin/bash\necho d\n' > "$E/q/40-d.sh"
chmod +x "$E/q/10-a.sh" "$E/q/20-b.sh" "$E/q/40-d.sh"
rc=0; dry "$E" --mode mock-queue --queue-dir "$E/q" --exit-when-empty --ram-scratch-mb 0 > "$E/out" 2>&1 || rc=$?
check "queue run exit 1 (one job failed)" test "$rc" = 1
check "3 jobs consumed" jcheck "$E/t.jsonl" 'len(rows) == 3'
check "statuses ok/job_failed/ok, exit codes" jcheck "$E/t.jsonl" \
  '[(r["job_name"], r["status"], r["exit_code"]) for r in rows] == [("10-a.sh","ok",0),("20-b.sh","job_failed",3),("40-d.sh","ok",0)]'
check "scratch off recorded" jcheck "$E/t.jsonl" 'all(r["ram_scratch_mb"] == 0 and r["ram_scratch_ok"] is None for r in rows)'
check ".done files" test -f "$E/q/10-a.sh.done" -a -f "$E/q/20-b.sh.done" -a -f "$E/q/40-d.sh.done"
check "no .running left" bash -c "! ls '$E/q'/*.running 2>/dev/null"
check "non-executable job left alone" test -f "$E/q/30-c.sh"
check "golden never mutated" jcheck "$E/t.jsonl" 'all(r["golden_mutated"] is False for r in rows)'
check "no /dev/shm leftovers" shm_empty "$E"

echo "# dry-run: boot failure on every attempt"
E="$(new_env bootfail)"
printf '#!/bin/bash\necho never\n' > "$E/job.sh"
rc=0; XNU_RUNNERD_DRY_FAIL_ALL=1 dry "$E" --mode mock-queue --job-script "$E/job.sh" > "$E/out" 2>&1 || rc=$?
check "exit 1" test "$rc" = 1
check "boot_failed after 2 attempts" jcheck "$E/t.jsonl" \
  'len(rows) == 1 and rows[0]["status"] == "boot_failed" and rows[0]["boot_attempts"] == 2 and rows[0]["exit_code"] is None'
check "no /dev/shm leftovers" shm_empty "$E"

echo "# golden protection"
E="$(new_env golden)"
printf '#!/bin/bash\ntrue\n' > "$E/job.sh"
chmod 644 "$E/golden/mac.img"
rc=0; dry "$E" --mode mock-queue --job-script "$E/job.sh" > "$E/out" 2>&1 || rc=$?
check "writable golden refused (exit 2)" test "$rc" = 2
check "refusal message" grep -q 'is writable' "$E/out"
check "--allow-writable-golden accepted" logto "$E/out2" dry "$E" --mode mock-queue --job-script "$E/job.sh" --allow-writable-golden
printf '#!/bin/bash\ntouch "%s"\n' "$E/golden/mac.img" > "$E/mutate.sh"
rc=0; dry "$E" --mode mock-queue --job-script "$E/mutate.sh" --allow-writable-golden --count 3 \
  --telemetry-log "$E/t2.jsonl" > "$E/out3" 2>&1 || rc=$?
check "golden mutation detected and fleet stopped (exit 2)" test "$rc" = 2
check "golden_mutated true, only 1 job ran" jcheck "$E/t2.jsonl" 'len(rows) == 1 and rows[0]["golden_mutated"] is True'

echo "# signals: SIGTERM mid-job tears down"
E="$(new_env sig)"
printf '#!/bin/bash\necho started\nsleep 31.7\n' > "$E/job.sh"
dry "$E" --mode mock-queue --job-script "$E/job.sh" --count 5 > "$E/out" 2>&1 &
pid=$!
for _ in $(seq 1 50); do grep -q started "$E/out" 2>/dev/null && break; sleep 0.1; done
t0=$(date +%s%N)
kill -TERM "$pid"
rc=0; wait "$pid" || rc=$?
dt=$(( ($(date +%s%N) - t0) / 1000000 ))
check "exits 0 within 5s of SIGTERM (${dt}ms, rc=$rc)" test "$dt" -lt 5000 -a "$rc" = 0
check "aborted job recorded" jcheck "$E/t.jsonl" 'len(rows) == 1 and rows[0]["status"] == "aborted"'
check "no /dev/shm leftovers" shm_empty "$E"
sleep 0.3; check "job process killed" bash -c "! pgrep -af 'sleep 31[.]7'"

echo "# reap-stale"
E="$(new_env reap)"
printf '#!/bin/bash\ntrue\n' > "$E/job.sh"
truncate -s 1M "$E/shm/xnu-runnerd-cow-1-2-3.img"; mkdir -p "$E/shm/xnu-runnerd-job-1-2-3"; touch "$E/shm/other-file"
check "reap run ok" logto "$E/out" dry "$E" --mode mock-queue --job-script "$E/job.sh" --reap-stale
check "stale files reaped, foreign file kept" bash -c "[ \"\$(ls -A '$E/shm')\" = other-file ]"

echo "# github-jit / github-reg dry-run"
E="$(new_env gh)"
check "github-jit dry run" logto "$E/out" dry "$E" --mode github-jit --repo octo/demo --max-jobs 2 --no-wait-for-queued
check "github-jit telemetry" jcheck "$E/t.jsonl" 'len(rows) == 2 and all(r["mode"] == "github-jit" and r["status"] == "ok" for r in rows)'
check "jit config never in logs" bash -c "! grep -rq 'DRY-RUN-JIT-CONFIG' '$E/out' '$E/logs'"
check "github-reg dry run" logto "$E/out2" dry "$E" --mode github-reg --repo octo/demo --max-jobs 1 --telemetry-log "$E/t2.jsonl"
check "reg token never in logs" bash -c "! grep -rq 'DRY-RUN-REG-TOKEN' '$E/out2' '$E/logs'"
check "no /dev/shm leftovers" shm_empty "$E"

echo "# GitHub REST client against a local fake API"
if python3 - "$D" > "$T/gh.out" 2>&1 <<'PY'
import http.server, importlib.machinery, importlib.util, io, json, logging, sys, threading
loader = importlib.machinery.SourceFileLoader("xnu_runnerd", sys.argv[1])
spec = importlib.util.spec_from_loader("xnu_runnerd", loader)
m = importlib.util.module_from_spec(spec); loader.exec_module(m)
seen = []
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _send(self, code, obj):
        b = json.dumps(obj).encode(); self.send_response(code)
        self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b)))
        self.end_headers(); self.wfile.write(b)
    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0); body = json.loads(self.rfile.read(n) or b"{}")
        seen.append(("POST", self.path, self.headers, body))
        if self.path.endswith("/generate-jitconfig"):
            self._send(201, {"runner": {"id": 42, "name": body["name"]}, "encoded_jit_config": "SECRETJITCONFIG123"})
        elif self.path.endswith("/registration-token"):
            self._send(201, {"token": "SECRETREGTOKEN456", "expires_at": "x"})
        else:
            self._send(404, {"message": "nope"})
    def do_GET(self):
        seen.append(("GET", self.path, self.headers, None))
        if "/actions/runs?status=queued" in self.path:
            self._send(200, {"total_count": 1, "workflow_runs": [{"id": 7}]})
        elif "/actions/runs?status=in_progress" in self.path:
            self._send(200, {"total_count": 0, "workflow_runs": []})
        elif "/actions/runs/7/jobs" in self.path:
            self._send(200, {"jobs": [{"name": "hosted", "status": "queued", "labels": ["macos-15"]},
                                      {"name": "showdown-self-hosted", "status": "queued",
                                       "labels": ["self-hosted", "macOS", "ARM64"]}]})
        else:
            self._send(404, {"message": "Not Found"})
    def do_DELETE(self):
        seen.append(("DELETE", self.path, self.headers, None)); self.send_response(204); self.end_headers()
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
api = f"http://127.0.0.1:{srv.server_address[1]}"
buf = io.StringIO(); h = logging.StreamHandler(buf); h.addFilter(m.REDACTOR)
m.log.addHandler(h); m.log.setLevel(logging.DEBUG)
gh = m.GitHub("octo/demo", "ghp_FAKETOKEN_abcdef", api)
jit, rid = gh.jit_config("xnu-ci-host-1", ["self-hosted", "macOS", "ARM64"])
assert (jit, rid) == ("SECRETJITCONFIG123", 42), (jit, rid)
meth, path, hdr, body = seen[-1]
assert path == "/repos/octo/demo/actions/runners/generate-jitconfig", path
assert body == {"name": "xnu-ci-host-1", "runner_group_id": 1, "labels": ["self-hosted", "macOS", "ARM64"],
                "work_folder": "_work"}, body
assert hdr["Accept"] == "application/vnd.github+json" and hdr["X-GitHub-Api-Version"] == "2022-11-28", hdr
assert hdr["Authorization"] == "Bearer ghp_FAKETOKEN_abcdef"
assert gh.registration_token() == "SECRETREGTOKEN456"
assert seen[-1][1] == "/repos/octo/demo/actions/runners/registration-token"
found = gh.queued_job_for(["self-hosted", "macOS", "ARM64"])
assert found and "showdown-self-hosted" in found, found
assert gh.queued_job_for(["self-hosted", "Linux"]) is None
gh.delete_runner(42)
assert seen[-1][:2] == ("DELETE", "/repos/octo/demo/actions/runners/42")
try:
    gh.req("GET", "/missing"); raise SystemExit("expected 404")
except m.GitHubError as e:
    assert e.code == 404 and "Not Found" in str(e)
m.log.info("token ghp_FAKETOKEN_abcdef jit SECRETJITCONFIG123 reg SECRETREGTOKEN456")
out = buf.getvalue()
assert "FAKETOKEN" not in out and "SECRETJIT" not in out and "SECRETREG" not in out, out
import os; os.environ["GITHUB_TOKEN"] = "leak"; assert "GITHUB_TOKEN" not in m.child_env()
print("github client ok")
PY
then ok "REST endpoints, headers, JIT body, queued-label match, redaction, child env scrub"; else bad "GitHub client"; cat "$T/gh.out"; fi

if [ "$LIVE" = 1 ]; then
  echo "# live: 2 real ephemeral CoW jobs on $GOLDEN_LIVE"
  L="$T/live"; mkdir -p "$L"
  cat > "$L/job.sh" <<'EOF'
#!/bin/bash
set -euo pipefail
M="/Users/$USER/.ci-ephemeral-dirty-marker"
if [ -e "$M" ]; then echo "LEAK: $M survived from a previous job"; exit 42; fi
sw_vers; uname -m; sysctl -n hw.ncpu
echo "scratch=${CI_SCRATCH_DIR:-none}"; df -h "${CI_SCRATCH_DIR:-/}" | tail -n 1
curl -fsS --max-time 20 https://api.github.com/zen && echo
echo dirty > "$M"
echo LIVE_JOB_OK
EOF
  set +e
  python3 "$D" --mode mock-queue --job-script "$L/job.sh" --count 2 --golden "$GOLDEN_LIVE" \
    --telemetry-log "$L/t.jsonl" --log-dir "$L/logs" --reap-stale 2>&1 | tee "$L/out"
  rc=${PIPESTATUS[0]}
  set -e
  check "live run exit 0" test "$rc" = 0
  check "live: 2 jobs ok, golden unchanged" jcheck "$L/t.jsonl" \
    'len(rows) == 2 and all(r["status"] == "ok" and r["golden_mutated"] is False and r["dry_run"] is False for r in rows)'
  check "live: marker isolation (job 2 saw no marker)" bash -c "[ \$(grep -c LIVE_JOB_OK '$L/out') = 2 ]"
  check "live: no xnu-runnerd dm devices left" bash -c "! sudo -n dmsetup ls | grep -q xnu-runnerd-cow-"
  check "live: no /dev/shm/xnu-runnerd-* left" bash -c "! ls /dev/shm/xnu-runnerd-* 2>/dev/null"
  check "live: xnu.ko unloaded" bash -c "! grep -q '^xnu ' /proc/modules"
  python3 - "$L/t.jsonl" <<'PY'
import json, sys
for r in map(json.loads, open(sys.argv[1])):
    print(f"  {r['job_id']}: boot {r['boot_s']}s x{r['boot_attempts']} job {r['job_s']}s teardown {r['teardown_s']}s "
          f"tx_dropped_delta {r['tx_dropped_delta']} scratch_ok {r['ram_scratch_ok']}")
    if r["teardown_s"] is not None and r["teardown_s"] >= 1.0:
        print(f"  note: teardown {r['teardown_s']}s is above the 1s target")
PY
fi

echo "# result: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
