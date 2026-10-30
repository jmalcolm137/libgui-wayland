#!/usr/bin/env python3
"""Turn a failed build or test log into a short Jev triage card.

The log is reduced to the lines that usually explain a failure: compiler and
linker errors, failed check lines, patch-application problems, and the tail.
That excerpt plus a little repository state is sent as the `state` of one
batched Jev request, so all questions cost one call.

Jev is a classifier, not an oracle: the card reports confidence, and a caller
should treat a low-confidence answer as "investigate", not as a verdict.

Usage:
    scripts/triage-failure.py build.log
    scripts/triage-failure.py - < build.log
    scripts/build-libgui.sh 2>&1 | scripts/triage-failure.py -
    scripts/triage-failure.py build.log --json

The excerpt is sent to the configured Jev provider. Use --print-request to see
exactly what would be sent without making a request.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SERENITY = PROJECT_ROOT / "serenity"
FETCH_SCRIPT = PROJECT_ROOT / "scripts" / "fetch-serenity.sh"

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# Lines worth sending: the ones that usually explain a build or test failure.
ERROR = re.compile(
    r"(fatal error:|error:|undefined reference|CMake Error|ninja: build stopped|"
    r"Segmentation fault|VERIFICATION FAILED|Assertion .*failed|"
    r"Traceback \(most recent call last\)|No such file or directory|"
    r"cannot find -l|timed out|Connection refused|Address already in use|"
    r"\bFAIL(?:ED)?\b)",
    re.IGNORECASE,
)
PATCH_PROBLEM = re.compile(
    r"(assumed applied|unverifiable|does not apply|"
    r"\bpatch(?:es)?\b.*\b(?:failed|error)\b)",
    re.IGNORECASE,
)
# Non-fatal: "did not apply (already applied?)" means the build continued.
PATCH_WARNING = re.compile(
    r"(\bdid not apply\b|"
    r"WARNING:.*\b(?:pin|pinned|patch|patches)\b)",
    re.IGNORECASE,
)
PATCH_TOTAL = re.compile(r"patches: \d+ applied", re.IGNORECASE)
FAILED_CHECK = re.compile(r"\bFAIL(?:ED)?\b")
# Upstream evidence that the harness never got a working transport. These
# explain the FAIL lines below them, so they take priority over test_failure.
ENV_ERROR = re.compile(
    r"(\bno compositor\b|\bno Wayland connection\b|\bwl_display_connect failed\b|"
    r"\bWAYLAND_DISPLAY\b|\bXDG_RUNTIME_DIR\b|\bCould not connect\b|"
    r"\bConnection refused\b|\bAddress already in use\b|\bpermission denied\b|"
    r"\btimed out\b|\bTimeout\b)",
    re.IGNORECASE,
)
# Keep credentials that a command may have echoed out of the excerpt.
SECRET = re.compile(
    r"(?i)([A-Z0-9_]*(?:API[_-]?KEY|TOKEN|SECRET|PASSWORD)[A-Z0-9_]*)"
    r"\s*[=:]\s*(\S+)"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "log",
        nargs="?",
        default="-",
        help="log file to triage, or - for stdin (default: -)",
    )
    parser.add_argument(
        "--job",
        default="build",
        help="job name recorded in the state (default: build)",
    )
    parser.add_argument(
        "--exit-code",
        type=int,
        default=None,
        help="exit status of the failed job, if known",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="print the triage response as JSON instead of a card",
    )
    parser.add_argument(
        "--max-chars",
        type=int,
        default=12000,
        help="maximum log excerpt characters to send (default: 12000)",
    )
    parser.add_argument(
        "--print-request",
        action="store_true",
        help="print the request that would be sent, then exit",
    )
    parser.add_argument(
        "--jev",
        default=os.environ.get("JEV", "jev"),
        help="jev executable (default: $JEV or jev)",
    )
    parser.add_argument(
        "--model",
        default=None,
        help="override the provider's default model",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=60.0,
        help="seconds to wait for the Jev request (default: 60)",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit nonzero when Jev is unavailable or the request fails",
    )
    return parser.parse_args()


def read_log(path: str) -> str:
    if path == "-":
        return sys.stdin.read()
    return Path(path).read_text(errors="replace")


def git_head(repo: Path) -> str | None:
    try:
        proc = subprocess.run(
            ["git", "-C", str(repo), "rev-parse", "--short", "HEAD"],
            text=True,
            capture_output=True,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    return proc.stdout.strip() if proc.returncode == 0 else None


def pinned_revision() -> str | None:
    try:
        text = FETCH_SCRIPT.read_text(errors="replace")
    except OSError:
        return None
    match = re.search(r"^SERENITY_REVISION=.*?([0-9a-f]{7,40})", text, re.MULTILINE)
    return match.group(1) if match else None


def collect_state(
    text: str, job: str, exit_code: int | None, max_chars: int
) -> dict:
    text = ANSI.sub("", text).replace("\x00", "")
    text = text.replace(str(Path.home()), "~")
    text = SECRET.sub(r"\1=<redacted>", text)
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    lines = text.split("\n")

    patch_problems: list[str] = []
    patch_warnings: list[str] = []
    patch_totals: list[str] = []
    failed_checks: list[str] = []
    environment_errors: list[str] = []
    for line in lines:
        stripped = line.strip()
        if not stripped:
            continue
        if PATCH_PROBLEM.search(stripped):
            patch_problems.append(stripped[:300])
        elif PATCH_WARNING.search(stripped):
            patch_warnings.append(stripped[:300])
        elif PATCH_TOTAL.search(stripped):
            patch_totals.append(stripped[:300])
        if FAILED_CHECK.search(stripped):
            failed_checks.append(stripped[:300])
        if ENV_ERROR.search(stripped):
            environment_errors.append(stripped[:300])

    # Keep the context around the first errors, then the tail, in log order.
    selected: list[int] = []
    seen: set[int] = set()

    def add(index: int) -> None:
        if 0 <= index < len(lines) and index not in seen:
            seen.add(index)
            selected.append(index)

    hits = 0
    for index, line in enumerate(lines):
        if ERROR.search(line):
            for nearby in range(max(0, index - 2), min(len(lines), index + 3)):
                add(nearby)
            hits += 1
            if hits >= 40:
                break
    for index in range(max(0, len(lines) - 120), len(lines)):
        add(index)

    selected.sort()
    excerpt_parts: list[str] = []
    previous: int | None = None
    for index in selected:
        if previous is not None and index > previous + 1:
            excerpt_parts.append("...")
        excerpt_parts.append(lines[index])
        previous = index
    excerpt = "\n".join(excerpt_parts).strip()

    if len(excerpt) > max_chars:
        excerpt = excerpt[-max_chars:]
        newline = excerpt.find("\n")
        if newline != -1:
            excerpt = excerpt[newline + 1 :]

    return {
        "job": job,
        "exit_code": exit_code,
        "serenity_head": git_head(SERENITY),
        "serenity_pin": pinned_revision(),
        "patch_problems": patch_problems[:12],
        "patch_warnings": patch_warnings[:12],
        "patch_totals": patch_totals[:2],
        "failed_checks": failed_checks[:20],
        "environment_errors": environment_errors[:10],
        "log_excerpt": excerpt,
        "log_lines": len(lines),
        "log_bytes": len(text.encode("utf-8", errors="replace")),
    }


def build_request(state: dict, model: str | None) -> dict:
    questions = {
        "failure_class": {
            "type": "choice",
            "instructions": (
                "Classify the primary failure in `log_excerpt` and `failed_checks`. "
                "Treat `environment_errors` as upstream evidence: if it shows the "
                "compositor or transport never started (no compositor, "
                "wl_display_connect failed, absent socket, timeout), choose "
                "environment even when checks fail; those FAIL lines are downstream "
                "symptoms. Prefer patch_drift when `patch_problems` reports "
                "unverifiable, missing, or non-applying patches. Do not choose "
                "patch_drift for `patch_warnings` that say a patch 'did not apply "
                "(already applied?)' when the build continued. Prefer build_order "
                "when a generated endpoint header is missing on a fresh build. "
                "Prefer configure_error when the log ends with a configure, imake, "
                "CMake, or Makefile-generation failure. Prefer test_failure only "
                "when a check ran against a working app and the assertion itself "
                "failed."
            ),
            "criteria": {
                "patch_drift": "A patch does not apply, is unverifiable, or may be missing after a Serenity revision change.",
                "build_order": "A generated header or build target is missing because of ordering, e.g. FileSystemAccessClientEndpoint.h.",
                "configure_error": "configure, imake, CMake, or generated Makefile setup failed before compilation.",
                "compile_error": "A C++ compiler error in project or Serenity source.",
                "link_error": "A missing symbol, library, or undefined reference at link time.",
                "test_failure": "A unit or headless integration check reported FAIL.",
                "environment": "A missing socket, display, resource, tool, permission, or a timeout.",
                "unknown": "None of the above is clearly supported by the log.",
            },
        },
        "subsystem": {
            "type": "choice",
            "instructions": (
                "Which part of the stack most likely owns the failure? "
                "Use paths and symbols in `log_excerpt`. Prefer compositor when "
                "`environment_errors` shows no Wayland connection; prefer libwm "
                "when the app's own rendering or input code failed; prefer "
                "build_system for configure, imake, CMake, or generated-Makefile "
                "failures."
            ),
            "criteria": {
                "libwm": "LibWM, WaylandClient, WindowServerConnection, input, clipboard, menus, or portals.",
                "libgui": "LibGUI or LibGfx widgets, painting, text, themes, or fonts.",
                "libcore": "AK, LibCore, IPC, process or session handling, or config.",
                "apps": "A SerenityOS application under Userland/Applications.",
                "compositor": "The headless compositor, wl_shm, or the test harness.",
                "build_system": "CMake, ninja, patch application, or generated headers.",
                "unknown": "The log does not identify a subsystem.",
            },
        },
        "severity": {
            "type": "score",
            "instructions": "How much does this failure block the workflow?",
            "criteria": [
                "Cosmetic or warning",
                "Blocks one test or app",
                "Blocks the build or full test run",
            ],
        },
        "next_action": {
            "type": "choice",
            "instructions": "What is the single best next action for a developer?",
            "criteria": {
                "refresh_patch": "Regenerate the failing patch hunks against the current Serenity revision, then rebuild.",
                "build_generators": "Rerun the two-phase build so generated endpoint headers exist before the shim.",
                "fix_build_setup": "Fix the configure, imake, CMake, or generated Makefile setup, then rerun.",
                "fix_source": "Change C++ source or a patch in response to the reported error.",
                "fix_environment": "Start the compositor or fix WAYLAND_DISPLAY/XDG_RUNTIME_DIR, then rerun the test.",
                "rerun": "Rerun once on a clean runtime directory; the failure looks transient.",
                "investigate": "Inspect the full log manually; the excerpt does not support a specific action.",
            },
        },
        "environmental": {
            "type": "noul",
            "instructions": (
                "Is this failure better explained by environment, flakiness, or "
                "build ordering than by a code regression?"
            ),
        },
    }
    request: dict = {"state": state, "questions": questions}
    if model:
        request["model"] = model
    return request


def call_jev(jev: str, request: dict, timeout: float) -> dict:
    executable = shutil.which(jev)
    if executable is None:
        candidate = Path(jev).expanduser()
        if candidate.is_file():
            executable = str(candidate)
    if executable is None:
        raise RuntimeError(f"{jev} not found; install jev-cli or set JEV")

    proc = subprocess.run(
        [executable, "run", "-"],
        input=json.dumps(request),
        text=True,
        capture_output=True,
        timeout=timeout,
    )
    if proc.returncode != 0:
        detail = proc.stderr.strip()
        try:
            detail = json.loads(detail).get("error", detail)
        except (json.JSONDecodeError, AttributeError):
            pass
        raise RuntimeError(detail or f"jev exited {proc.returncode}")

    try:
        response = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"jev returned invalid JSON: {exc}") from exc
    if not isinstance(response, dict) or "answers" not in response:
        raise RuntimeError("jev response has no answers")
    return response


def confidence(value: object) -> float:
    try:
        return float(value)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return 0.0


def format_choice(answer: dict) -> str:
    choice = answer.get("choice", "?")
    return f"{choice} ({confidence(answer.get('confidence')):.2f})"


def format_noul(answer: dict) -> str:
    probability = confidence(answer.get("noul"))
    verdict = "yes" if probability >= 0.5 else "no"
    return f"{probability:.2f} ({verdict})"


def format_score(answer: dict) -> str:
    legend = answer.get("legend", {})
    index = None
    probabilities = answer.get("probabilities")
    if isinstance(probabilities, dict) and probabilities:
        try:
            index = int(max(probabilities.items(), key=lambda item: float(item[1]))[0])
        except (TypeError, ValueError):
            index = None
    if index is None:
        try:
            index = int(round(float(answer.get("score"))))
        except (TypeError, ValueError):
            index = None

    if isinstance(legend, dict) and index is not None:
        label = legend.get(str(index), str(index))
    else:
        label = str(answer.get("score", "?"))
    return f"{label} ({confidence(answer.get('confidence')):.2f})"


HINTS = {
    "refresh_patch": (
        "Refresh the failing patch against the current Serenity revision; the "
        "patch problems above name it. Then rerun the build."
    ),
    "build_generators": (
        "Run the build again: the two-phase build generates endpoint headers "
        "first. If it repeats, check the GENERATED targets in build-libgui.sh."
    ),
    "fix_build_setup": (
        "Inspect the configure/imake/CMake log; the generated Makefile was not "
        "produced, so this is build setup rather than C++ source."
    ),
    "fix_source": "Fix the reported source error; the excerpt above is the starting point.",
    "fix_environment": (
        "Start the compositor and check WAYLAND_DISPLAY/XDG_RUNTIME_DIR; the app "
        "log shows the transport never connected."
    ),
    "rerun": "Rerun on a clean runtime directory; if it passes, treat it as transient.",
    "investigate": "Inspect the full log; the excerpt did not support a specific action.",
}


def excerpt_line_count(state: dict) -> int:
    excerpt = state.get("log_excerpt", "")
    return excerpt.count("\n") + 1 if excerpt else 0


def print_card(response: dict, state: dict, log_path: str) -> None:
    answers = response.get("answers", {})
    job = state["job"]
    suffix = f" (exit {state['exit_code']})" if state.get("exit_code") is not None else ""
    print(f"==> Jev triage: {job}{suffix}")
    print(f"    {'failure':<12} {format_choice(answers.get('failure_class', {}))}")
    print(f"    {'subsystem':<12} {format_choice(answers.get('subsystem', {}))}")
    print(f"    {'severity':<12} {format_score(answers.get('severity', {}))}")
    print(f"    {'env/order':<12} {format_noul(answers.get('environmental', {}))}")
    print(f"    {'next':<12} {format_choice(answers.get('next_action', {}))}")

    if state.get("patch_problems"):
        print(f"    {'patch clues':<12} " + " | ".join(state["patch_problems"][:2]))
    if state.get("failed_checks"):
        print(f"    {'failed':<12} " + " | ".join(state["failed_checks"][:3]))
    if state.get("environment_errors"):
        print(f"    {'env clues':<12} " + " | ".join(state["environment_errors"][:2]))

    action = answers.get("next_action", {}).get("choice")
    hint = HINTS.get(action) if isinstance(action, str) else None
    if hint:
        print(f"    {'next step':<12} {hint}")

    if response.get("model"):
        usage = response.get("usage", {})
        tokens = ""
        if isinstance(usage, dict) and "input_tokens" in usage:
            tokens = f", {usage.get('input_tokens')} in / {usage.get('output_tokens')} out tokens"
        print(f"    {'model':<12} {response['model']}{tokens}")
    print(
        f"    {'log':<12} {state['log_lines']} lines, "
        f"{excerpt_line_count(state)} sent"
        + (f" ({log_path})" if log_path != "-" else "")
    )


def print_json(response: dict, state: dict, log_path: str) -> None:
    output = {
        "job": state["job"],
        "exit_code": state.get("exit_code"),
        "model": response.get("model"),
        "answers": response.get("answers", {}),
        "usage": response.get("usage", {}),
        "log": {
            "path": log_path,
            "lines": state["log_lines"],
            "bytes": state["log_bytes"],
            "excerpt_lines": excerpt_line_count(state),
        },
        "patch_problems": state["patch_problems"],
        "patch_warnings": state["patch_warnings"],
        "failed_checks": state["failed_checks"],
        "environment_errors": state["environment_errors"],
    }
    print(json.dumps(output, indent=2))


def main() -> int:
    args = parse_args()
    try:
        text = read_log(args.log)
    except OSError as exc:
        print(f"triage-failure: {exc}", file=sys.stderr)
        return 2

    state = collect_state(text, args.job, args.exit_code, args.max_chars)
    request = build_request(state, args.model)

    if args.print_request:
        print(json.dumps(request, indent=2))
        return 0

    try:
        response = call_jev(args.jev, request, args.timeout)
    except Exception as exc:  # noqa: BLE001 - a triage failure must stay advisory
        print(f"triage skipped: {exc}", file=sys.stderr)
        return 1 if args.strict else 0

    if args.json:
        print_json(response, state, args.log)
    else:
        print_card(response, state, args.log)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
