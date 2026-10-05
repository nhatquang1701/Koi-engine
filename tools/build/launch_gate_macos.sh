#!/usr/bin/env bash
# Launch tools/build/speed_gate.ps1 on macOS arm64 so a multi-hour gate survives
# the SSH session that started it and the machine does not idle-sleep mid-run.
#
# Usage (from anywhere in the repository):
#   tools/build/launch_gate_macos.sh \
#     -BaselineExecutable build/<name>-baseline-release/koi-bench \
#     -CandidateExecutable build/release/koi-bench \
#     -OutputDirectory artifacts/verification/speed-gate/<name> \
#     -Runs 5 -Threads 1 -DepthSweep 2..5 \
#     -FenFile artifacts/verification/speed-gate/<name>/sparse-qsearch-fens.txt
#
# -OutputDirectory is required so the wrapper knows where to place the logs;
# it is passed through to speed_gate.ps1 unchanged.  The wrapper returns
# immediately and writes gate.out.log, gate.err.log, gate.pid, and (when the
# gate exits) gate-status.txt into that directory.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
gate_script="$script_dir/speed_gate.ps1"

if ! command -v pwsh >/dev/null 2>&1; then
    echo "error: pwsh (PowerShell 7) is required; install with: brew install powershell" >&2
    exit 2
fi
if ! command -v caffeinate >/dev/null 2>&1; then
    echo "error: caffeinate is required to keep the machine awake during the gate" >&2
    exit 2
fi

output_dir=""
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do
    case "${args[$i]}" in
        -OutputDirectory | -outputdirectory)
            output_dir="${args[$((i + 1))]:-}"
            ;;
    esac
done
if [[ -z "$output_dir" ]]; then
    echo "error: pass -OutputDirectory <dir> so the gate logs have a home" >&2
    exit 2
fi
if [[ "$output_dir" != /* ]]; then
    output_dir="$repo_root/$output_dir"
fi
mkdir -p "$output_dir"

status_file="$output_dir/gate-status.txt"
rm -f "$status_file"

# The inner bash records the pwsh exit code (the wrapper itself has already
# exited, so it cannot wait on the gate).  caffeinate -i prevents idle sleep;
# nohup detaches the gate from the SSH session's SIGHUP.
cd "$repo_root"
nohup bash -c '
    gate="$1"
    status="$2"
    shift 2
    caffeinate -i pwsh -NoProfile -File "$gate" "$@"
    echo "EXITCODE $?" > "$status"
' _ "$gate_script" "$status_file" "$@" \
    > "$output_dir/gate.out.log" 2> "$output_dir/gate.err.log" &
gate_pid=$!
printf '%s\n' "$gate_pid" > "$output_dir/gate.pid"

echo "gate launched pid=$gate_pid"
echo "output_directory=$output_dir"
echo "logs: $output_dir/gate.out.log / gate.err.log"
echo "status: $output_dir/gate-status.txt (written when the gate exits)"
