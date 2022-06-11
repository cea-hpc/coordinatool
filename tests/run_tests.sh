#!/bin/bash

error() {
	printf "%s\n" "$@" >&2
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
FAILURES=0
TESTS=0
SKIPS=0
ONLY=${ONLY:-}
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
		if ! [[ $caseno =~ ^$ONLY$ ]]; then
			((SKIPS++))
			return
		fi
	fi

	echo -n "Running $caseno: $testcase..."
	(
		set -e
		declare -a CLEANUP=( )
		trap cleanup EXIT
		"$testcase"
	)
	status="$?"
	if ((status == SKIP_RC)); then
		echo SKIP
		((SKIPS++))
	elif ((status)); then
		echo FAIL
		((FATAL)) && exit 1
		((FAILURES++))
	else
		echo Ok
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

	indices=( ${!CLEANUP[@]} )
	for ((i=${#indices[@]} - 1; i >= 0; i--)); do
		eval "${CLEANUP[indices[i]]}"
	done

	return "$ret"
}

#### plain action helpers
do_coordinatool_start() {
	local i="$1"
	# This is a bit misleading, CTOOL_ENV should be a _string_ that looks
	# like an associative array definition e.g. "( [COORDINATOOL_CONF]=/path )"
	# This allows passing multiple arguments, with well defined escaping rules
	declare -A CTOOL_ENV=${CTOOL_ENV:-( )}
	local env="" var

	for var in "${!CTOOL_ENV[@]}"; do
		env+=" -E $var=${CTOOL_ENV[$var]@Q}"
	done

	do_client "$i" "
		systemd-run -P -G --unit=ctest_coordinatool@${i}.service $env \
		${BUILDDIR@Q}/lhsmd_coordinatool -vv MNTPATH
		" &
	CLEANUP+=( "wait $!" "do_coordinatool_service $i stop" )
}

do_coordinatool_service() {
	local i="$1"
	local action="$2"

	do_client "$i" "systemctl $action ctest_coordinatool@${i}.service" || :
}

do_lhsmtoolcmd_start() {
	local i="$1"
	shift
	local LHSMCMD_CONF="${LHSMCMD_CONF:-${SOURCEDIR}/tests/lhsm_cmd.conf}"
	# see coordinatool_start commment for CTOOL_ENV for usage (string -> assoc array)
	declare -A AGENT_ENV=${AGENT_ENV:-( )}
	local env="" var

	if [ -z "${AGENT_ENV[COORDINATOOL_CLIENT_ID]}" ]; then
		# add default client id unless set
		AGENT_ENV[COORDINATOOL_CLIENT_ID]="agent_$i"
	fi

	for var in "${!AGENT_ENV[@]}"; do
		env+=" -E $var=${AGENT_ENV[$var]@Q}"
	done

	do_client "$i" "
		rm -rf ${ARCHIVEDIR@Q} && mkdir ${ARCHIVEDIR@Q}
		systemd-run -P -G --unit=ctest_lhsmtool_cmd@$i.service $env \
			-E LD_PRELOAD=${ASAN:+${ASAN}:}${BUILDDIR@Q}/libcoordinatool_client.so \
			${BUILDDIR@Q}/tests/lhsmtool_cmd -vv \
				--config ${LHSMCMD_CONF@Q} \
				MNTPATH ${*@Q}
		" &
	CLEANUP+=( "wait $!" "do_lhsmtoolcmd_service $i stop" )
}

do_lhsmtoolcmd_service() {
	local i="$1"
	local action="$2"

	do_client "$i" "systemctl $action ctest_lhsmtool_cmd@${i}.service" || :
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
		mkdir ${TESTDIR@Q}
		"
	CLEANUP+=( "rm -rf ${TESTDIR@Q}" )
}

client_archive_n_req() {
	local i="$1"
	local n="$2"

	do_client "$i" "
		cd ${TESTDIR@Q}
		for i in {1..$n}; do
			echo foo.\$i > file.\$i
			lfs hsm_archive file.\$i
		done
		"
}

client_archive_n_wait() {
	local i="$1"
	local n="$2"
	local TMOUT="${TMOUT:-100}"

	do_client "$i" "
		cd ${TESTDIR@Q}
		TMOUT=$TMOUT
		while sleep 0.1; ((TMOUT-- > 0)); do
			for i in {1..$n}; do
				lfs hsm_state file.\$i | grep -q archived || continue 2
			done
			break
		done
		if ((TMOUT <= 0)); then echo 'Failed to archive'; exit 1; fi
	"
}

client_archive_n() {
	client_archive_n_req "$@"
	client_archive_n_wait "$@"
}

client_restore_n() {
	local i="$1"
	local n="$2"
	local TMOUT="${TMOUT:-100}"

	do_client "$i" "
		cd ${TESTDIR@Q}
		for i in {1..$n}; do
			lfs hsm_release file.\$i
		done
		for i in {1..$n}; do
			lfs hsm_restore file.\$i
		done
		TMOUT=$TMOUT
		while sleep 0.1; ((TMOUT-- > 0)); do
			for i in {1..$n}; do
				lfs hsm_state file.\$i | grep -q released && continue 2
			done
			break
		done
		if ((TMOUT <= 0)); then echo 'Failed to restore'; exit 1; fi
		for i in {1..$n}; do
			if ! [[ \"\$(cat file.\$i)\" = foo.\$i ]]; then
				echo 'Content does not match after restore'
				exit 1
			fi
		done
	"
}

client_remove_n() {
	local i="$1"
	local n="$2"
	local TMOUT="${TMOUT:-100}"

	do_client "$i" "
		cd ${TESTDIR@Q}
		for i in {1..$n}; do
			lfs hsm_remove file.\$i
		done
		TMOUT=$TMOUT
		while sleep 0.1; ((TMOUT-- > 0)); do
			for i in {1..$n}; do
				lfs hsm_state file.\$i | grep -q archived && continue 2
			done
			break
		done
		if ((TMOUT <= 0)); then echo 'Failed to remove'; exit 1; fi
	"
}

mds_requeue_active_requests() {
	local i="$1"

	do_mds "$i" "lctl get_param mdt.MDT.hsm.active_requests | \
			${BUILDDIR@Q}/coordinatool-client -Q"
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
		do_client $i "df -t lustre MNTPATH >/dev/null" \
			|| error "${CLIENT[i]}:${MNTPATH[i]} not mounted"
		do_client $i "touch ${TESTDIR@Q} >/dev/null" \
			|| error "No sudo or cannot touch ${MNTPATH[i]}/.test on ${CLIENT[i]}"
		do_client $i "stat ${BUILDDIR@Q}/lhsmd_coordinatool > /dev/null" \
			|| error "$BUILDDIR not a build dir or not accessible on ${CLIENT[i]}"
		do_client $i "stat ${SOURCEDIR@Q}/tests/lhsm_cmd.conf > /dev/null" \
			|| error "$SOURCEDIR not a source dir or not accessible on ${CLIENT[i]}"
		do_coordinatool_service $i stop 2>/dev/null || :
		do_lhsmtoolcmd_service $i stop 2>/dev/null || :
	done


	[[ "${#MDS[@]}" = "${#MDT[@]}" ]] \
		|| error "MDSs and MDTs number don't match"

	for i in {0..1}; do
		[[ "$(do_mds $i "lctl get_param mdt.MDT.hsm_control")" =~ =enabled$ ]] \
			|| error "hsm not enabled on mdt $i (or no sudo on mds)"
		# seems like agents stay registered for a while, but this does not seem
		# to bother tests... ignore for now
		#[[ -z "$(do_mds $i "lctl get_param mdt.MDT.hsm.agents")" ]] \
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
}
run_test 01 normal_requests

# coordinatool restart with actions queued but no active requests on agents (no agent)
server_restart_parse_active_requests() {
	do_coordinatool_start 0

	# start only coordinatool with no agent to queue a request

	client_reset 3
	client_archive_n_req 3 100

	# wait for server to have processed requests, then flush cached data
	# XXX add option to run without redis instead
	sleep 1
	redis-cli hdel coordinatool_requests
	redis-cli hdel coordinatool_assigned

	do_coordinatool_service 0 restart

	# make sure service really restarted before requeueing active requests
	sleep 1
	mds_requeue_active_requests 0
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

	# XXX can lower once clients have expoential backoff reconnect
	# but better would be to wait for ehlo in logs somehow?...
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


echo "Summary: ran $TESTS tests, $SKIPS skipped, $FAILURES failures"

exit $FAILURES

