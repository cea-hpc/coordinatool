#!/bin/bash

error() {
	printf "%s\n" "$@" >&2
	exit 1
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

# helpers
do_client() {
	local i="$1"
	local sh_opts="e"
	shift

	[[ "$i" =~ ^[0-9]+$ ]] || error "do_client: $i must be 0-4"
	(( i < ${#CLIENT[@]} )) || error "do_client: $i must be < ${#CLIENT[@]}"

	set -- "${@//MNTPATH/${MNTPATH[$i]}}"
	if [[ "$-" =~ x ]]; then
		sh_opts+="x"
	fi
	if [[ "${CLIENT[$i]}" = localhost ]]; then
		sudo sh -${sh_opts}c "$@"
	else
		ssh "${CLIENT[$i]}" sudo sh -${sh_opts}c "${@@Q}"
	fi
}

do_mds() {
	local i="$1"
	local sh_opts="e"
	shift

	[[ "$i" =~ ^[0-9]+$ ]] || error "do_mds: $i must be 0-4"
	(( i < ${#MDS[@]} )) || error "do_mds: $i must be < ${#CLIENT[@]}"

	set -- "${@//MDT/${MDT[$i]}}"
	if [[ "$-" =~ x ]]; then
		sh_opts+="x"
	fi
	if [[ "${MDS[$i]}" = localhost ]]; then
		sudo sh -${sh_opts}c "$@"
	else
		ssh "${MDS[$i]}" sudo sh -${sh_opts}c "${@@Q}"
	fi
}

run_test() {
	local testcase="$1" status
	local caseno="$2"

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
		echo "SKIP"
		((SKIPS++))
	elif ((status)); then
		echo FAIL
		((FATAL)) && exit 1
		((FAILURES++))
	else
		echo "Ok"
	fi
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

#### service helpers
start_coordinatool() {
	local i="$1"

	do_client "$i" "
		systemd-run -P -G --unit=coordinatool.service \
		${BUILDDIR@Q}/lhsmd_coordinatool -vv MNTPATH
		" &
	CLEANUP+=( "wait $!" "service_coordinatool $i stop" )
}

service_coordinatool() {
	local i="$1"
	local action="$2"

	do_client "$i" "systemctl $action coordinatool.service" || :
}

start_lhsmtool_cmd() {
	local i="$1"

	do_client "$i" "
		rm -rf ${ARCHIVEDIR@Q} && mkdir ${ARCHIVEDIR@Q}
		systemd-run -P -G --unit=lhsmtool_cmd@$i.service \
			-E LD_PRELOAD=${ASAN:+${ASAN}:}${BUILDDIR@Q}/libcoordinatool_client.so \
			${BUILDDIR@Q}/tests/lhsmtool_cmd -vv \
				--config ${SOURCEDIR@Q}/tests/lhsm_cmd.conf \
				MNTPATH
		" &
	CLEANUP+=( "wait $!" "service_lhsmtool_cmd $i stop" )
}

service_lhsmtool_cmd() {
	local i="$1"
	local action="$2"

	do_client "$i" "systemctl $action lhsmtool_cmd@${i}.service" || :
}


#### client helpers
client_setup() {
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
	local TMOUT="${TMOUT:-10}"

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

# init conditional global variables
init() {
	ASAN=$(ldd "$BUILDDIR/tests/lhsmtool_cmd" | grep -oE '/lib.*libasan.so[.0-9]*')
}

# sanity checks before we try to run real tests
sanity() {
	local i

	[[ "${#CLIENT[@]}" = "${#MNTPATH[@]}" ]] \
		|| error "client and mntpoints number don't match"

	for i in {0..4}; do
		do_client $i "df -t lustre MNTPATH >/dev/null" \
			|| error "${CLIENT[$i]}:${MNTPATH[$i]} not mounted"
		do_client $i "touch ${TESTDIR@Q} >/dev/null" \
			|| error "No sudo or cannot touch ${MNTPATH[$i]}/.test on ${CLIENT[$i]}"
		do_client $i "stat ${BUILDDIR@Q}/lhsmd_coordinatool > /dev/null" \
			|| error "$BUILDDIR not a build dir or not accessible on client $i"
		do_client $i "stat ${SOURCEDIR@Q}/tests/lhsm_cmd.conf > /dev/null" \
			|| error "$SOURCEDIR not a source dir or not accessible on client $i"
		do_client $i "systemctl stop coordinatool.service 2>/dev/null
			      systemctl stop lhsmtool_cmd@*.service 2>/dev/null" || :
	done


	[[ "${#MDS[@]}" = "${#MDT[@]}" ]] \
		|| error "client and mntpoints number don't match"

	for i in {0..1}; do
		[[ "$(do_mds $i "lctl get_param mdt.MDT.hsm_control")" =~ =enabled$ ]] \
			|| error "hsm not enabled on mdt $i (or no sudo on mds)"
		# seems like agents stay registered for a while, but this does not seem
		# to bother tests... ignore for now
		#[[ -z "$(do_mds $i "lctl get_param mdt.MDT.hsm.agents")" ]] \
		#	|| error "Other hsm agent running on mdt $i"
	done
}

# optimal scenario: servers all running, send requests
normal_requests() {
	start_coordinatool 0
	start_lhsmtool_cmd 1
	start_lhsmtool_cmd 2

	# starts all are async here so first request might come in before servers
	# started, but later requests (restore/remove) do test the optimal server running
	# case

	client_setup 3
	client_archive_n 3 100
	client_restore_n 3 100
	client_remove_n 3 100
}

# coordinatool restart with actions queued but no active requests on agents (no agent)
server_restart_no_agent_active_requests() {
	start_coordinatool 0

	# start only coorinatool with no agent to queue a request

	client_setup 3
	client_archive_n_req 3 100

	service_coordinatool 0 restart

	# make sure service really restarted before requeueing active requests
	sleep 1
	do_mds 0 "lctl get_param mdt.MDT.hsm.active_requests | ${BUILDDIR@Q}/coordinatool-client -Q"
	start_lhsmtool_cmd 1
	client_archive_n_wait 3 100
}

init

# if we're being sourced, don't actually run tests
[[ $(caller | cut -d' ' -f1) != "0" ]] && return 0


FATAL=1 run_test sanity 00
run_test normal_requests 01
run_test server_restart_no_agent_active_requests 02

echo "Summary: ran $TESTS tests, $SKIPS skipped, $FAILURES failures"

exit $FAILURES

