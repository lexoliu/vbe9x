#!/bin/sh
# Builds the driver and the test guest program into build/ with the pinned Open Watcom.
# Arguments go to wmake, e.g. tools/build.sh DEBUG=1
set -eu
cd "$(dirname "$0")/.."
WATCOM=$(tools/watcom.sh)
case "$(uname -s)" in
Linux) host=binl64 ;;
Darwin) host=armo64 ;;
esac
export WATCOM
PATH=$WATCOM/$host:$PATH
export PATH
mkdir -p build/debug
exec wmake -h "$@"
