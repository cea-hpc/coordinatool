# default test config can be overriden with tests_config.local.sh
# instead of modifying this file: that will allow for less git
# conflicts

declare -a CLIENT=(
	localhost
	localhost
	localhost
	localhost
	$(hostname)
)
declare -a MNTPATH=(
	/mnt/lustre
	/mnt/lustre2
	/mnt/lustre3
	/mnt/lustre4
	/mnt/lustre5
)

declare -a MDS=(
	localhost
	$(hostname)
)
declare -a MDT=(
	testfs0-MDT0000
	testfs0-MDT0001
)

SOURCEDIR="$REPO_ROOT"
BUILDDIR="$SOURCEDIR/build"
# must match paths in $BUILDDIR/tests/lhsm_cmd*.conf
ARCHIVEDIR=/tmp/archive
TESTDIR=MNTPATH/.tests
