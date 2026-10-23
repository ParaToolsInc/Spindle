#!/usr/bin/env bash

# Usage:
#   ./run_startup_fault_tests.sh [--launcher=serial|flux|slurm|slurm-plugin]
#                                [--nodes=N] [--cases=LIST] [--session]
#
#   --launcher:   specify launcher to use (default TEST_RESOURCE_MANAGER)
#   --nodes=N:    run on N nodes (required for launchers other than serial)
#   --cases=list: list of tests to run (default all)
#   --session:    run session tests instead of standard tests (requires flux)
#
# Injects startup faults into the Spindle servers and checks that startup
# ends with a nonzero status within a deadline instead of hanging.

set -u

LAUNCHER="TEST_RESOURCE_MANAGER"
NODES=""
CASES=""
SESSION=0
SPINDLE="${SPINDLE:-SPINDLE_EXEC}"
SPINDLE_RC="${SPINDLE_RC:-SPINDLE_RC_PATH}"
TESTDIR="${TESTDIR:-TEST_RUN_DIR}"

TIMELIMIT="${STARTUP_FAULT_TIMELIMIT:-20}"
CLIENT_WAIT=60
BOUND=$((3 * TIMELIMIT + CLIENT_WAIT + 30))
# Launchers may return before every task's bounded client wait has ended
LEFTOVER_WAIT=$(( (TIMELIMIT > CLIENT_WAIT ? TIMELIMIT : CLIENT_WAIT) + 5 ))
PORT_LO="${SPINDLE_TEST_PORT_LO:-21940}"
PORT_HI="${SPINDLE_TEST_PORT_HI:-22189}"

PASSED=0
FAILED=0

die() { echo "ERROR: $*" >&2; exit 1; }
pass() { echo "PASS: $1"; PASSED=$((PASSED + 1)); }
fail() { echo "FAIL: $1"; FAILED=$((FAILED + 1)); }

# Table of test cases to run
# Special values in environment variables:
#   @MID@:       an interior rank that has children
#   @LAST@:      the last rank
#   @LAST_HOST@: the hostname of the last node
#   @HALF@:      half the time limit
CASES_TABLE=(
# name             ; environment variables                                          ; expected result
 'baseline         ;                                                                ; success'
 'delay-rank-tree  ; SPINDLE_TEST_BE_STALL=@MID@ SPINDLE_TEST_BE_STALL_SECS=@HALF@  ; success'
 'exit-host-early  ; SPINDLE_TEST_BE_EXIT_HOSTS=@LAST_HOST@                         ; failure'
 'exit-rank-tree   ; SPINDLE_TEST_BE_EXIT=@MID@                                     ; failure'
 'stall-rank-tree  ; SPINDLE_TEST_BE_STALL=@MID@                                    ; failure'
 'exit-rank-ready  ; SPINDLE_TEST_BE_EXIT=@LAST@ SPINDLE_TEST_BE_FAULT_PHASE=ready  ; failure'
 'stall-rank-ready ; SPINDLE_TEST_BE_STALL=@LAST@ SPINDLE_TEST_BE_FAULT_PHASE=ready ; failure'
)

trim() { local s="$1"; s="${s#"${s%%[![:space:]]*}"}"; echo "${s%"${s##*[![:space:]]}"}"; }

parse_args() {
   for a in "$@"; do
      case "$a" in
         --launcher=*) LAUNCHER="${a#*=}" ;;
         --nodes=*)    NODES="${a#*=}" ;;
         --cases=*)    CASES="${a#*=}" ;;
         --session)    SESSION=1 ;;
         --help|-h)    sed -n '3,/^$/s/^# \{0,1\}//p' "$0"; exit 0 ;;
         *) die "unknown argument '$a'" ;;
      esac
   done
   case "$LAUNCHER" in
      serial) NODES=1 ;;
      flux|slurm|slurm-plugin)
         [[ "$NODES" =~ ^[1-9][0-9]*$ ]] || die "--nodes=N required"
         [ "$NODES" -ge 3 ] || die "--nodes must be at least 3 so that @MID@ has children" ;;
      *) die "unknown launcher '$LAUNCHER'" ;;
   esac
   if (( SESSION )) && [ "$LAUNCHER" != flux ]; then
      die "--session requires --launcher=flux"
   fi
}

# True if case was explicitly selected or all cases are enabled
selected() { [ -z "$CASES" ] || [[ ",$CASES," == *",$1,"* ]]; }

# True if the exit code indicates the process was killed by timeout
hung() { [ "$1" -eq 124 ] || [ "$1" -eq 137 ]; }

# Run a bash command on every node of the allocation
on_all_nodes() {
   case "$LAUNCHER" in
      serial)             bash -c "$1" ;;
      flux)               flux exec -r all bash -c "$1" ;;
      slurm|slurm-plugin) salloc -N"$NODES" -n"$NODES" srun bash -c "$1" 2>/dev/null ;;
   esac
}

# Get hostname of last node of the allocation
last_host() {
   case "$LAUNCHER" in
      serial)             hostname ;;
      flux)               flux exec -r $((NODES - 1)) hostname ;;
      slurm|slurm-plugin) salloc -N"$NODES" -n"$NODES" \
                             srun -N1 -n1 --relative=$((NODES - 1)) hostname 2>/dev/null ;;
   esac
}

# Count TCP sockets listening on port range
count_listeners() {
   local n=0 local_addr st port
   while read -r _ local_addr _ st _; do
      [ "$st" = 0A ] || continue
      port=$((16#${local_addr##*:}))
      [ "$port" -ge "$1" ] && [ "$port" -le "$2" ] && n=$((n + 1))
   done < <(tail -n +2 /proc/net/tcp; tail -n +2 /proc/net/tcp6 2>/dev/null)
   echo "$n"
}

    # Count spindle_bootstrap processes
count_bootstraps() {
   pgrep -c -f "[s]pindle_bootstrap" || true
}

# Count Spindle server processes
count_servers() {
   pgrep -c -x spindle_be || true
}

# True if the session is still published and has a server on every node.
# A job run after the session has gone falls back to a non-session launch
# and succeeds.
session_alive() {
   local n
   flux kvs namespace list | grep -qw spindle || return 1
   n=$(on_all_nodes "$(declare -f count_servers); count_servers" | sum_lines)
   [ "$n" -eq "$NODES" ]
}

# End a session.  Flux removes the session's KVS namespace asynchronously
# (about 1s), and a session started before then can't publish its settings.
end_session() {
   "$SPINDLE" "--end-session${1:+=$1}" >/dev/null 2>&1
   sleep 3
}

# Create FIFOs for a client that never connects: make_stale_fifos PID SUFFIX...
make_stale_fifos() {
   local d pid=$1 s
   shift
   find "${TMPDIR:-/tmp}" -maxdepth 4 -type d -name spindle_comm 2>/dev/null |
      while read -r d; do
         for s in "$@"; do mkfifo "$d/fifo-$pid-$s"; done
      done
}

# Sum the numbers read one per line; other lines (launcher noise) are ignored
sum_lines() {
   local line sum=0
   while read -r line || [ -n "$line" ]; do
      [[ "$line" =~ ^[0-9]+$ ]] && sum=$((sum + 10#$line))
   done
   echo "$sum"
}

set_launch_cmd() {
   case "$LAUNCHER" in
      serial)       CMD=("$SPINDLE" --no-mpi -- /bin/true) ;;
      flux)         CMD=(flux run -o userrc="$SPINDLE_RC" -o spindle -N"$NODES" -n"$NODES" -- /bin/true) ;;
      slurm)        CMD=(salloc -N"$NODES" -n"$NODES" "$SPINDLE" -- srun /bin/true) ;;
      slurm-plugin) CMD=(salloc -N"$NODES" -n"$NODES" srun --spindle /bin/true) ;;
   esac
}

substitute() {
   local s="$1"
   s="${s//@MID@/$MID}"
   s="${s//@LAST@/$LAST}"
   s="${s//@HALF@/$((TIMELIMIT / 2))}"
   s="${s//@LAST_HOST@/$LAST_HOST}"
   echo "$s"
}

check_leftovers() {
   local name="$1" n
   sleep "$LEFTOVER_WAIT"
   n=$(on_all_nodes "$(declare -f count_listeners); count_listeners $PORT_LO $PORT_HI" | sum_lines)
   [ "$n" -eq 0 ] || { fail "$name: $n server(s) still waiting for a parent"; return 1; }
   n=$(on_all_nodes "$(declare -f count_bootstraps); count_bootstraps" | sum_lines)
   [ "$n" -eq 0 ] || { fail "$name: $n spindle_bootstrap process(es) left running"; return 1; }
   return 0
}

run_case() {
   local name="$1" envs="$2" expect="$3" out rc start elapsed
   local -a case_env
   read -ra case_env <<< "$envs"
   out="$SCRATCH/$name.log"
   start=$SECONDS
   timeout -k 10 "$BOUND" env COBO_CONNECT_TIMELIMIT="$TIMELIMIT" SPINDLE_TEST_BE_STALL_SECS="$BOUND" \
      "${case_env[@]}" "${CMD[@]}" >"$out" 2>&1
   rc=$?
   elapsed=$((SECONDS - start))
   if hung $rc; then
      fail "$name: hung (killed after ${BOUND}s, log $out)"; return
   fi
   if [ "$expect" = success ]; then
      [ $rc -eq 0 ] || { fail "$name: expected success, got rc=$rc (log $out)"; return; }
      pass "$name (${elapsed}s)"; return
   fi
   [ $rc -ne 0 ] || { fail "$name: startup failure reported as success (log $out)"; return; }
   if [ "$LAUNCHER" = flux ] && ! grep -q "spindleInitFE error" "$out"; then
      fail "$name: FE did not report the failure (log $out)"; return
   fi
   check_leftovers "$name" || return
   pass "$name (rc=$rc, ${elapsed}s)"
}

# A session whose server on the last node exits before listening must fail to start
session_exit_host_early() {
   local name=session-exit-host-early out rc sid
   out="$SCRATCH/$name.log"
   COBO_CONNECT_TIMELIMIT="$TIMELIMIT" SPINDLE_TEST_BE_EXIT_HOSTS="$LAST_HOST" \
      timeout -k 10 "$BOUND" "$SPINDLE" --start-session >"$out" 2>"$out.err"
   rc=$?
   if hung $rc; then
      fail "$name: --start-session hung (log $out.err)"
   elif [ $rc -eq 0 ]; then
      sid=$(head -n 1 "$out")
      end_session "$sid"
      fail "$name: --start-session reported success (log $out.err)"
   else
      check_leftovers "$name" && pass "$name (rc=$rc)"
   fi
}

# A client that died mid-connect must not block the session's servers:
# session_stale_fifo NAME PID SUFFIX...
session_stale_fifo() {
   local name=$1 out rc sid alive
   shift
   out="$SCRATCH/$name.log"
   sid=$("$SPINDLE" --start-session 2>"$SCRATCH/$name-start.log") || {
      fail "$name: could not start session"; return; }
   on_all_nodes "$(declare -f make_stale_fifos); make_stale_fifos $*" >/dev/null 2>&1
   timeout -k 10 "$BOUND" "${CMD[@]}" >"$out" 2>&1
   rc=$?
   session_alive; alive=$?
   end_session "$sid"
   if [ $rc -ne 0 ]; then
      fail "$name: rc=$rc (log $out)"
   elif [ $alive -ne 0 ]; then
      fail "$name: session servers did not survive (log $out)"
   else
      pass "$name"
   fi
}

# A session left idle past every startup wait must still run a job
session_idle() {
   local name=session-idle out rc sid alive
   local idle=$(( (TIMELIMIT > CLIENT_WAIT ? TIMELIMIT : CLIENT_WAIT) + 10 ))
   out="$SCRATCH/$name.log"
   sid=$(COBO_CONNECT_TIMELIMIT="$TIMELIMIT" "$SPINDLE" --start-session 2>"$SCRATCH/$name-start.log") || {
      fail "$name: could not start session"; return; }
   sleep "$idle"
   timeout -k 10 "$BOUND" "${CMD[@]}" >"$out" 2>&1
   rc=$?
   session_alive; alive=$?
   end_session "$sid"
   if [ $rc -ne 0 ]; then
      fail "$name: rc=$rc after ${idle}s idle (log $out)"
   elif [ $alive -ne 0 ]; then
      fail "$name: session servers did not survive ${idle}s idle (log $out)"
   else
      pass "$name (idle ${idle}s)"
   fi
}

parse_args "$@"
set_launch_cmd
MID=$((NODES / 2))
LAST=$((NODES - 1))
LAST_HOST=$(last_host)
[ -n "$LAST_HOST" ] || die "could not determine the last node's hostname"
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/startup_fault.XXXXXX") || die "can't create scratch dir"
cd "$TESTDIR" || die "can't cd to $TESTDIR"

if (( SESSION )); then
   selected session-exit-host-early && session_exit_host_early
   selected session-stale-fifo && session_stale_fifo session-stale-fifo 999999 0 1
   selected session-stale-fifo1 && session_stale_fifo session-stale-fifo1 999998 1
   selected session-idle && session_idle
else
   for entry in "${CASES_TABLE[@]}"; do
      IFS=';' read -r name envs expect <<< "$entry"
      name=$(trim "$name"); envs=$(trim "$envs"); expect=$(trim "$expect")
      selected "$name" || continue
      run_case "$name" "$(substitute "$envs")" "$expect"
   done
fi

echo "$PASSED passed, $FAILED failed"
[ $FAILED -eq 0 ] && [ $PASSED -gt 0 ]
