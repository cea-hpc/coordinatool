#!/bin/bash

# ignore silly warnings: cannot source file, CLEANUP modified in subshell
# and test functions unreachable.
# shellcheck disable=SC1091,SC2030,SC2031,SC2317

error() {
	printf "ERROR: %s\n" "$@" >&2
	if [[ "$-" = *i* ]]; then
		return 1;
	else
		exit 1
	fi
}

REPO_ROOT=$(git rev-parse --show-toplevel) \
	|| error "Must run within git repository"
SKIP_RC=163
FATAL=0
FAILURES=()
TESTS=0
SKIPS=0
ONLY=${ONLY:-}
SKIP=${SKIP:-06}
SLEEP_FAIL=${SLEEP_FAIL:-}
ASAN=
. "${REPO_ROOT}/tests/tests_config.sh"
if [[ -e "${REPO_ROOT}/tests/tests_config.local.sh" ]]; then
	. "${REPO_ROOT}/tests/tests_config.local.sh"
fi

# helpers
do_client() {
	local i="$1"
	local sh_opts="e"
	shift

	[[ -n "$i" ]] && [[ -n "${CLIENT[i]}" ]] \
		|| error "do_client: CLIENT[$i] not set" \
		|| return

	set -- "${@//MNTPATH/${MNTPATH[i]}}"
	if [[ "$-" = *x* ]]; then
		sh_opts+="x"
	fi
	if [[ "${CLIENT[i]}" = localhost ]]; then
		sudo sh -${sh_opts}c "$@"
	else
		ssh "${CLIENT[i]}" sudo sh -${sh_opts}c "${@@Q}"
	fi
}

do_mds() {
	local i="$1"
	local sh_opts="e"
	shift

	[[ -n "$i" ]] && [[ -n "${MDS[i]}" ]] \
		|| error "do_mds: MDS[$i] not set" \
		|| return

	set -- "${@//MDT/${MDT[i]}}"
	if [[ "$-" = *x* ]]; then
		sh_opts+="x"
	fi
	if [[ "${MDS[i]}" = localhost ]]; then
		sudo sh -${sh_opts}c "$@"
	else
		ssh "${MDS[i]}" sudo sh -${sh_opts}c "${@@Q}"
	fi
}

run_test() {
	local caseno="$1"
	local testcase="$2"
	local status

	((TESTS++))

	if [[ -n "$ONLY" ]]; then
		case ",$ONLY," in
		*,"$caseno",*) ;;
		*)
			echo "Skipped $caseno: $testcase..."
			((SKIPS++))
			return
			;;
		esac
	else
		case ",$SKIP," in
		*,"$caseno",*)
			echo "Skipped $caseno: $testcase..."
			((SKIPS++))
			return
			;;
		esac
	fi

	echo -n "Running $caseno: $testcase..."
	(
		set -e
		declare -a CLEANUP=( )
		trap cleanup EXIT
		"$testcase"
	) 3>&1 > "$REPO_ROOT/tests/logs.$caseno" 2>&1
	status="$?"
	if ((status == SKIP_RC)); then
		echo " SKIP"
		((SKIPS++))
	elif ((status)); then
		echo " FAIL"
		((FATAL)) && exit 1
		FAILURES+=( "$caseno: $testcase" )
	else
		echo " Ok"
	fi
	return "$status"
}

cleanup() {
	# preserve return code
	local ret="$?"
	declare -a indices

	# unset trap if run voluntarly, no more -e
	trap - EXIT
	set +e

	# wait if requested
	if [ "$ret" != 0 ] && [ -n "$SLEEP_FAIL" ]; then
		set +x
		touch /tmp/test_failed_sleep
		printf "%s\n" "" \
			"Test failed, inspect $REPO_ROOT/tests/logs.$caseno" \
			"and ${TESTDIR//MNTPATH/${MNTPATH[0]}}" \
			"Remove /tmp/test_failed_sleep when done" >&3
		while [ -e /tmp/test_failed_sleep ]; do
			sleep 1
		done
	fi

	redis_clear_db

	indices=( "${!CLEANUP[@]}" )
	for ((i=${#indices[@]} - 1; i >= 0; i--)); do
		eval "${CLEANUP[indices[i]]}"
	done

	return "$ret"
}

redis_clear_db() {
	redis-cli del coordinatool_requests
	redis-cli del coordinatool_assigned
}

#### plain action helpers
do_coordinatool_start() {
	local i="$1"
	# This is a bit misleading, CTOOL_ENV should be a _string_ that looks
	# like an associative array definition e.g. "( [COORDINATOOL_CONF]=/path )"
	# This allows passing multiple arguments, with well defined escaping rules
	declare -A CTOOL_ENV=${CTOOL_ENV:-( )}
	local CTOOL_CONF=${CTOOL_CONF-$SOURCEDIR/tests/coordinatool_tests_defaults.conf}
	local env="" var

	for var in "${!CTOOL_ENV[@]}"; do
		env+=" -E $var=${CTOOL_ENV[$var]@Q}"
	done

	do_client "$i" "
		systemd-run -P -G --unit=ctest_coordinatool@${i}.service $env \
			${BUILDDIR@Q}/lhsmd_coordinatool -vv \
				${CTOOL_CONF:+ --config ${CTOOL_CONF@Q} }MNTPATH
		" &
	CLEANUP+=( "wait $!" "do_coordinatool_service $i stop" )
}

do_coordinatool_service() {
	local i="$1"
	local action="$2"

	do_client "$i" "systemctl --no-pager $action ctest_coordinatool@${i}.service"
}

do_lhsmtoolcmd_start() {
	local i="$1"
	shift
	local LHSMCMD_CONF="${LHSMCMD_CONF:-${BUILDDIR}/tests/lhsm_cmd.conf}"
	local ARCHIVEDIR="${ARCHIVEDIR:-/tmp/archive}"
	local WAIT_FILE="${WAIT_FILE:-}"
	local CTDATA_PATH="${CTDATA_PATH:-}"
	local CTOOL_CONF="${CTOOL_CONF-$SOURCEDIR/tests/coordinatool_tests_defaults.conf}"
	# see coordinatool_start comment for CTOOL_ENV for usage (string -> assoc array)
	declare -A AGENT_ENV=${AGENT_ENV:-( )}
	local env="" var

	if [ -z "${AGENT_ENV[COORDINATOOL_CLIENT_ID]}" ]; then
		# add default client id unless set
		AGENT_ENV[COORDINATOOL_CLIENT_ID]="agent_$i"
	fi

	for var in "${!AGENT_ENV[@]}"; do
		env+=" -E $var=${AGENT_ENV[$var]@Q}"
	done

	if [ ! -z "${CTOOL_CONF}" ]; then
		env+=" -E COORDINATOOL_CONF=${CTOOL_CONF@Q}"
	fi

	do_client "$i" "
		rm -rf ${ARCHIVEDIR@Q} && mkdir -p ${ARCHIVEDIR@Q}
		if [ -n ${WAIT_FILE@Q} ]; then rm -f ${WAIT_FILE@Q}; fi
		systemd-run -P -G --unit=ctest_lhsmtool_cmd@$i.service $env \
			-E LD_PRELOAD=${ASAN:+${ASAN}:}${BUILDDIR@Q}/libcoordinatool_client.so \
			-E ARCHIVEDIR=${ARCHIVEDIR@Q} \
			-E WAIT_FILE=${WAIT_FILE@Q} \
			-E CTDATA_PATH=${CTDATA_PATH@Q} \
			${BUILDDIR@Q}/tests/lhsmtool_cmd -vv \
				--config ${LHSMCMD_CONF@Q} \
				MNTPATH ${*@Q}
		" &
	CLEANUP+=( "wait $!" "do_lhsmtoolcmd_service $i stop" )
}

do_lhsmtoolcmd_service() {
	local i="$1"
	local action="$2"

	do_client "$i" "systemctl --no-pager $action ctest_lhsmtool_cmd@${i}.service"
}

do_coordinatool_client() {
	local i="$1"
	shift

	do_client "$i" "${BUILDDIR@Q}/coordinatool-client ${*@Q}"
}

#### complex action helpers
client_reset() {
	local i="$1"

	do_client "$i" "
		rm -rf ${TESTDIR@Q}
		lfs mkdir -c -1 ${TESTDIR@Q}
		"
	CLEANUP+=( "rm -rf ${TESTDIR@Q}" )
}

client_archive_n_req() {
	local i="$1"
	local n="$2"
	local start=${3:-1}
	local archive_data="${archive_data:-}"
	local archive_id="${archive_id:-}"

	do_client "$i" "
		cd ${TESTDIR@Q}
		for i in {$start..$n}; do
			echo foo.\$i > file.\$i
			lfs hsm_archive ${archive_id:+--archive ${archive_id@Q} }${archive_data:+--data ${archive_data@Q} }file.\$i
		done
		"
}

client_archive_n_wait() {
	local i="$1"
	local n="$2"
	local start=${3:-1}
	local TMOUT="${TMOUT:-100}"

	do_client "$i" "
		cd ${TESTDIR@Q}
		TMOUT=$TMOUT
		while sleep 0.1; ((TMOUT-- > 0)); do
			for i in {$start..$n}; do
				[ -e file.\$i ] || continue
				lfs hsm_state file.\$i | grep -q archived || continue 2
			done
			exit
		done
		exit 1
	" || error "Timed out waiting for archives $start..$n"
}

client_archive_n() {
	client_archive_n_req "$@"
	client_archive_n_wait "$@"
}

client_archive_check_none() {
	local i="$1"
	local n="$2"
	local start=${3:-1}

        do_client "$i" "
                cd ${TESTDIR@Q}
                for i in {$start..$n}; do
                        lfs hsm_state file.\$i | grep archived && exit 1
                done
		true
        " || error "Some files of $start..$n were incorrectly archived"
}

client_restore_n_req() {
	local i="$1"
	local n="$2"
	local start=${3:-1}
	local release_data="${release_data:-}"
	local restore_data="${restore_data:-}"

	do_client "$i" "
		cd ${TESTDIR@Q}
		for i in {$start..$n}; do
			lfs hsm_release ${release_data:+--data ${release_data@Q} }file.\$i
		done
		for i in {$start..$n}; do
			lfs hsm_restore ${restore_data:+--data ${restore_data@Q} }file.\$i
		done
		"
}

client_restore_n_wait() {
	local i="$1"
	local n="$2"
	local start=${3:-1}
	local TMOUT="${TMOUT:-100}"

	do_client "$i" "
		cd ${TESTDIR@Q}
		TMOUT=$TMOUT
		while sleep 0.1; ((TMOUT-- > 0)); do
			for i in {$start..$n}; do
				lfs hsm_state file.\$i | grep -q released && continue 2
			done
			break
		done
		if ((TMOUT <= 0)); then echo 'ERROR: Timed out'; exit 1; fi
		for i in {$start..$n}; do
			if ! [[ \"\$(cat file.\$i)\" = foo.\$i ]]; then
				echo 'ERROR: Content does not match after restore'
				exit 1
			fi
		done
	" || error "Failed to restore $start..$n"
}
client_restore_n() {
	client_restore_n_req "$@"
	client_restore_n_wait "$@"
}

client_remove_n() {
	local i="$1"
	local n="$2"
	local start=${3:-1}
	local TMOUT="${TMOUT:-100}"
	remove_data="${remove_data:-}"

	do_client "$i" "
		cd ${TESTDIR@Q}
		for i in {$start..$n}; do
			lfs hsm_remove ${remove_data:+--data ${remove_data@Q} }file.\$i
		done
		TMOUT=$TMOUT
		while sleep 0.1; ((TMOUT-- > 0)); do
			for i in {$start..$n}; do
				lfs hsm_state file.\$i | grep -q archived && continue 2
			done
			break
		done
		if ((TMOUT <= 0)); then echo 'Failed to remove'; exit 1; fi
	"
}

mds_requeue_active_requests() {
	local i="$1"

	do_mds "$i" "lctl get_param -n mdt.MDT.hsm.active_requests \
			| ${BUILDDIR@Q}/coordinatool-client -Q"
}

# init conditional global variables
init() {
	ASAN=$(ldd "$BUILDDIR/tests/lhsmtool_cmd" | grep -oE '/lib.*libasan.so[.0-9]*')
}

init

# if we're being sourced, don't actually run tests
[[ $(caller | cut -d' ' -f1) != "0" ]] && return 0


# sanity checks before we try to run real tests
sanity() {
	local i

	[[ "${#CLIENT[@]}" = "${#MNTPATH[@]}" ]] \
		|| error "client and mntpoints number don't match"

	for i in {0..4}; do
		do_client "$i" "df -t lustre MNTPATH >/dev/null" \
			|| error "${CLIENT[i]}:${MNTPATH[i]} not mounted"
		do_client "$i" "touch ${TESTDIR@Q} >/dev/null" \
			|| error "No sudo or cannot touch ${MNTPATH[i]}/.test on ${CLIENT[i]}"
		do_client "$i" "stat ${BUILDDIR@Q}/lhsmd_coordinatool > /dev/null" \
			|| error "$BUILDDIR not a build dir or not accessible on ${CLIENT[i]}"
		do_client "$i" "stat ${SOURCEDIR@Q}/tests/lhsm_cmd_command.sh > /dev/null" \
			|| error "$SOURCEDIR not a source dir or not accessible on ${CLIENT[i]}"
		do_coordinatool_service "$i" stop 2>/dev/null || :
		do_lhsmtoolcmd_service "$i" stop 2>/dev/null || :
	done


	[[ "${#MDS[@]}" = "${#MDT[@]}" ]] \
		|| error "MDSs and MDTs number don't match"

	for i in {0..1}; do
		[[ "$(do_mds "$i" "lctl get_param mdt.MDT.hsm_control")" =~ =enabled$ ]] \
			|| error "hsm not enabled on mdt $i (or no sudo on mds)"
		# seems like agents stay registered for a while, but this does not seem
		# to bother tests... ignore for now
		#[[ -z "$(do_mds "$i" "lctl get_param mdt.MDT.hsm.agents")" ]] \
		#	|| error "Other hsm agent running on mdt $i"
	done
}
FATAL=1 run_test 00 sanity

# optimal scenario: servers all running, send requests
normal_requests() {
	do_coordinatool_start 0
	do_lhsmtoolcmd_start 1
	do_lhsmtoolcmd_start 2

	# starts all are async here so first request might come in before servers
	# started, but later requests (restore/remove) do test the optimal server running
	# case

	client_reset 3
	client_archive_n 3 100
	client_restore_n 3 100
	client_remove_n 3 100

	do_client 1 "[ \"\$(find ${ARCHIVEDIR@Q} | wc -l)\" = 1 ]" \
		|| error "files not removed?"
}
run_test 01 normal_requests

# coordinatool restart with actions queued but no active requests on agents (no agent)
server_restart_parse_active_requests() {
	CTOOL_ENV="( [COORDINATOOL_REDIS_HOST]='' )" \
		do_coordinatool_start 0

	# start only coordinatool with no agent to queue a request

	client_reset 3
	client_archive_n_req 3 100

	# wait for server to have processed requests, then flush cached data
	sleep 1

	do_coordinatool_service 0 restart

	# make sure service really restarted before requeueing active requests
	sleep 1
	mds_requeue_active_requests 0
	mds_requeue_active_requests 1
	do_lhsmtoolcmd_start 1
	client_archive_n_wait 3 100
}
run_test 02 server_restart_parse_active_requests

# restart coordinatool with actions queued on agent
server_restart_coordinatool_recovery() {
	do_coordinatool_start 0

	# start only coordinatool with no agent to queue some work,
	# then restart it and start an agent

	client_reset 3
	client_archive_n_req 3 100

	# XXX remove sleep once signal handler implemented
	sleep 1

	do_coordinatool_service 0 restart

	do_lhsmtoolcmd_start 1
	client_archive_n_wait 3 100
}
run_test 03 server_restart_coordinatool_recovery

# restart server while agents process data
server_restart_coordinatool_recovery_busy() {
	do_coordinatool_start 0
	do_lhsmtoolcmd_start 1
	do_lhsmtoolcmd_start 2

	client_reset 3
	client_archive_n_req 3 100

	# XXX empirical: xfers aren't yet over in 0.5s...
	sleep 0.5
	do_coordinatool_service 0 restart

	client_archive_n_wait 3 100
	do_lhsmtoolcmd_service 1 status || error "lhsmtool 1 gone"
	do_lhsmtoolcmd_service 2 status || error "lhsmtool 2 gone"
	do_coordinatool_service 0 status || error "coordinatool gone"
}
run_test 04 server_restart_coordinatool_recovery_busy

# restart an agent while it processes data
server_restart_lhsmtoolcmd_busy() {
	do_coordinatool_start 0
	do_lhsmtoolcmd_start 1

	client_reset 3
	client_archive_n_req 3 100

	# XXX empirical: xfers aren't yet over in 0.5s...
	sleep 0.5
	do_lhsmtoolcmd_service 1 restart

	client_archive_n_wait 3 100
}
run_test 05 server_restart_lhsmtoolcmd_busy

# stop an agent processing data and wait grace period
server_stop_lhsmtoolcmd_busy() {
	# grace is in ms
	CTOOL_ENV="( [COORDINATOOL_CLIENT_GRACE]=500 )" \
		do_coordinatool_start 0
	do_lhsmtoolcmd_start 1
	do_lhsmtoolcmd_start 2

	client_reset 3
	client_archive_n_req 3 100

	# XXX empirical: xfers aren't yet over in 0.5s...
	sleep 0.5
	# XXX: this is broken and can loose requests (this test is flaky)
	# the problem happens if the sigterm comes in when lhsmtool_cmd is *in* the real
	# llapi_hsm_action_end:
	# there's a state where lustre cleared the HSM archive request, but the file
	# is not marked as archived (or perhaps the next action_begin/end from replay
	# removes the flag?)
	# This is probably a bug in lhsmtool_cmd signal handler calling _exit(1),
	# it should wait for "quitable" points on sigterm (also possibly kill spawned
	# processes?)
	do_lhsmtoolcmd_service 1 stop

	client_archive_n_wait 3 100
}
run_test 06 server_stop_lhsmtoolcmd_busy

# test archive_id is respected
respect_client_archive_id() {
	do_coordinatool_start 0
	# requests go on archive id 1 by default, so start an agent on
	# others and make sure it does not process anything
	do_lhsmtoolcmd_start 1 -A 2 -A 3

	client_reset 3
	client_archive_n_req 3 5

	# XXX can lower once clients have exponential backoff reconnect
	# but better would be to wait for one coordinatool scheduling pass
	# if/when we ever can
	sleep 4
	do_client 3 "
		cd ${TESTDIR@Q}
		for i in {1..5}; do
			lfs hsm_state file.\$i | grep -q 0x00000001 || exit 1
		done
		"

	# then start a normal one and wait
	do_lhsmtoolcmd_start 2 -A 1
	client_archive_n_wait 3 5
}
run_test 07 respect_client_archive_id

# run without redis
no_redis() {
	CTOOL_ENV="( [COORDINATOOL_REDIS_HOST]='' )" \
		do_coordinatool_start 0
	do_lhsmtoolcmd_start 1

	client_reset 3
	client_archive_n 3 5
}
run_test 08 no_redis

# restart redis while doing transfers
redis_restart() {
	do_coordinatool_start 0
	do_lhsmtoolcmd_start 1

	client_reset 3
	client_archive_n_req 3 100

	# XXX empirical: xfers aren't yet over in 0.5s...
	sleep 0.5
	do_client 0 "systemctl --no-pager restart redis"

	client_archive_n_wait 3 100
	# XXX check redis db is empty after this?
	# pretty sure it won't be...
}
run_test 09 redis_restart

archive_on_host() {
	CTOOL_CONF="$SOURCEDIR"/tests/coordinatool_archive_on_host.conf \
		do_coordinatool_start 0
	ARCHIVEDIR="$ARCHIVEDIR/0" do_lhsmtoolcmd_start 0
	ARCHIVEDIR="$ARCHIVEDIR/1" do_lhsmtoolcmd_start 1
	ARCHIVEDIR="$ARCHIVEDIR/2" do_lhsmtoolcmd_start 2
	ARCHIVEDIR="$ARCHIVEDIR/3" do_lhsmtoolcmd_start 3

	# wait for copytools to connect
	# (otherwise requests aren't scheduled)
	sleep 1
	echo "done waiting"

	# config has:
	#  - tag=n0 -> agent 0
	#  - tag=n1 -> agent 1/2
	#  - tag=n2 -> agent 3/4
	# we check:
	#  - n0 all go to 0
	#  - n1 split between 1/2
	#  - n2 all to 3 (agent 4 not started)

	client_reset 3
	archive_data="tag=n0" client_archive_n_req 3 19 00
	archive_data="ignored,tag=n1" client_archive_n_req 3 39 20
	archive_data="tag=n2,ignored" client_archive_n_req 3 59 40

	client_archive_n_wait 3 59 00

	do_client 0 "[ \"\$(find ${ARCHIVEDIR@Q}/0 | wc -l)\" = 21 ]" \
		|| error "missing archives on 0"

	local nb_client_1=$(find ${ARCHIVEDIR}/1 | wc -l)
	local nb_client_2=$(find ${ARCHIVEDIR}/2 | wc -l)
	if [[ $(($nb_client_1 + $nb_client_2)) -ne 22 ]]; then
		error "missing archives on 1 and 2"
	fi

	do_client 3 "[ \"\$(find ${ARCHIVEDIR@Q}/3 | wc -l)\" = 21 ]" \
		|| error "missing archives on 3"
}
run_test 10 archive_on_host

restarts_with_pending_work() {
	CTOOL_CONF="$SOURCEDIR"/tests/coordinatool_archive_on_host.conf \
		do_coordinatool_start 0
	sleep 0.3
	WAIT_FILE="$ARCHIVEDIR/wait" ARCHIVEDIR="$ARCHIVEDIR/1" do_lhsmtoolcmd_start 1
	WAIT_FILE="$ARCHIVEDIR/wait" ARCHIVEDIR="$ARCHIVEDIR/2" do_lhsmtoolcmd_start 2
	WAIT_FILE="$ARCHIVEDIR/wait" ARCHIVEDIR="$ARCHIVEDIR/3" do_lhsmtoolcmd_start 3

	# wait for copytools to connect
	# (otherwise requests aren't scheduled)
	sleep 0.5
	echo "done waiting"

	# config has:
	#  - tag=n0 -> agent 0
	#  - tag=n1 -> agent 1/2
	#  - tag=n2 -> agent 3/4
	# we check:
	#  - n0: unassigned, will be held back on a disconnected agent_0
	#  - n1: will go to 1/2, stop 1 see what happens (redirected to 2 after 5s)
	#  - n2: will go to 3, restart 3 see what happens (re-sent to itself on reconnect)
	# note that unlike skipped test in 06

	client_reset 3
	archive_data="tag=n0" client_archive_n_req 3 119 100
	archive_data="ignored,tag=n1" client_archive_n_req 3 219 200
	archive_data="tag=n2,ignored" client_archive_n_req 3 319 300

	sleep 0.5

	do_lhsmtoolcmd_service 1 stop
	do_lhsmtoolcmd_service 3 restart
	# XXX wait a bit for restart to finish before we create wait file:
	# if wait file is created before service stop we risk running in
	# the same race as test 06 (stop during llapi_hsm_action_end)
	sleep 0.5
	for client in 2 3; do
		do_client $client "touch ${ARCHIVEDIR@Q}/wait"
	done

	client_archive_n_wait 3 219 200
	client_archive_n_wait 3 319 300

        # archives with tag=n0 should all have been held back
	client_archive_check_none 3 119 100

	do_client 1 "[ \"\$(find ${ARCHIVEDIR@Q}/1 | wc -l)\" = 1 ]" \
		|| error "should be no archive on 1"
	do_client 2 "[ \"\$(find ${ARCHIVEDIR@Q}/2 | wc -l)\" -gt 20 ]" \
		|| error "should be at least 20 archives on 2"
	do_client 3 "[ \"\$(find ${ARCHIVEDIR@Q}/3 | wc -l)\" -gt 20 ]" \
		|| error "should be at least 20 archives on 3"
}
run_test 11 restarts_with_pending_work

lock_and_quit() {
	do_coordinatool_start 0
	WAIT_FILE="$ARCHIVEDIR/wait" do_lhsmtoolcmd_start 1

	client_reset 3

	# send 10 requests and wait a bit for them to be queued to mover.
	# with max_archives=3, we'll have 3 requests in progress (waiting)
	# and 7 queued when we lock
	client_archive_n_req 3 10
	sleep 1
	do_coordinatool_client 0 --lock
	do_client 1 "touch ${ARCHIVEDIR@Q}/wait"
	# now unlock and wait for some files
	sleep 1
	# 3 files + wait + dir
	do_client 1 "[ \"\$(find ${ARCHIVEDIR@Q} | wc -l)\" = 5 ]" \
		|| error "should be exactly 3 files archived"
	do_client 1 "rm -f ${ARCHIVEDIR@Q}/wait"
	do_coordinatool_client 0 --unlock
	# wait for next 3 requests to be sent to mover
	sleep 1
	do_coordinatool_client 0 --lock-quit
	sleep 1
	# service is still running (requests not done)
	do_coordinatool_service 0 status
	do_client 1 "touch ${ARCHIVEDIR@Q}/wait"
	sleep 1

	do_client 1 "[ \"\$(find ${ARCHIVEDIR@Q} | wc -l)\" = 8 ]" \
		|| error "should be exactly 6 files archived"
	! do_coordinatool_service 0 status \
		|| error "service not stopped after all done"
}
run_test 12 lock_and_quit

archive_on_hosts_ch() {
	CTOOL_CONF="$SOURCEDIR"/tests/coordinatool_archive_on_host.conf \
		do_coordinatool_start 0
	sleep 1
	ARCHIVEDIR="$ARCHIVEDIR/1" do_lhsmtoolcmd_start 1
	ARCHIVEDIR="$ARCHIVEDIR/2" do_lhsmtoolcmd_start 2

	# wait for copytools to connect
	# (otherwise requests aren't scheduled)
	sleep 1
	echo "done waiting"

	client_reset 3
	archive_data="grouping=test0" client_archive_n_req 3 19 0
	archive_data="grouping=test1" client_archive_n_req 3 39 20
	archive_data="grouping=test2" client_archive_n_req 3 59 40

	sleep 0.5
	do_lhsmtoolcmd_service 1 restart

	client_archive_check_none 3 19 0
	client_archive_n_wait 3 59 20

	do_client 1 "[ \"\$(find ${ARCHIVEDIR@Q}/1 | wc -l)\" = 21 ]" \
		|| error "missing archives on 1"
	do_client 2 "[ \"\$(find ${ARCHIVEDIR@Q}/2 | wc -l)\" = 21 ]" \
		|| error "missing archives on 2"

	ARCHIVEDIR="$ARCHIVEDIR/0" do_lhsmtoolcmd_start 0
	sleep 1

	client_archive_n_wait 3 19 0
	do_client 0 "[ \"\$(find ${ARCHIVEDIR@Q}/0 | wc -l)\" = 21 ]" \
		|| error "missing archives on 0"
}
run_test 13 archive_on_hosts_ch

# 3x tests: test lfs hsm_* --data
# normal copies
data_normal() {
	do_coordinatool_start 0
	CTDATA_PATH=1 do_lhsmtoolcmd_start 1

	client_reset 3
	# our test lhsmtool_cmd archives at $ARCHIVEDIR/{ctdata}{fid}
	# since it split command's words after expansion we need to escape spaces
	archive_data='some data'
	restore_data="$archive_data"
	remove_data="$archive_data"
	client_archive_n 3 1
	do_client 1 "ls ${ARCHIVEDIR@Q}/some\ data*" \
		|| error "archive data was lost?"
	client_restore_n 3 1
	client_remove_n 3 1

	do_client 1 "[ \"\$(find ${ARCHIVEDIR@Q} | wc -l)\" = 1 ]" \
		|| error "files not removed?"
}
run_test 30 data_normal

# make sure restart from redis works
data_restart() {
	do_coordinatool_start 0

	# start only coordinatool with no agent to queue a request

	client_reset 3
	archive_data='some data'
	client_archive_n_req 3 100

	# wait for server to have processed requests, then flush cached data
	sleep 1

	do_coordinatool_service 0 restart

	# make sure service really restarted before processing requests
	CTDATA_PATH=1 do_lhsmtoolcmd_start 1
	client_archive_n_wait 3 100
	do_client 1 "ls ${ARCHIVEDIR@Q}/some\ data*" \
		|| error "archive data was lost?"
}
run_test 31 data_restart

data_restore_active_requests() {
	CTOOL_ENV="( [COORDINATOOL_REDIS_HOST]='' )" \
		do_coordinatool_start 0

	# start only coordinatool with no agent to queue a request

	client_reset 3
	# XXX lustre only lists up to 5 chars in active_requests:
	# longer data is lost!!!
	# (this test will fail if more is restored)
	archive_data='da ta_anything longer is lost'
	client_archive_n_req 3 100

	# wait for server to have processed requests, then flush cached data
	sleep 1

	do_coordinatool_service 0 restart

	# make sure service really restarted before requeueing active requests
	sleep 1
	mds_requeue_active_requests 0
	mds_requeue_active_requests 1
	CTDATA_PATH=1 do_lhsmtoolcmd_start 1
	client_archive_n_wait 3 100
	do_client 1 "ls ${ARCHIVEDIR@Q}/da\ ta*" \
		|| error "archive data was lost?"
	do_client 1 "! ls ${ARCHIVEDIR@Q}/da\ ta_*" \
		|| error "restore from active_requests restored more than 5 chars?"
}
run_test 32 data_restore_active_requests

# 4x tests: test multiple archive ids
# normal copies
archive_id_normal() {
	do_coordinatool_start 0
	do_lhsmtoolcmd_start 1

	client_reset 3
	# default coordinatool config accepts any archive, so this should work
	# as normal, except we'll see archive_id by checking the file
	archive_id=2
	client_archive_n 3 1

	do_client 3 "
		cd ${TESTDIR@Q}
		lfs hsm_state file.1 | grep archive_id:2
		" || error "archive id wasn't 2"
	client_restore_n 3 1
	client_remove_n 3 1

	do_client 1 "[ \"\$(find ${ARCHIVEDIR@Q} | wc -l)\" = 1 ]" \
		|| error "files not removed?"
}
run_test 40 archive_id_normal

# make sure restart from redis works
archive_id_restart() {
	do_coordinatool_start 0

	# start only coordinatool with no agent to queue a request

	client_reset 3
	archive_id=2
	client_archive_n_req 3 299 200
	archive_id=1
	client_archive_n_req 3 199 100

	# wait for server to have processed requests, then flush cached data
	sleep 1

	do_coordinatool_service 0 restart

	# make sure service really restarted before processing requests
	do_lhsmtoolcmd_start 1
	client_archive_n_wait 3 299 100
	do_client 3 "
		cd ${TESTDIR@Q}
		! lfs hsm_state file.2* | grep -v archive_id:2
		! lfs hsm_state file.1* | grep -v archive_id:1
		" || error "some archive id wasn't as expected"
}
run_test 41 archive_id_restart

archive_id_restore_active_requests() {
	CTOOL_ENV="( [COORDINATOOL_REDIS_HOST]='' )" \
		do_coordinatool_start 0

	# start only coordinatool with no agent to queue a request

	client_reset 3
	archive_id=2
	client_archive_n_req 3 299 200
	archive_id=1
	client_archive_n_req 3 199 100

	# wait for server to have processed requests, then flush cached data
	sleep 1

	do_coordinatool_service 0 restart

	# make sure service really restarted before requeueing active requests
	sleep 1
	mds_requeue_active_requests 0
	mds_requeue_active_requests 1
	do_lhsmtoolcmd_start 1
	client_archive_n_wait 3 299 100
	do_client 3 "
		cd ${TESTDIR@Q}
		! lfs hsm_state file.2* | grep -v archive_id:2
		! lfs hsm_state file.1* | grep -v archive_id:1
		" || error "some archive id wasn't as expected"
}
run_test 42 archive_id_restore_active_requests

# 5x tests: slot batching
# basic test
archive_basic_batch_common() {
	local mover tag1 tag2 tag3 tag4 tag5 count

	for mover; do
		CTDATA_PATH=1 ARCHIVEDIR="$ARCHIVEDIR/$mover" \
			do_lhsmtoolcmd_start $mover
	done

	# test expects 2 slots, idle 10s / max 20s timeouts
	# t 0s:
	# - slot1: send just one request for tag1
	# - slot2: send 10 requests for tag2 every 5s
	# - waiting queue: 10x tag3, 10x tag4, 10x tag5
	# t 10s:
	# - slot1 should expire, tag3(or 4 or 5) comes in
	# - slot2 still busy with tag2
	# t 20s:
	# - slot1 expires tag3 (idle), tag4 comes in
	# - slot2 expires max time for tag2, tag5 comes in
	# t 30s:
	# - either slot pick tag2 and finish work
	#
	# verification done through print manually for now
	tag1="tag=n0"
	tag2="ignored,tag=n1"
	tag3="tag=n2,ignored"
	tag4="tag=n2,different"
	tag5="tag=n1,different"

	client_reset 3
	archive_data="$tag1" client_archive_n_req 3 100 100
	archive_data="$tag2" client_archive_n_req 3 209 200
	# wait a bit to ensure these two get scheduled first
	sleep 1
	archive_data="$tag3" client_archive_n_req 3 309 300
	archive_data="$tag4" client_archive_n_req 3 409 400
	archive_data="$tag5" client_archive_n_req 3 509 500

	sleep 4
	# t=5s, first two batches processed
	archs=$(for mover; do do_client $mover "find ${ARCHIVEDIR@Q}/$mover" || exit; done)
	echo t=5s
	echo ====================
	echo "$archs"
	echo ====================
	archive_data="$tag2" client_archive_n_req 3 219 210
	TMOUT=1 client_archive_n_wait 3 100 100
	TMOUT=1 client_archive_n_wait 3 209 200
	do_client 3 "lfs hsm_state ${TESTDIR@Q}/file.300 | grep -q archived" && error "tag3 already processed"
	do_client 3 "lfs hsm_state ${TESTDIR@Q}/file.400 | grep -q archived" && error "tag4 already processed"
	do_client 3 "lfs hsm_state ${TESTDIR@Q}/file.500 | grep -q archived" && error "tag5 already processed"

	sleep 5
	# t=10s, another tag started but we don't know which yet
	archs=$(for mover; do do_client $mover "find ${ARCHIVEDIR@Q}/$mover" || exit; done)
	echo t=10s
	echo ====================
	echo "$archs"
	echo ====================
	archive_data="$tag2" client_archive_n_req 3 229 220
	TMOUT=1 client_archive_n_wait 3 219 200
	count=0
	sleep 1
	do_client 3 "lfs hsm_state ${TESTDIR@Q}/file.300 | grep -q archived" && count=$((count+1))
	do_client 3 "lfs hsm_state ${TESTDIR@Q}/file.400 | grep -q archived" && count=$((count+1))
	do_client 3 "lfs hsm_state ${TESTDIR@Q}/file.500 | grep -q archived" && count=$((count+1))
	[ "$count" = 1 ] || error "more or less than 1 extra tag processed? $count"

	sleep 4
	# t=15s
	archs=$(for mover; do do_client $mover "find ${ARCHIVEDIR@Q}/$mover" || exit; done)
	echo t=15s
	echo ====================
	echo "$archs"
	echo ====================
	archive_data="$tag2" client_archive_n_req 3 239 230

	sleep 5
	# t=20s
	archs=$(for mover; do do_client $mover "find ${ARCHIVEDIR@Q}/$mover" || exit; done)
	echo t=20s
	echo ====================
	echo "$archs"
	echo ====================
	archive_data="$tag2" client_archive_n_req 3 249 240

	echo done sending, waiting

	client_archive_n_wait 3 800 100
	echo done waiting
	archs=$(for mover; do do_client $mover "find ${ARCHIVEDIR@Q}/$mover" || exit; done)
	echo ====================
	echo "$archs"
	echo ====================
}
archive_basic_batch() {
	CTOOL_CONF="$SOURCEDIR"/tests/coordinatool_batch.conf \
		do_coordinatool_start 0
	archive_basic_batch_common 0 1
}
run_test 50 archive_basic_batch

archive_basic_batch_multislots() {
	CTOOL_CONF="$SOURCEDIR"/tests/coordinatool_batch_multislots.conf \
		do_coordinatool_start 0
	archive_basic_batch_common 0
}
run_test 51 archive_basic_batch_multislots

# This does not pass because we don't allocate enough hosts
#archive_basic_batch_on_hosts() {
#	CTOOL_CONF="$SOURCEDIR"/tests/coordinatool_batch_on_hosts.conf \
#		archive_basic_batch_common
#}
#run_test 52 archive_basic_batch_on_hosts


reporting_basic_test() {
	do_client 0 "rm -rf MNTPATH/.reporting"

	CTOOL_CONF="$SOURCEDIR"/tests/coordinatool_reporting.conf \
		do_coordinatool_start 0

	# need to wait a bit so copytool can connect immeditely...
	sleep 0.5
	client_reset 3

	# we have max_archive=3
	# - send 5 requests to fill in agent 1 waiting queue
	# - wait a bit
	# - start 2nd agent
	# - send 3 more requests with another tag
	# - wait a bit and check
	# - unblock agent 1 & wait
	# - check only first report got cleaned up
	# - unblock agent 2 & wait
	# - chcek all is clean

	WAIT_FILE="$ARCHIVEDIR/wait_1" do_lhsmtoolcmd_start 1
	archive_data="cr=report00" client_archive_n_req 3 04 00

	# wait a bit to get messages...
	sleep 1
	WAIT_FILE="$ARCHIVEDIR/wait_2" do_lhsmtoolcmd_start 2
	# (wait till report00 requests gets sent to agent_2)
	sleep 1
	archive_data="foo,cr=report01,bar" client_archive_n_req 3 12 10

	sleep 1
	do_client 0 "[ \"\$(grep -c agent_1 MNTPATH/.reporting/report00)\" = 3 ]" \
		|| error "Unexpected number of agent_1 in report00"
	do_client 0 "[ \"\$(grep -c agent_2 MNTPATH/.reporting/report00)\" = 2 ]" \
		|| error "Unexpected number of agent_2 in report00"
	do_client 0 "[ \"\$(grep -c agent_2 MNTPATH/.reporting/report01)\" = 1 ]" \
		|| error "Unexpected number of agent_2 in report01"
	do_client 0 "[ \"\$(grep -c new MNTPATH/.reporting/report01)\" = 3 ]" \
		|| error "Unexpected number of new in report01"

	sleep 4
	touch $ARCHIVEDIR/wait_2
	client_archive_n_wait 3 12 10


	do_client 0 'test -e MNTPATH/.reporting/report00' \
		|| error "report01 was incorrectly removed"

	touch $ARCHIVEDIR/wait_1
	client_archive_n_wait 3 04 00

	# wait for delayed unlink
	sleep 3
	do_client 0 'test ! -e MNTPATH/.reporting/report01' \
		|| error "report01 was not removed"
	do_client 0 'test ! -e MNTPATH/.reporting/report00' \
		|| error "report00 was not removed"

}
run_test 60 reporting_basic_test

reporting_normal_requests() {
	# archive data and restore data must match because our archive script
	# prefixes archive by data, so if it doesn't match file is not found..
	CTOOL_CONF="$SOURCEDIR"/tests/coordinatool_reporting.conf \
	archive_data="cr=report01" \
	restore_data="cr=report01" \
	remove_data="cr=report01" \
		normal_requests
}
run_test 61 reporting_normal_requests

reporting_reuse_tags() {
	do_client 0 "rm -rf MNTPATH/.reporting"

	CTOOL_CONF="$SOURCEDIR"/tests/coordinatool_reporting.conf \
		do_coordinatool_start 0

	do_lhsmtoolcmd_start 1
	do_lhsmtoolcmd_start 2

	client_reset 3

	archive_data="cr=report01" \
		client_archive_n_req 3 09 00
	archive_data="cr=report02,foo" \
		client_archive_n_req 3 19 10
	archive_data="foo,cr=report01,test" \
		client_archive_n_req 3 29 20

	client_archive_n_wait 3 29 00
	archive_data="bar,cr=report01" \
		client_archive_n 3 39 30

	# wait for delayed cleanup
	sleep 3
	do_client 0 'test ! -e MNTPATH/.reporting/report01' \
		|| error "report01 was not removed"
	do_client 0 'test ! -e MNTPATH/.reporting/report02' \
		|| error "report02 was not removed"
}
run_test 62 reporting_reuse_tags

reporting_restore_progress() {
	do_client 0 "rm -rf MNTPATH/.reporting"

	CTOOL_CONF="$SOURCEDIR"/tests/coordinatool_reporting.conf \
		do_coordinatool_start 0

	# need to wait a bit so copytool can connect immeditely...
	sleep 0.5
	client_reset 3

	# archive/release some files, then block restores a bit and see how progresses are reported
	WAIT_FILE="$ARCHIVEDIR/wait_1" do_lhsmtoolcmd_start 1
	sleep 0.5
	touch "$ARCHIVEDIR/wait_1"
	client_archive_n 3 09 00

	rm -f "$ARCHIVEDIR/wait_1"
	restore_data="foo,cr=report00,bar" client_restore_n_req 3 09 00

	# wait a bit to get messages...
	sleep 3
	do_client 0 "[ \"\$(grep -c new MNTPATH/.reporting/report00)\" = 10 ]" \
		|| error "Unexpected number of new in report00"
	do_client 0 "[ \"\$(grep -c sent MNTPATH/.reporting/report00)\" = 3 ]" \
		|| error "Unexpected number of sent in report00"
	# more than 10 ensures there are repeats
	do_client 0 "[ \"\$(grep -c progress MNTPATH/.reporting/report00)\" -gt 10 ]" \
		|| error "Unexpected number of progress in report00"

	touch $ARCHIVEDIR/wait_1
	client_restore_n_wait 3 09 00
	do_client 0 "[ \"\$(grep -c done MNTPATH/.reporting/report00)\" = 10 ]" \
		|| error "Unexpected number of done in report00"

	# wait for delayed unlink
	sleep 3
	do_client 0 'test ! -e MNTPATH/.reporting/report00' \
		|| error "report00 was not removed"
}
run_test 63 reporting_restore_progress

echo "Summary: ran $TESTS tests, $SKIPS skipped, ${#FAILURES[@]} failures"
printf "%s\n" "${FAILURES[@]}"

exit ${#FAILURES[@]}
