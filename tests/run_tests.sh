#!/bin/bash

error() {
	printf "%s\n" "$@" >&2
	exit 1
}

REPO_ROOT=$(git rev-parse --show-toplevel) \
	|| error "Must run within git repository"
SKIP=163
FATAL=0
FAILURES=0
. "${REPO_ROOT}/tests/tests_config.sh"


# helpers
do_client() {
	local i="$1"
	shift

	[[ "$i" =~ ^[0-9]+$ ]] || error "do_client: $i must be 0-4"
	(( i < ${#CLIENT[@]} )) || error "do_client: $i must be < ${#CLIENT[@]}"

	set -- "${@//MNTPATH/${MNTPATH[$i]}}"
	if [[ "${CLIENT[$i]}" = localhost ]]; then
		sudo sh -ec "$@"
	else
		ssh "${CLIENT[$i]}" sudo sh -ec "${@@Q}"
	fi
}

do_mds() {
	local i="$1"
	shift

	[[ "$i" =~ ^[0-9]+$ ]] || error "do_mds: $i must be 0-4"
	(( i < ${#MDS[@]} )) || error "do_mds: $i must be < ${#CLIENT[@]}"

	set -- "${@//MDT/${MDT[$i]}}"
	if [[ "${MDS[$i]}" = localhost ]]; then
		sudo sh -ec "$@"
	else
		ssh "${MDS[$i]}" sudo sh -ec "${@@Q}"
	fi
}

run_test() {
	local testcase="$1" status

	echo -n "Running $testcase..."
	(
		set -e
		"$testcase"
	)
	status="$?"
	if ((status == SKIP)); then
		echo "SKIP"
	elif ((status)); then
		echo FAIL
		((FATAL)) && exit 1
		((FAILURES++))
	else
		echo "Ok"
	fi
}

# sanity checks
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
		do_client $i "pkill [l]hsmd_coordinatool" || :
		do_client $i "pkill [l]hsmtool_cmd" || :
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


normal_requests() {
	do_client 0 "${BUILDDIR@Q}/lhsmd_coordinatool -vv MNTPATH" &
	do_client 1 "rm -rf ${ARCHIVEDIR@Q} && mkdir ${ARCHIVEDIR@Q}"
	do_client 1 "LD_PRELOAD=${BUILDDIR@Q}/libcoordinatool_client.so \
	       ${BUILDDIR@Q}/tests/lhsmtool_cmd -vv --config ${SOURCEDIR@Q}/tests/lhsm_cmd.conf MNTPATH" &

	do_client 2 "rm -rf ${TESTDIR@Q}
		mkdir ${TESTDIR@Q}
		cd ${TESTDIR@Q}
		echo foo > file1
		lfs hsm_archive file1
		TMOUT=30; while ! lfs hsm_state file1 | grep -q archived && ((TMOUT-- > 0)); do
			sleep 0.1
		done
		if ! ((TMOUT)); then echo 'Failed to archive'; exit 1; fi
		lfs hsm_release file1
		[[ \$(cat file1) = foo ]]
		lfs hsm_remove file1
		TMOUT=30; while lfs hsm_state file1 | grep -q archived && ((TMOUT-- > 0)); do
			sleep 0.1
		done
		if ! ((TMOUT)); then echo 'Failed to remove'; exit 1; fi
		"

	do_client 1 "pkill [l]hsmtool" || :
	do_client 0 "pkill [l]hsmd" || :
	wait
}


FATAL=1 run_test sanity
run_test normal_requests

exit $FAILURES

