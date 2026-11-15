#!/bin/bash
set -u
FAIL=0
for p in packages/falcon-cli packages/falcon-daemon packages/libfalcon-core packages/libfalcon-drives packages/libfalcon-protocols packages/libfalcon-storage; do
  cd "/home/cui/workspaces/falcon/build-coverage/$p/tests" || continue
  echo "=== RUNNING $p ==="
  TMPDIR=/home/cui/workspaces/falcon/.tmp/gcc-scratch ctest --build-config Debug --output-on-failure --timeout 180 --no-tests=error 2>&1
  rc=$?
  echo "=== $p EXIT $rc ==="
  [ $rc -ne 0 ] && FAIL=1
done
exit $FAIL
