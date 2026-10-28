#!/usr/bin/env bash
# Offline regression checks for scripts/triage-failure.py.
#
# No network and no Jev credentials are required: the request-building path is
# checked with --print-request, and the response path uses a fake jev binary.
set -uo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TRIAGE="$PROJECT_ROOT/scripts/triage-failure.py"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/triage-test.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/patch.log" <<'EOF'
==> 0017-libcore-resolve-resource-path.patch assumed applied (overlapping context; unverifiable)
==> patches: 2 applied, 71 already applied, 1 unverifiable
    unverifiable (they would not be applied): 0017-libcore-resolve-resource-path.patch
==> Running headless integration test
check "theme resource loaded"          FAIL
VERIFICATION FAILED: could not open /res/color-schemes/default.ini
TYPESAFE_API_KEY=should-not-leak
EOF

cat > "$TMP/test.log" <<'EOF'
==> Running upstream library test suites
TestGfxBitmap      PASS
TestPainter        FAIL
    Expected pixel at (10, 10) to be #ff0000, got #000000
==> 1 test(s) failed
EOF

cat > "$TMP/headless.log" <<'EOF'
window presented over wl_shm             FAIL
pointer entered the window               FAIL
synthetic click reached widget           FAIL
==> 3 headless check(s) failed; app log:
LibWM: no Wayland connection on the main thread: LibWM: wl_display_connect failed (no compositor?)
LibWM: running without a Wayland compositor: LibWM: wl_display_connect failed (no compositor?)
EOF

python3 - "$TMP/large.log" <<'PY'
import sys
path = sys.argv[1]
lines = [f"[{i}/50000] Building CXX object LibGUI/Widget{i % 50}.cpp.o" for i in range(50000)]
lines.insert(10, "fatal error: early error that may be truncated away")
lines.append("fatal error: final error that must be retained")
open(path, "w").write("\n".join(lines))
PY

"$TRIAGE" "$TMP/patch.log" --print-request > "$TMP/patch.request.json"
"$TRIAGE" "$TMP/headless.log" --print-request > "$TMP/headless.request.json"
"$TRIAGE" "$TMP/large.log" --max-chars 2000 --print-request > "$TMP/large.request.json"

cat > "$TMP/fake-jev" <<'EOF'
#!/usr/bin/env bash
cat >/dev/null
cat <<'JSON'
{"model":"fake-1","answers":{"failure_class":{"type":"choice","choice":"test_failure","confidence":0.9},"subsystem":{"type":"choice","choice":"libgui","confidence":0.9},"severity":{"type":"score","score":1.2,"confidence":0.8,"legend":{"0":"Cosmetic","1":"Blocks one test or app","2":"Blocks the build"},"probabilities":{"0":0.1,"1":0.6,"2":0.3}},"next_action":{"type":"choice","choice":"fix_source","confidence":0.8},"environmental":{"type":"noul","noul":0.2}},"usage":{"input_tokens":10,"output_tokens":5}}
JSON
EOF
chmod +x "$TMP/fake-jev"

"$TRIAGE" "$TMP/test.log" --jev "$TMP/fake-jev" --json > "$TMP/fake.json"
"$TRIAGE" "$TMP/test.log" --jev "$TMP/fake-jev" > "$TMP/fake.card"

"$TRIAGE" "$TMP/test.log" --jev "$TMP/missing-jev" >/dev/null 2>&1
soft_status=$?
"$TRIAGE" "$TMP/test.log" --jev "$TMP/missing-jev" --strict >/dev/null 2>&1
strict_status=$?

python3 - "$TMP" "$soft_status" "$strict_status" <<'PY'
import json
import pathlib
import sys

tmp = pathlib.Path(sys.argv[1])
soft_status = int(sys.argv[2])
strict_status = int(sys.argv[3])
failures = 0


def check(name, condition):
    global failures
    if condition:
        print(f"{name:<44} PASS")
    else:
        print(f"{name:<44} FAIL")
        failures += 1


patch = json.loads((tmp / "patch.request.json").read_text())["state"]
check("patch problem extracted",
      any("0017" in line for line in patch["patch_problems"]))
check("failed check extracted",
      any("theme resource loaded" in line for line in patch["failed_checks"]))
check("error context retained",
      "VERIFICATION FAILED" in patch["log_excerpt"])

headless = json.loads((tmp / "headless.request.json").read_text())["state"]
check("environment clues extracted",
      any("no compositor" in line.lower() for line in headless["environment_errors"]))
check("secret redacted",
      "should-not-leak" not in patch["log_excerpt"] and "<redacted>" in patch["log_excerpt"])

large = json.loads((tmp / "large.request.json").read_text())["state"]
check("excerpt respects --max-chars", len(large["log_excerpt"]) <= 2000)
check("tail error retained", "final error that must be retained" in large["log_excerpt"])
check("line count reported", large["log_lines"] == 50002)

fake = json.loads((tmp / "fake.json").read_text())
check("fake response model recorded", fake["model"] == "fake-1")
check("fake response answer recorded",
      fake["answers"]["failure_class"]["choice"] == "test_failure")
check("fake response usage recorded", fake["usage"]["input_tokens"] == 10)

card = (tmp / "fake.card").read_text()
check("card shows choice", "test_failure" in card)
check("card shows score label", "Blocks one test or app" in card)
check("card shows next action", "fix_source" in card)

check("missing jev is soft by default", soft_status == 0)
check("missing jev fails under --strict", strict_status == 1)

if failures:
    print(f"==> {failures} check(s) failed")
    raise SystemExit(1)
print("==> all triage checks passed")
PY
