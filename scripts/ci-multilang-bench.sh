#!/usr/bin/env bash
# Self-contained multi-language macOS CI benchmark and test suite.
# Runs the same way on a self-hosted headless macOS runner and on GitHub-hosted macos-15.
# Every project is generated here; nothing is downloaded from a package registry.
#
# Usage: ci-multilang-bench.sh [LABEL] [--ephemeral-check] [--strict]
#   --ephemeral-check  fail at once if ~/.ci-ephemeral-dirty-marker exists (leaked from a previous job),
#                      and create it at the end (the next copy-on-write boot must not see it)
#   --strict           a missing toolchain is a failure instead of a SKIP
# Output: INFO|key|value, METRIC|name|ms, SKIP|name|reason, FAIL|name|detail. Exit 1 if any FAIL.
set -euo pipefail

LABEL="unknown"
EPHEMERAL_CHECK=0
STRICT="${CI_BENCH_STRICT:-0}"
for arg in "$@"; do
  case "$arg" in
    --ephemeral-check) EPHEMERAL_CHECK=1;;
    --strict) STRICT=1;;
    -h|--help) sed -n '2,11p' "$0"; exit 0;;
    -*) echo "unknown option: $arg" >&2; exit 2;;
    *) LABEL="$arg";;
  esac
done

# Non-login SSH shells get a minimal PATH: add the usual toolchain locations that exist.
for d in /usr/local/go/bin "$HOME/.cargo/bin" /opt/homebrew/bin /usr/local/bin; do
  if [ -d "$d" ]; then
    case ":$PATH:" in *":$d:"*) ;; *) PATH="$PATH:$d";; esac
  fi
done
export PATH

OS="$(uname -s)"
ARCH="$(uname -m)"
USER="${USER:-$(id -un)}"
if [ "$OS" = Darwin ]; then
  MARKER="/Users/$USER/.ci-ephemeral-dirty-marker"
  NCPUS="$(sysctl -n hw.ncpu)"
  RAM_GB="$(awk -v b="$(sysctl -n hw.memsize)" 'BEGIN { printf "%.2f", b/1073741824 }')"
  OS_VER="$(sw_vers -productName) $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
  CPU_NAME="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
else
  MARKER="$HOME/.ci-ephemeral-dirty-marker"
  NCPUS="$(nproc)"
  RAM_GB="$(awk '/MemTotal/ { printf "%.2f", $2/1048576 }' /proc/meminfo)"
  OS_VER="Linux $(uname -r)"
  CPU_NAME="$(awk -F: '/model name|^CPU part/ { gsub(/^ +/, "", $2); print $2; exit }' /proc/cpuinfo 2>/dev/null || echo unknown)"
fi

if [ "$EPHEMERAL_CHECK" = 1 ] && [ -e "$MARKER" ]; then
  echo "FAIL|ephemeral_check|$MARKER exists: state leaked from a previous job"
  exit 3
fi

WORK="${BENCH_WORK:-/tmp/ci-multilang-work}"
rm -rf "$WORK"
mkdir -p "$WORK/logs"
cd "$WORK"
if [ -n "${CI_SCRATCH_DIR:-}" ]; then SCRATCH_MODE="ram:$CI_SCRATCH_DIR"; else SCRATCH_MODE="disk"; fi

FAILED=0
have() { command -v "$1" >/dev/null 2>&1; }
info() { printf 'INFO|%s|%s\n' "$1" "$(printf '%s' "$2" | tr '|\n' '/ ' | sed 's/ *$//')"; }
ver() {  # ver KEY CMD...: first line of the version, or "missing"
  local k="$1"; shift
  if have "$1"; then info "$k" "$("$@" 2>&1 | head -n 1)"; else info "$k" missing; fi
}
skip() {  # skip METRIC REASON
  if [ "$STRICT" = 1 ]; then
    echo "FAIL|$1|$2 (strict)"; FAILED=1
  else
    echo "SKIP|$1|$2"
  fi
}
time_cmd() {  # time_cmd METRIC CMD...: METRIC|name|ms, or FAIL with the log tail
  local name="$1"; shift
  local logf="$WORK/logs/$name.log" dt rc=0
  dt="$(python3 - "$logf" "$@" <<'PY'
import subprocess, sys, time
t0 = time.perf_counter()
with open(sys.argv[1], "wb") as f:
    r = subprocess.run(sys.argv[2:], stdout=f, stderr=subprocess.STDOUT)
dt = int((time.perf_counter() - t0) * 1000)
print(dt)
sys.exit(r.returncode)
PY
)" || rc=$?
  if [ "$rc" = 0 ]; then
    printf 'METRIC|%s|%d\n' "$name" "$dt"
  else
    echo "FAIL|$name|exit $rc after ${dt:-?}ms"
    tail -n 40 "$logf" | sed 's/^/    /'
    FAILED=1
    return 1
  fi
}

# node: PATH first, else the one shipped with actions/runner (no global node on a bare runner image)
NODE=""
if have node; then
  NODE="$(command -v node)"
else
  for n in $(ls -d "$HOME"/actions-runner/externals/node*/bin/node 2>/dev/null | sort -V -r); do
    [ -x "$n" ] && { NODE="$n"; break; }
  done
fi

T_ALL0="$(python3 -c 'import time; print(time.time())')"
echo "=== CI MULTI-LANGUAGE BENCHMARK: $LABEL ==="
info label "$LABEL"
info os "$OS_VER"
info arch "$ARCH"
info cpus "$NCPUS"
info ram_gb "$RAM_GB"
info cpu_name "$CPU_NAME"
info scratch_mode "$SCRATCH_MODE"
ver swift swift --version
ver clang clang --version
if [ "$OS" = Darwin ] && have xcodebuild && xcodebuild -version >/dev/null 2>&1; then
  info xcode "$(xcodebuild -version 2>/dev/null | tr '\n' ' ')"
else
  info xcode missing
fi
ver rustc rustc --version
ver cargo cargo --version
ver go go version
ver cmake cmake --version
ver ninja ninja --version
if [ -n "$NODE" ]; then info node "$("$NODE" --version)"; else info node missing; fi
ver python3 python3 --version
[ "$EPHEMERAL_CHECK" = 1 ] && info ephemeral_check "clean (no marker at start)"

# ---------------------------------------------------------------------------
# 1. SwiftPM: Core library + CLI executable + XCTest suite (concurrency, algorithms, JSON)
# ---------------------------------------------------------------------------
if have swift; then
  P="$WORK/swiftpm"
  mkdir -p "$P/Sources/Core" "$P/Sources/ci-cli" "$P/Tests/CoreTests"
  cat > "$P/Package.swift" <<'EOF'
// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "CIMulti",
    platforms: [.macOS(.v13)],
    products: [
        .library(name: "Core", targets: ["Core"]),
        .executable(name: "ci-cli", targets: ["ci-cli"]),
    ],
    targets: [
        .target(name: "Core"),
        .executableTarget(name: "ci-cli", dependencies: ["Core"]),
        .testTarget(name: "CoreTests", dependencies: ["Core"]),
    ]
)
EOF
  cat > "$P/Sources/Core/Algorithms.swift" <<'EOF'
public enum Algorithms {
    /// Primes below n (sieve of Eratosthenes).
    public static func primes(below n: Int) -> [Int] {
        guard n > 2 else { return [] }
        var sieve = [Bool](repeating: true, count: n)
        sieve[0] = false; sieve[1] = false
        var i = 2
        while i * i < n {
            if sieve[i] { var j = i * i; while j < n { sieve[j] = false; j += i } }
            i += 1
        }
        return sieve.indices.filter { sieve[$0] }
    }

    public static func mergeSort<T: Comparable>(_ a: [T]) -> [T] {
        guard a.count > 1 else { return a }
        let mid = a.count / 2
        let l = mergeSort(Array(a[..<mid])), r = mergeSort(Array(a[mid...]))
        var out: [T] = []; out.reserveCapacity(a.count)
        var i = 0, j = 0
        while i < l.count && j < r.count { if l[i] <= r[j] { out.append(l[i]); i += 1 } else { out.append(r[j]); j += 1 } }
        out.append(contentsOf: l[i...]); out.append(contentsOf: r[j...])
        return out
    }

    /// 64-bit FNV-1a.
    public static func fnv1a(_ bytes: [UInt8]) -> UInt64 {
        var h: UInt64 = 0xcbf29ce484222325
        for b in bytes { h ^= UInt64(b); h = h &* 0x100000001b3 }
        return h
    }

    public static func levenshtein(_ a: String, _ b: String) -> Int {
        let x = Array(a), y = Array(b)
        if x.isEmpty { return y.count }
        if y.isEmpty { return x.count }
        var prev = Array(0...y.count)
        for i in 1...x.count {
            var cur = [i] + [Int](repeating: 0, count: y.count)
            for j in 1...y.count {
                cur[j] = Swift.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (x[i - 1] == y[j - 1] ? 0 : 1))
            }
            prev = cur
        }
        return prev[y.count]
    }
}
EOF
  cat > "$P/Sources/Core/Concurrency.swift" <<'EOF'
public actor Counter {
    private var value = 0
    public init() {}
    public func add(_ n: Int) { value += n }
    public func get() -> Int { value }
}

public enum Parallel {
    /// Sum of f(i) for i in 0..<n, computed in `chunks` child tasks.
    public static func sum(_ n: Int, chunks: Int, _ f: @escaping @Sendable (Int) -> Int) async -> Int {
        await withTaskGroup(of: Int.self) { group in
            let step = (n + chunks - 1) / chunks
            for c in 0..<chunks {
                let lo = c * step, hi = Swift.min(n, lo + step)
                if lo >= hi { continue }
                group.addTask { var s = 0; for i in lo..<hi { s &+= f(i) }; return s }
            }
            var total = 0
            for await part in group { total &+= part }
            return total
        }
    }
}
EOF
  cat > "$P/Sources/Core/Model.swift" <<'EOF'
import Foundation

public struct BuildRecord: Codable, Equatable, Sendable {
    public var name: String
    public var durationMs: Int
    public var tags: [String]
    public var ok: Bool
    public init(name: String, durationMs: Int, tags: [String], ok: Bool) {
        self.name = name; self.durationMs = durationMs; self.tags = tags; self.ok = ok
    }
}

public enum Codec {
    public static func encode(_ r: [BuildRecord]) throws -> Data {
        let e = JSONEncoder(); e.outputFormatting = [.sortedKeys]
        return try e.encode(r)
    }
    public static func decode(_ d: Data) throws -> [BuildRecord] { try JSONDecoder().decode([BuildRecord].self, from: d) }
}
EOF
  cat > "$P/Sources/ci-cli/main.swift" <<'EOF'
import Core
import Foundation

let primes = Algorithms.primes(below: 100_000)
let recs = (0..<64).map { BuildRecord(name: "job\($0)", durationMs: $0 * 7, tags: ["ci", "swift"], ok: $0 % 5 != 0) }
let data = try Codec.encode(recs)
let back = try Codec.decode(data)
precondition(back == recs, "JSON round trip")
print("SWIFTPM_CLI_OK primes=\(primes.count) json_bytes=\(data.count) fnv=\(String(Algorithms.fnv1a(Array(data)), radix: 16))")
EOF
  cat > "$P/Tests/CoreTests/CoreTests.swift" <<'EOF'
import XCTest
@testable import Core

final class AlgorithmTests: XCTestCase {
    func testPrimes() {
        XCTAssertEqual(Algorithms.primes(below: 30), [2, 3, 5, 7, 11, 13, 17, 19, 23, 29])
        XCTAssertEqual(Algorithms.primes(below: 1_000_000).count, 78_498)
    }
    func testMergeSort() {
        var g = SystemRandomNumberGenerator()
        let a = (0..<20_000).map { _ in Int.random(in: -1_000_000...1_000_000, using: &g) }
        XCTAssertEqual(Algorithms.mergeSort(a), a.sorted())
    }
    func testFNV() {
        XCTAssertEqual(Algorithms.fnv1a(Array("".utf8)), 0xcbf29ce484222325)
        XCTAssertEqual(Algorithms.fnv1a(Array("a".utf8)), 0xaf63dc4c8601ec8c)
    }
    func testLevenshtein() {
        XCTAssertEqual(Algorithms.levenshtein("kitten", "sitting"), 3)
        XCTAssertEqual(Algorithms.levenshtein("", "abc"), 3)
        XCTAssertEqual(Algorithms.levenshtein("swift", "swift"), 0)
    }
}

final class ConcurrencyTests: XCTestCase {
    func testTaskGroupSum() async {
        let n = 2_000_000
        let s = await Parallel.sum(n, chunks: 16) { $0 }
        XCTAssertEqual(s, n * (n - 1) / 2)
    }
    func testActorCounter() async {
        let c = Counter()
        await withTaskGroup(of: Void.self) { g in
            for i in 1...1000 { g.addTask { await c.add(i) } }
        }
        let v = await c.get()
        XCTAssertEqual(v, 500_500)
    }
}

final class JSONTests: XCTestCase {
    func testRoundTrip() throws {
        let recs = (0..<500).map { BuildRecord(name: "r\($0)", durationMs: $0, tags: ["a", "b\($0 % 3)"], ok: $0 % 2 == 0) }
        let d = try Codec.encode(recs)
        XCTAssertEqual(try Codec.decode(d), recs)
        let obj = try JSONSerialization.jsonObject(with: d) as? [[String: Any]]
        XCTAssertEqual(obj?.count, 500)
        XCTAssertEqual(obj?.first?["name"] as? String, "r0")
    }
}
EOF
  if time_cmd swiftpm_build_ms bash -c "cd '$P' && swift build -j $NCPUS"; then
    out="$("$P/.build/debug/ci-cli" 2>&1 || true)"
    case "$out" in SWIFTPM_CLI_OK*) echo "INFO|swiftpm_cli|$out";; *) echo "FAIL|swiftpm_cli|$out"; FAILED=1;; esac
  fi
  time_cmd swiftpm_test_ms bash -c "cd '$P' && swift test -j $NCPUS" || true
else
  skip swiftpm_build_ms "swift not found"
  skip swiftpm_test_ms "swift not found"
fi

# ---------------------------------------------------------------------------
# 2. Headless xcodebuild: generated HelloCI.xcodeproj (Swift command-line tool, 2 files, shared scheme)
# ---------------------------------------------------------------------------
if [ "$OS" = Darwin ] && have xcodebuild && xcodebuild -version >/dev/null 2>&1; then
  X="$WORK/HelloCI"
  mkdir -p "$X/HelloCI" "$X/HelloCI.xcodeproj/xcshareddata/xcschemes"
  cat > "$X/HelloCI/main.swift" <<'EOF'
let g = Greeter(name: "xcodebuild")
print(g.token())
EOF
  cat > "$X/HelloCI/Greeter.swift" <<'EOF'
struct Greeter {
    let name: String
    func token() -> String {
        let checksum = name.unicodeScalars.reduce(UInt32(0)) { ($0 &* 31) &+ $1.value }
        precondition(checksum != 0)
        return "HelloCI_\(name)_OK"
    }
}
EOF
  cat > "$X/HelloCI.xcodeproj/project.pbxproj" <<'EOF'
// !$*UTF8*$!
{
	archiveVersion = 1;
	classes = {
	};
	objectVersion = 56;
	objects = {

/* Begin PBXBuildFile section */
		C1A000000000000000000001 /* main.swift in Sources */ = {isa = PBXBuildFile; fileRef = C1A000000000000000000011 /* main.swift */; };
		C1A000000000000000000002 /* Greeter.swift in Sources */ = {isa = PBXBuildFile; fileRef = C1A000000000000000000012 /* Greeter.swift */; };
/* End PBXBuildFile section */

/* Begin PBXFileReference section */
		C1A000000000000000000011 /* main.swift */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.swift; path = main.swift; sourceTree = "<group>"; };
		C1A000000000000000000012 /* Greeter.swift */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.swift; path = Greeter.swift; sourceTree = "<group>"; };
		C1A000000000000000000013 /* HelloCI */ = {isa = PBXFileReference; explicitFileType = "compiled.mach-o.executable"; includeInIndex = 0; path = HelloCI; sourceTree = BUILT_PRODUCTS_DIR; };
/* End PBXFileReference section */

/* Begin PBXFrameworksBuildPhase section */
		C1A000000000000000000021 /* Frameworks */ = {
			isa = PBXFrameworksBuildPhase;
			buildActionMask = 2147483647;
			files = (
			);
			runOnlyForDeploymentPostprocessing = 0;
		};
/* End PBXFrameworksBuildPhase section */

/* Begin PBXGroup section */
		C1A000000000000000000031 = {
			isa = PBXGroup;
			children = (
				C1A000000000000000000032 /* HelloCI */,
				C1A000000000000000000033 /* Products */,
			);
			sourceTree = "<group>";
		};
		C1A000000000000000000032 /* HelloCI */ = {
			isa = PBXGroup;
			children = (
				C1A000000000000000000011 /* main.swift */,
				C1A000000000000000000012 /* Greeter.swift */,
			);
			path = HelloCI;
			sourceTree = "<group>";
		};
		C1A000000000000000000033 /* Products */ = {
			isa = PBXGroup;
			children = (
				C1A000000000000000000013 /* HelloCI */,
			);
			name = Products;
			sourceTree = "<group>";
		};
/* End PBXGroup section */

/* Begin PBXNativeTarget section */
		C1A000000000000000000041 /* HelloCI */ = {
			isa = PBXNativeTarget;
			buildConfigurationList = C1A000000000000000000061 /* Build configuration list for PBXNativeTarget "HelloCI" */;
			buildPhases = (
				C1A000000000000000000051 /* Sources */,
				C1A000000000000000000021 /* Frameworks */,
			);
			buildRules = (
			);
			dependencies = (
			);
			name = HelloCI;
			productName = HelloCI;
			productReference = C1A000000000000000000013 /* HelloCI */;
			productType = "com.apple.product-type.tool";
		};
/* End PBXNativeTarget section */

/* Begin PBXProject section */
		C1A000000000000000000042 /* Project object */ = {
			isa = PBXProject;
			attributes = {
				BuildIndependentTargetsInParallel = 1;
				LastSwiftUpdateCheck = 1500;
				LastUpgradeCheck = 1500;
			};
			buildConfigurationList = C1A000000000000000000062 /* Build configuration list for PBXProject "HelloCI" */;
			compatibilityVersion = "Xcode 14.0";
			developmentRegion = en;
			hasScannedForEncodings = 0;
			knownRegions = (
				en,
				Base,
			);
			mainGroup = C1A000000000000000000031;
			productRefGroup = C1A000000000000000000033 /* Products */;
			projectDirPath = "";
			projectRoot = "";
			targets = (
				C1A000000000000000000041 /* HelloCI */,
			);
		};
/* End PBXProject section */

/* Begin PBXSourcesBuildPhase section */
		C1A000000000000000000051 /* Sources */ = {
			isa = PBXSourcesBuildPhase;
			buildActionMask = 2147483647;
			files = (
				C1A000000000000000000001 /* main.swift in Sources */,
				C1A000000000000000000002 /* Greeter.swift in Sources */,
			);
			runOnlyForDeploymentPostprocessing = 0;
		};
/* End PBXSourcesBuildPhase section */

/* Begin XCBuildConfiguration section */
		C1A000000000000000000071 /* Debug */ = {
			isa = XCBuildConfiguration;
			buildSettings = {
				CODE_SIGNING_ALLOWED = NO;
				CODE_SIGN_IDENTITY = "";
				DEBUG_INFORMATION_FORMAT = dwarf;
				MACOSX_DEPLOYMENT_TARGET = 13.0;
				ONLY_ACTIVE_ARCH = YES;
				SDKROOT = macosx;
				SWIFT_OPTIMIZATION_LEVEL = "-Onone";
				SWIFT_VERSION = 5.0;
			};
			name = Debug;
		};
		C1A000000000000000000072 /* Release */ = {
			isa = XCBuildConfiguration;
			buildSettings = {
				CODE_SIGNING_ALLOWED = NO;
				CODE_SIGN_IDENTITY = "";
				DEBUG_INFORMATION_FORMAT = dwarf;
				MACOSX_DEPLOYMENT_TARGET = 13.0;
				ONLY_ACTIVE_ARCH = YES;
				SDKROOT = macosx;
				SWIFT_COMPILATION_MODE = wholemodule;
				SWIFT_OPTIMIZATION_LEVEL = "-O";
				SWIFT_VERSION = 5.0;
			};
			name = Release;
		};
		C1A000000000000000000073 /* Debug */ = {
			isa = XCBuildConfiguration;
			buildSettings = {
				ENABLE_HARDENED_RUNTIME = NO;
				PRODUCT_NAME = "$(TARGET_NAME)";
			};
			name = Debug;
		};
		C1A000000000000000000074 /* Release */ = {
			isa = XCBuildConfiguration;
			buildSettings = {
				ENABLE_HARDENED_RUNTIME = NO;
				PRODUCT_NAME = "$(TARGET_NAME)";
			};
			name = Release;
		};
/* End XCBuildConfiguration section */

/* Begin XCConfigurationList section */
		C1A000000000000000000061 /* Build configuration list for PBXNativeTarget "HelloCI" */ = {
			isa = XCConfigurationList;
			buildConfigurations = (
				C1A000000000000000000073 /* Debug */,
				C1A000000000000000000074 /* Release */,
			);
			defaultConfigurationIsVisible = 0;
			defaultConfigurationName = Release;
		};
		C1A000000000000000000062 /* Build configuration list for PBXProject "HelloCI" */ = {
			isa = XCConfigurationList;
			buildConfigurations = (
				C1A000000000000000000071 /* Debug */,
				C1A000000000000000000072 /* Release */,
			);
			defaultConfigurationIsVisible = 0;
			defaultConfigurationName = Release;
		};
/* End XCConfigurationList section */
	};
	rootObject = C1A000000000000000000042 /* Project object */;
}
EOF
  cat > "$X/HelloCI.xcodeproj/xcshareddata/xcschemes/HelloCI.xcscheme" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<Scheme
   LastUpgradeVersion = "1500"
   version = "1.7">
   <BuildAction
      parallelizeBuildables = "YES"
      buildImplicitDependencies = "YES">
      <BuildActionEntries>
         <BuildActionEntry
            buildForTesting = "YES"
            buildForRunning = "YES"
            buildForProfiling = "YES"
            buildForArchiving = "YES"
            buildForAnalyzing = "YES">
            <BuildableReference
               BuildableIdentifier = "primary"
               BlueprintIdentifier = "C1A000000000000000000041"
               BuildableName = "HelloCI"
               BlueprintName = "HelloCI"
               ReferencedContainer = "container:HelloCI.xcodeproj">
            </BuildableReference>
         </BuildActionEntry>
      </BuildActionEntries>
   </BuildAction>
   <LaunchAction
      buildConfiguration = "Release"
      selectedDebuggerIdentifier = ""
      selectedLauncherIdentifier = "Xcode.IDEFoundation.Launcher.PosixSpawn"
      launchStyle = "0"
      useCustomWorkingDirectory = "NO"
      ignoresPersistentStateOnLaunch = "NO"
      debugDocumentVersioning = "YES"
      debugServiceExtension = "internal"
      allowLocationSimulation = "YES">
      <BuildableProductRunnable
         runnableDebuggingMode = "0">
         <BuildableReference
            BuildableIdentifier = "primary"
            BlueprintIdentifier = "C1A000000000000000000041"
            BuildableName = "HelloCI"
            BlueprintName = "HelloCI"
            ReferencedContainer = "container:HelloCI.xcodeproj">
         </BuildableReference>
      </BuildableProductRunnable>
   </LaunchAction>
</Scheme>
EOF
  if time_cmd xcodebuild_ms bash -c "cd '$X' && xcodebuild -project HelloCI.xcodeproj -scheme HelloCI -configuration Release CODE_SIGNING_ALLOWED=NO SYMROOT='$X/build' build"; then
    out="$("$X/build/Release/HelloCI" 2>&1 || true)"
    if [ "$out" = HelloCI_xcodebuild_OK ]; then
      echo "INFO|xcodebuild_run|$out"
    else
      echo "FAIL|xcodebuild_run|unexpected output: $out"; FAILED=1
    fi
  fi
elif [ "$OS" = Darwin ]; then
  echo "FAIL|xcodebuild_ms|xcodebuild unavailable on macOS"; FAILED=1
else
  echo "SKIP|xcodebuild_ms|not macOS"
fi

# ---------------------------------------------------------------------------
# 3. Rust: multi-module crate (lib + hash + parser + bin + integration tests), no dependencies
# ---------------------------------------------------------------------------
if have cargo && have rustc; then
  R="$WORK/rust"
  mkdir -p "$R/src" "$R/tests"
  RUST_TARGET="${CARGO_TARGET_DIR:-$WORK/rust-target}/ci-multilang"
  rm -rf "$RUST_TARGET"
  cat > "$R/Cargo.toml" <<'EOF'
[package]
name = "ci_rust"
version = "0.1.0"
edition = "2021"

[dependencies]

[profile.release]
codegen-units = 16
EOF
  cat > "$R/src/lib.rs" <<'EOF'
pub mod hash;
pub mod parser;

use std::thread;

/// Hash `n` generated records on `threads` threads; returns the XOR of all FNV-1a hashes.
pub fn parallel_digest(n: usize, threads: usize) -> u64 {
    let threads = threads.max(1);
    let chunk = n.div_ceil(threads);
    thread::scope(|s| {
        let handles: Vec<_> = (0..threads)
            .map(|t| {
                s.spawn(move || {
                    let lo = t * chunk;
                    let hi = (lo + chunk).min(n);
                    (lo..hi).fold(0u64, |acc, i| acc ^ hash::fnv1a(format!("record-{i}").as_bytes()))
                })
            })
            .collect();
        handles.into_iter().map(|h| h.join().unwrap()).fold(0, |a, b| a ^ b)
    })
}

pub fn primes_below(n: usize) -> Vec<usize> {
    if n < 3 {
        return vec![];
    }
    let mut sieve = vec![true; n];
    sieve[0] = false;
    sieve[1] = false;
    let mut i = 2;
    while i * i < n {
        if sieve[i] {
            (i * i..n).step_by(i).for_each(|j| sieve[j] = false);
        }
        i += 1;
    }
    sieve.iter().enumerate().filter(|&(_, &p)| p).map(|(i, _)| i).collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn digest_is_thread_count_independent() {
        assert_eq!(parallel_digest(10_000, 1), parallel_digest(10_000, 8));
    }
    #[test]
    fn primes() {
        assert_eq!(primes_below(30), vec![2, 3, 5, 7, 11, 13, 17, 19, 23, 29]);
        assert_eq!(primes_below(1_000_000).len(), 78_498);
    }
}
EOF
  cat > "$R/src/hash.rs" <<'EOF'
/// 64-bit FNV-1a.
pub fn fnv1a(data: &[u8]) -> u64 {
    data.iter().fold(0xcbf29ce484222325u64, |h, &b| (h ^ b as u64).wrapping_mul(0x100000001b3))
}

/// CRC-32 (IEEE 802.3), table-driven.
pub fn crc32(data: &[u8]) -> u32 {
    let mut table = [0u32; 256];
    for (i, e) in table.iter_mut().enumerate() {
        let mut c = i as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { 0xEDB88320 ^ (c >> 1) } else { c >> 1 };
        }
        *e = c;
    }
    !data.iter().fold(!0u32, |c, &b| table[((c ^ b as u32) & 0xff) as usize] ^ (c >> 8))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn known_vectors() {
        assert_eq!(fnv1a(b""), 0xcbf29ce484222325);
        assert_eq!(fnv1a(b"a"), 0xaf63dc4c8601ec8c);
        assert_eq!(crc32(b"123456789"), 0xCBF43926);
    }
}
EOF
  cat > "$R/src/parser.rs" <<'EOF'
//! Recursive-descent parser and evaluator for integer arithmetic: + - * / % and parentheses.

#[derive(Debug, PartialEq)]
pub enum Error {
    Unexpected(usize),
    DivZero,
    End,
}

pub fn eval(src: &str) -> Result<i64, Error> {
    let toks: Vec<char> = src.chars().filter(|c| !c.is_whitespace()).collect();
    let mut p = Parser { t: &toks, i: 0 };
    let v = p.expr()?;
    if p.i != toks.len() {
        return Err(Error::Unexpected(p.i));
    }
    Ok(v)
}

struct Parser<'a> {
    t: &'a [char],
    i: usize,
}

impl Parser<'_> {
    fn peek(&self) -> Option<char> {
        self.t.get(self.i).copied()
    }
    fn expr(&mut self) -> Result<i64, Error> {
        let mut v = self.term()?;
        while let Some(op @ ('+' | '-')) = self.peek() {
            self.i += 1;
            let r = self.term()?;
            v = if op == '+' { v + r } else { v - r };
        }
        Ok(v)
    }
    fn term(&mut self) -> Result<i64, Error> {
        let mut v = self.atom()?;
        while let Some(op @ ('*' | '/' | '%')) = self.peek() {
            self.i += 1;
            let r = self.atom()?;
            v = match op {
                '*' => v * r,
                _ if r == 0 => return Err(Error::DivZero),
                '/' => v / r,
                _ => v % r,
            };
        }
        Ok(v)
    }
    fn atom(&mut self) -> Result<i64, Error> {
        match self.peek() {
            None => Err(Error::End),
            Some('(') => {
                self.i += 1;
                let v = self.expr()?;
                if self.peek() != Some(')') {
                    return Err(Error::Unexpected(self.i));
                }
                self.i += 1;
                Ok(v)
            }
            Some('-') => {
                self.i += 1;
                Ok(-self.atom()?)
            }
            Some(c) if c.is_ascii_digit() => {
                let mut v = 0i64;
                while let Some(d) = self.peek().and_then(|c| c.to_digit(10)) {
                    v = v * 10 + d as i64;
                    self.i += 1;
                }
                Ok(v)
            }
            Some(_) => Err(Error::Unexpected(self.i)),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn precedence() {
        assert_eq!(eval("1 + 2 * 3"), Ok(7));
        assert_eq!(eval("(1 + 2) * 3"), Ok(9));
        assert_eq!(eval("-(4 - 10) % 4"), Ok(2));
    }
    #[test]
    fn errors() {
        assert_eq!(eval("1 / 0"), Err(Error::DivZero));
        assert_eq!(eval("1 +"), Err(Error::End));
        assert_eq!(eval("2 $ 3"), Err(Error::Unexpected(1)));
    }
}
EOF
  cat > "$R/src/main.rs" <<'EOF'
fn main() {
    let threads = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(2);
    let digest = ci_rust::parallel_digest(200_000, threads);
    let primes = ci_rust::primes_below(2_000_000).len();
    let expr = ci_rust::parser::eval("(17 * 3 + 9) / 4 - 2 % 3").unwrap();
    let crc = ci_rust::hash::crc32(b"123456789");
    println!("RUST_APP_OK threads={threads} digest={digest:016x} primes={primes} expr={expr} crc={crc:08x}");
}
EOF
  cat > "$R/tests/integration_test.rs" <<'EOF'
use ci_rust::{hash, parallel_digest, parser, primes_below};

#[test]
fn digest_matches_serial() {
    let serial = (0..50_000).fold(0u64, |a, i| a ^ hash::fnv1a(format!("record-{i}").as_bytes()));
    assert_eq!(parallel_digest(50_000, 7), serial);
}

#[test]
fn parser_and_primes_agree() {
    let n = parser::eval("10 * 10").unwrap() as usize;
    assert_eq!(primes_below(n).len(), 25);
}

#[test]
fn crc_of_large_buffer() {
    let buf: Vec<u8> = (0..1_000_000u32).map(|i| (i % 251) as u8).collect();
    assert_eq!(hash::crc32(&buf), hash::crc32(&buf.clone()));
    assert_ne!(hash::crc32(&buf), 0);
}
EOF
  export CARGO_NET_OFFLINE=true
  if time_cmd rust_cargo_build_ms bash -c "cd '$R' && CARGO_TARGET_DIR='$RUST_TARGET' cargo build --release --offline -j $NCPUS"; then
    out="$("$RUST_TARGET/release/ci_rust" 2>&1 || true)"
    case "$out" in RUST_APP_OK*) echo "INFO|rust_app|$out";; *) echo "FAIL|rust_app|$out"; FAILED=1;; esac
  fi
  time_cmd rust_cargo_test_ms bash -c "cd '$R' && CARGO_TARGET_DIR='$RUST_TARGET' cargo test --offline -j $NCPUS" || true
else
  skip rust_cargo_build_ms "cargo/rustc not found"
  skip rust_cargo_test_ms "cargo/rustc not found"
fi

# ---------------------------------------------------------------------------
# 4. Go: module with a worker-pool package (goroutines + SHA-256 + JSON), tests, and a command
# ---------------------------------------------------------------------------
if have go; then
  G="$WORK/go"
  mkdir -p "$G/pkg/worker" "$G/cmd/app"
  GO_CACHE_DIR="${GOCACHE:-$WORK/go-cache}/ci-multilang"
  rm -rf "$GO_CACHE_DIR"
  cat > "$G/go.mod" <<'EOF'
module example.com/cimultilang

go 1.21
EOF
  cat > "$G/pkg/worker/worker.go" <<'EOF'
// Package worker runs SHA-256 jobs on a bounded goroutine pool.
package worker

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"sort"
	"sync"
)

type Job struct {
	ID      int    `json:"id"`
	Payload string `json:"payload"`
	Rounds  int    `json:"rounds"`
}

type Result struct {
	ID     int    `json:"id"`
	Digest string `json:"digest"`
}

// Digest hashes payload `rounds` times (SHA-256 chained).
func Digest(payload string, rounds int) string {
	sum := sha256.Sum256([]byte(payload))
	for i := 1; i < rounds; i++ {
		sum = sha256.Sum256(sum[:])
	}
	return hex.EncodeToString(sum[:])
}

// Run processes jobs on `workers` goroutines; results are sorted by job ID.
func Run(jobs []Job, workers int) []Result {
	if workers < 1 {
		workers = 1
	}
	in := make(chan Job)
	out := make(chan Result, len(jobs))
	var wg sync.WaitGroup
	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := range in {
				out <- Result{ID: j.ID, Digest: Digest(j.Payload, j.Rounds)}
			}
		}()
	}
	for _, j := range jobs {
		in <- j
	}
	close(in)
	wg.Wait()
	close(out)
	res := make([]Result, 0, len(jobs))
	for r := range out {
		res = append(res, r)
	}
	sort.Slice(res, func(a, b int) bool { return res[a].ID < res[b].ID })
	return res
}

// Encode/Decode round-trip results as JSON.
func Encode(r []Result) ([]byte, error) { return json.Marshal(r) }

func Decode(b []byte) ([]Result, error) {
	var r []Result
	err := json.Unmarshal(b, &r)
	return r, err
}
EOF
  cat > "$G/pkg/worker/worker_test.go" <<'EOF'
package worker

import (
	"fmt"
	"reflect"
	"runtime"
	"testing"
)

func jobs(n int) []Job {
	js := make([]Job, n)
	for i := range js {
		js[i] = Job{ID: i, Payload: fmt.Sprintf("payload-%d", i), Rounds: 200}
	}
	return js
}

func TestDigestKnownVector(t *testing.T) {
	const want = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
	if got := Digest("abc", 1); got != want {
		t.Fatalf("sha256(abc) = %s", got)
	}
}

func TestPoolDeterministic(t *testing.T) {
	a := Run(jobs(2000), 1)
	b := Run(jobs(2000), runtime.NumCPU()*2)
	if !reflect.DeepEqual(a, b) {
		t.Fatal("pool results depend on worker count")
	}
	for i, r := range b {
		if r.ID != i || len(r.Digest) != 64 {
			t.Fatalf("bad result %d: %+v", i, r)
		}
	}
}

func TestJSONRoundTrip(t *testing.T) {
	r := Run(jobs(300), 4)
	b, err := Encode(r)
	if err != nil {
		t.Fatal(err)
	}
	back, err := Decode(b)
	if err != nil || !reflect.DeepEqual(r, back) {
		t.Fatalf("round trip failed: %v", err)
	}
}

func TestParallelSubtests(t *testing.T) {
	for i := 0; i < 8; i++ {
		i := i
		t.Run(fmt.Sprintf("shard%d", i), func(t *testing.T) {
			t.Parallel()
			if got := len(Run(jobs(250), 2)); got != 250 {
				t.Fatalf("shard %d: %d results", i, got)
			}
		})
	}
}
EOF
  cat > "$G/cmd/app/main.go" <<'EOF'
package main

import (
	"fmt"
	"os"
	"runtime"

	"example.com/cimultilang/pkg/worker"
)

func main() {
	js := make([]worker.Job, 5000)
	for i := range js {
		js[i] = worker.Job{ID: i, Payload: fmt.Sprintf("app-%d", i), Rounds: 100}
	}
	res := worker.Run(js, runtime.NumCPU())
	b, err := worker.Encode(res)
	if err != nil || len(res) != len(js) {
		fmt.Println("GO_APP_FAIL", err)
		os.Exit(1)
	}
	fmt.Printf("GO_APP_OK jobs=%d json_bytes=%d last=%s\n", len(res), len(b), res[len(res)-1].Digest[:16])
}
EOF
  if time_cmd go_build_test_ms env GOCACHE="$GO_CACHE_DIR" GOTOOLCHAIN=local GOPROXY=off GOFLAGS= \
      bash -c "cd '$G' && go test -race=false ./... && go build -o app ./cmd/app && ./app"; then
    echo "INFO|go_app|$(grep GO_APP_OK "$WORK/logs/go_build_test_ms.log" | tail -n 1)"
  fi
else
  skip go_build_test_ms "go not found"
fi

# ---------------------------------------------------------------------------
# 5. C/C++20 with CMake + Ninja: static library from 6 translation units, app, unit tests via ctest
# ---------------------------------------------------------------------------
if have cmake && have ninja; then
  C="$WORK/cpp"
  mkdir -p "$C/src" "$C/include" "$C/tests"
  cat > "$C/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(CiMulti LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
find_package(Threads REQUIRED)

add_library(cilib STATIC
  src/hashing.c
  src/strutil.cpp
  src/matrix.cpp
  src/sieve.cpp
  src/threadpool.cpp
  src/json_writer.cpp)
target_include_directories(cilib PUBLIC include)
target_link_libraries(cilib PUBLIC Threads::Threads)
target_compile_options(cilib PRIVATE -Wall -Wextra)

add_executable(ci_app src/main.cpp)
target_link_libraries(ci_app PRIVATE cilib)

add_executable(ci_tests tests/test_main.cpp)
target_link_libraries(ci_tests PRIVATE cilib)

enable_testing()
add_test(NAME unit_tests COMMAND ci_tests)
add_test(NAME app_runs COMMAND ci_app)
set_tests_properties(app_runs PROPERTIES PASS_REGULAR_EXPRESSION "CPP_APP_OK")
EOF
  cat > "$C/include/cihash.h" <<'EOF'
#ifndef CIHASH_H
#define CIHASH_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t ci_crc32(const unsigned char *data, size_t len);
uint64_t ci_fnv1a(const unsigned char *data, size_t len);
#ifdef __cplusplus
}
#endif
#endif
EOF
  cat > "$C/include/cilib.hpp" <<'EOF'
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>
#include "cihash.h"

namespace ci {
std::vector<std::string> split(const std::string &s, char sep);
std::string join(std::span<const std::string> parts, const std::string &sep);
std::string to_upper(std::string s);

struct Matrix {
    int n;
    std::vector<double> a;
    explicit Matrix(int n_) : n(n_), a(static_cast<size_t>(n_) * n_, 0.0) {}
    double &at(int i, int j) { return a[static_cast<size_t>(i) * n + j]; }
    double at(int i, int j) const { return a[static_cast<size_t>(i) * n + j]; }
};
Matrix multiply(const Matrix &x, const Matrix &y, unsigned threads);
Matrix identity(int n);

std::vector<int> primes_below(int n);

// Runs fn(i) for i in [0, n) on `threads` threads; returns the sum of results.
std::uint64_t parallel_sum(int n, unsigned threads, const std::function<std::uint64_t(int)> &fn);

std::string to_json(const std::map<std::string, std::string> &obj);
}  // namespace ci
EOF
  cat > "$C/src/hashing.c" <<'EOF'
#include "cihash.h"

uint32_t ci_crc32(const unsigned char *data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= data[i];
        for (int k = 0; k < 8; k++) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

uint64_t ci_fnv1a(const unsigned char *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < len; i++) { h ^= data[i]; h *= 0x100000001b3ull; }
    return h;
}
EOF
  cat > "$C/src/strutil.cpp" <<'EOF'
#include <algorithm>
#include <cctype>
#include "cilib.hpp"

namespace ci {
std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); } else { cur += c; }
    }
    out.push_back(cur);
    return out;
}
std::string join(std::span<const std::string> parts, const std::string &sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) { if (i) out += sep; out += parts[i]; }
    return out;
}
std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}
}  // namespace ci
EOF
  cat > "$C/src/matrix.cpp" <<'EOF'
#include <thread>
#include "cilib.hpp"

namespace ci {
Matrix identity(int n) {
    Matrix m(n);
    for (int i = 0; i < n; i++) m.at(i, i) = 1.0;
    return m;
}
Matrix multiply(const Matrix &x, const Matrix &y, unsigned threads) {
    Matrix r(x.n);
    if (threads == 0) threads = 1;
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < threads; t++) {
        ts.emplace_back([&, t] {
            for (int i = static_cast<int>(t); i < x.n; i += static_cast<int>(threads))
                for (int k = 0; k < x.n; k++) {
                    double v = x.at(i, k);
                    for (int j = 0; j < x.n; j++) r.at(i, j) += v * y.at(k, j);
                }
        });
    }
    for (auto &th : ts) th.join();
    return r;
}
}  // namespace ci
EOF
  cat > "$C/src/sieve.cpp" <<'EOF'
#include "cilib.hpp"

namespace ci {
std::vector<int> primes_below(int n) {
    std::vector<int> out;
    if (n < 3) return out;
    std::vector<bool> s(static_cast<size_t>(n), true);
    s[0] = s[1] = false;
    for (long i = 2; i * i < n; i++)
        if (s[static_cast<size_t>(i)])
            for (long j = i * i; j < n; j += i) s[static_cast<size_t>(j)] = false;
    for (int i = 2; i < n; i++) if (s[static_cast<size_t>(i)]) out.push_back(i);
    return out;
}
}  // namespace ci
EOF
  cat > "$C/src/threadpool.cpp" <<'EOF'
#include <atomic>
#include <thread>
#include "cilib.hpp"

namespace ci {
std::uint64_t parallel_sum(int n, unsigned threads, const std::function<std::uint64_t(int)> &fn) {
    if (threads == 0) threads = 1;
    std::atomic<int> next{0};
    std::atomic<std::uint64_t> total{0};
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < threads; t++) {
        ts.emplace_back([&] {
            std::uint64_t local = 0;
            for (int i; (i = next.fetch_add(64)) < n;)
                for (int k = i; k < i + 64 && k < n; k++) local += fn(k);
            total += local;
        });
    }
    for (auto &th : ts) th.join();
    return total.load();
}
}  // namespace ci
EOF
  cat > "$C/src/json_writer.cpp" <<'EOF'
#include <cstdio>
#include "cilib.hpp"

namespace ci {
static std::string escape(const std::string &s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    o += buf;
                } else {
                    o += c;
                }
        }
    }
    return o;
}
std::string to_json(const std::map<std::string, std::string> &obj) {
    std::string o = "{";
    bool first = true;
    for (const auto &[k, v] : obj) {
        if (!first) o += ",";
        first = false;
        o += "\"" + escape(k) + "\":\"" + escape(v) + "\"";
    }
    return o + "}";
}
}  // namespace ci
EOF
  cat > "$C/src/main.cpp" <<'EOF'
#include <cstdio>
#include <thread>
#include "cilib.hpp"

int main() {
    unsigned th = std::thread::hardware_concurrency();
    auto m = ci::multiply(ci::identity(256), ci::identity(256), th);
    auto primes = ci::primes_below(2'000'000);
    auto sum = ci::parallel_sum(1'000'000, th, [](int i) { return static_cast<std::uint64_t>(i); });
    const char *msg = "123456789";
    auto crc = ci_crc32(reinterpret_cast<const unsigned char *>(msg), 9);
    std::printf("CPP_APP_OK threads=%u trace=%.0f primes=%zu sum=%llu crc=%08x json=%s\n", th, m.at(255, 255) * 256,
                primes.size(), static_cast<unsigned long long>(sum), crc,
                ci::to_json({{"lang", "c++20"}, {"build", "cmake+ninja"}}).c_str());
    return 0;
}
EOF
  cat > "$C/tests/test_main.cpp" <<'EOF'
#include <cstdio>
#include <cstring>
#include <string>
#include "cilib.hpp"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main() {
    const auto *abc = reinterpret_cast<const unsigned char *>("123456789");
    CHECK(ci_crc32(abc, 9) == 0xCBF43926u);
    CHECK(ci_fnv1a(reinterpret_cast<const unsigned char *>("a"), 1) == 0xaf63dc4c8601ec8cull);

    auto parts = ci::split("a,b,,c", ',');
    CHECK(parts.size() == 4 && parts[2].empty());
    CHECK(ci::join(parts, "-") == "a-b--c");
    CHECK(ci::to_upper("ci-bench") == "CI-BENCH");

    ci::Matrix a(64), b = ci::identity(64);
    for (int i = 0; i < 64; i++) for (int j = 0; j < 64; j++) a.at(i, j) = i * 64 + j;
    auto r1 = ci::multiply(a, b, 1), r8 = ci::multiply(a, b, 8);
    CHECK(r1.a == a.a && r8.a == a.a);

    CHECK(ci::primes_below(30) == (std::vector<int>{2, 3, 5, 7, 11, 13, 17, 19, 23, 29}));
    CHECK(ci::primes_below(1'000'000).size() == 78'498);

    CHECK(ci::parallel_sum(100'000, 8, [](int i) { return static_cast<std::uint64_t>(i); }) == 4'999'950'000ull);
    CHECK(ci::to_json({{"k", "v\"q"}, {"a", "1"}}) == "{\"a\":\"1\",\"k\":\"v\\\"q\"}");

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("CPP_TESTS_OK\n");
    return 0;
}
EOF
  time_cmd cmake_ninja_build_test_ms bash -c "cd '$C' && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j $NCPUS && ctest --test-dir build --output-on-failure" || true
else
  skip cmake_ninja_build_test_ms "cmake/ninja not found"
fi

# ---------------------------------------------------------------------------
# 6a. Node.js: node:test + node:assert/strict + node:crypto + worker_threads (ESM)
# ---------------------------------------------------------------------------
if [ -n "$NODE" ]; then
  N="$WORK/node"
  mkdir -p "$N/src" "$N/test"
  cat > "$N/package.json" <<'EOF'
{ "name": "ci-multilang-node", "version": "1.0.0", "type": "module", "private": true }
EOF
  cat > "$N/src/lib.mjs" <<'EOF'
import { createHash, pbkdf2, randomBytes } from 'node:crypto';
import { promisify } from 'node:util';

export const sha256 = (s) => createHash('sha256').update(s).digest('hex');
export const derive = (pw, salt) => promisify(pbkdf2)(pw, salt, 20000, 32, 'sha256');
export const token = () => randomBytes(16).toString('hex');

export function wordFreq(text) {
  const m = new Map();
  for (const w of text.toLowerCase().match(/[a-z]+/g) ?? []) m.set(w, (m.get(w) ?? 0) + 1);
  return [...m.entries()].sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]));
}
EOF
  cat > "$N/src/worker.mjs" <<'EOF'
import { parentPort, workerData } from 'node:worker_threads';
import { createHash } from 'node:crypto';

let h = Buffer.from(workerData.seed);
for (let i = 0; i < workerData.rounds; i++) h = createHash('sha256').update(h).digest();
parentPort.postMessage({ id: workerData.id, digest: h.toString('hex') });
EOF
  cat > "$N/test/lib.test.mjs" <<'EOF'
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { sha256, derive, token, wordFreq } from '../src/lib.mjs';

test('sha256 known vector', () => {
  assert.equal(sha256('abc'), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
});

test('pbkdf2 is deterministic and async', async () => {
  const [a, b] = await Promise.all([derive('pw', 'salt'), derive('pw', 'salt')]);
  assert.deepEqual(a, b);
  assert.equal(a.length, 32);
});

test('random tokens differ', () => {
  const s = new Set(Array.from({ length: 1000 }, token));
  assert.equal(s.size, 1000);
});

test('word frequency + JSON round trip', () => {
  const f = wordFreq('the cat and the hat and the bat');
  assert.deepEqual(f[0], ['the', 3]);
  assert.deepEqual(JSON.parse(JSON.stringify(f)), f);
});
EOF
  cat > "$N/test/worker.test.mjs" <<'EOF'
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Worker } from 'node:worker_threads';
import { createHash } from 'node:crypto';
import os from 'node:os';

const run = (id, seed, rounds) => new Promise((resolve, reject) => {
  const w = new Worker(new URL('../src/worker.mjs', import.meta.url), { workerData: { id, seed, rounds } });
  w.once('message', resolve);
  w.once('error', reject);
});

test('worker_threads pool matches main-thread hashing', async () => {
  const n = (os.availableParallelism?.() ?? os.cpus().length) * 2;
  const results = await Promise.all(Array.from({ length: n }, (_, i) => run(i, `seed-${i}`, 5000)));
  for (const r of results) {
    let h = Buffer.from(`seed-${r.id}`);
    for (let i = 0; i < 5000; i++) h = createHash('sha256').update(h).digest();
    assert.equal(r.digest, h.toString('hex'));
  }
});
EOF
  time_cmd node_test_ms bash -c "cd '$N' && '$NODE' --test test/lib.test.mjs test/worker.test.mjs" || true
else
  skip node_test_ms "node not found"
fi

# ---------------------------------------------------------------------------
# 6b. Python 3: unittest over sqlite3, zlib, json, hashlib, concurrent.futures
# ---------------------------------------------------------------------------
PY="$WORK/python"
mkdir -p "$PY"
cat > "$PY/test_ci.py" <<'EOF'
import concurrent.futures as cf
import hashlib
import json
import os
import sqlite3
import unittest
import zlib


def chained_sha256(seed, rounds=20000):
    h = seed.encode()
    for _ in range(rounds):
        h = hashlib.sha256(h).digest()
    return h.hex()


class SQLiteTest(unittest.TestCase):
    def test_aggregate_and_transaction(self):
        db = sqlite3.connect(":memory:")
        db.execute("create table jobs (id integer primary key, lang text, ms integer)")
        rows = [(i, ("swift", "rust", "go", "c++")[i % 4], i * 3) for i in range(20000)]
        with db:
            db.executemany("insert into jobs values (?, ?, ?)", rows)
        got = dict(db.execute("select lang, count(*) from jobs group by lang"))
        self.assertEqual(got, {"swift": 5000, "rust": 5000, "go": 5000, "c++": 5000})
        try:
            with db:
                db.execute("insert into jobs values (1, 'dup', 0)")
        except sqlite3.IntegrityError:
            pass
        self.assertEqual(db.execute("select count(*) from jobs").fetchone()[0], 20000)
        db.execute("create index i_ms on jobs(ms)")
        self.assertEqual(db.execute("select max(ms) from jobs where ms < 100").fetchone()[0], 99)


class ZlibJsonTest(unittest.TestCase):
    def test_roundtrip(self):
        doc = {"runs": [{"id": i, "ok": i % 7 != 0, "tags": ["ci", str(i % 5)]} for i in range(5000)]}
        raw = json.dumps(doc, sort_keys=True).encode()
        packed = zlib.compress(raw, 9)
        self.assertLess(len(packed), len(raw) // 4)
        self.assertEqual(json.loads(zlib.decompress(packed)), doc)
        self.assertEqual(zlib.crc32(b"123456789"), 0xCBF43926)


class HashTest(unittest.TestCase):
    def test_vectors(self):
        self.assertEqual(hashlib.sha256(b"abc").hexdigest(),
                         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        self.assertEqual(hashlib.md5(b"").hexdigest(), "d41d8cd98f00b204e9800998ecf8427e")


class FuturesTest(unittest.TestCase):
    seeds = ["seed-%d" % i for i in range(16)]

    def test_threads_and_processes_agree(self):
        serial = [chained_sha256(s) for s in self.seeds]
        workers = os.cpu_count() or 2
        with cf.ThreadPoolExecutor(workers) as ex:
            self.assertEqual(list(ex.map(chained_sha256, self.seeds)), serial)
        with cf.ProcessPoolExecutor(workers) as ex:
            self.assertEqual(list(ex.map(chained_sha256, self.seeds)), serial)


if __name__ == "__main__":
    unittest.main()
EOF
time_cmd python_test_ms bash -c "cd '$PY' && python3 -m unittest -v test_ci" || true

# ---------------------------------------------------------------------------
# 7. Live GitHub HTTPS: API + a release tarball (real outbound TLS)
# ---------------------------------------------------------------------------
if have curl; then
  cat > "$WORK/https_check.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
cd "$1"
auth=()
if [ -n "${GITHUB_TOKEN:-}" ]; then   # optional: avoids the shared-IP unauthenticated rate limit; kept off argv
  printf 'header = "Authorization: Bearer %s"\n' "$GITHUB_TOKEN" > .curlrc-auth
  auth=(--config .curlrc-auth)
fi
zen="$(curl -fsSL --retry 3 --retry-delay 2 --retry-all-errors --max-time 30 ${auth[@]+"${auth[@]}"} https://api.github.com/zen)"
rm -f .curlrc-auth
[ -n "$zen" ] || { echo "empty zen"; exit 1; }
echo "zen: $zen"
curl -fsSL --retry 3 --retry-delay 2 --retry-all-errors --max-time 120 \
  https://github.com/lua/lua/archive/refs/tags/v5.4.7.tar.gz -o lua.tar.gz
size=$(wc -c < lua.tar.gz | tr -d ' ')
[ "$size" -gt 100000 ] || { echo "tarball too small: $size"; exit 1; }
gzip -t lua.tar.gz
tar -tzf lua.tar.gz > lua.list   # no pipe into grep -q: SIGPIPE under pipefail
grep -q 'lua-5.4.7/lapi.c' lua.list
sha=$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' lua.tar.gz)
echo "lua.tar.gz bytes=$size sha256=$sha"
EOF
  mkdir -p "$WORK/https"
  if time_cmd github_https_ms bash "$WORK/https_check.sh" "$WORK/https"; then
    echo "INFO|github_https|$(grep -E '^lua.tar.gz' "$WORK/logs/github_https_ms.log")"
  fi
else
  skip github_https_ms "curl not found"
fi

T_ALL1="$(python3 -c 'import time; print(time.time())')"
printf 'METRIC|total_ms|%d\n' "$(python3 -c "print(int(($T_ALL1 - $T_ALL0) * 1000))")"

if [ "$EPHEMERAL_CHECK" = 1 ]; then
  echo "dirty marker from $LABEL at $(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$MARKER"
  info ephemeral_marker "created $MARKER (must be absent on the next copy-on-write boot)"
fi

if [ "$FAILED" = 0 ]; then
  echo "=== DONE: $LABEL (all passed) ==="
else
  echo "=== DONE: $LABEL (FAILURES above) ==="
  exit 1
fi
