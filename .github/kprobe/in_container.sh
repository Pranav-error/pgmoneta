#!/bin/bash
# Same steps as the build-linux container job (clang, Debug, PG 15). Exit 99 on a stall.
set -u
cd /src && mkdir -p build && cd build && CC=/usr/bin/clang cmake -DCMAKE_BUILD_TYPE=Debug -DDOCS=FALSE .. >/dev/null && make -j$(nproc) >/dev/null || exit 1
chown -R runner:runner /src
cat /proc/net/netstat > /tmp/hang-diag/netstat_start.txt
bash /src/.github/hang_watchdog.sh 15 >/dev/null 2>&1 &
su - runner -c "cd /src && export TEST_PG_VERSION=15 && export PATH=/usr/pgsql-15/bin:\$PATH && bash ./test/check.sh ci" > /tmp/hang-diag/check.out 2>&1 &
T=$!
while kill -0 $T 2>/dev/null; do
  sleep 15
  if [ -s /tmp/hang-diag/diag.txt ]; then
    date -u +%H:%M:%S > /tmp/hang-diag/stall_detected_utc.txt
    ss -tanpiem > /tmp/hang-diag/ss_mem.txt
    cat /proc/net/netstat > /tmp/hang-diag/netstat_stall.txt
    echo "STALL"; tail -3 /tmp/hang-diag/check.out; exit 99
  fi
done
wait $T; rc=$?
cat /proc/net/netstat > /tmp/hang-diag/netstat_end.txt
echo "check.sh rc=$rc"; tail -3 /tmp/hang-diag/check.out; exit $rc
