#!/bin/bash
# Diagnostic only: when no test has finished for 8 minutes, record what pgmoneta and PostgreSQL wait on.
V=$1
L=/tmp/pgmoneta-test/log
OUT=/tmp/hang-diag
mkdir -p $OUT
while true; do
  sleep 30
  pgrep -f 'test/check.sh ci' >/dev/null || exit 0
  [ -f $L/pgmoneta-test.log ] || continue
  age=$(( $(date +%s) - $(stat -c %Y $L/pgmoneta-test.log) ))
  [ $age -lt 480 ] && continue
  {
    echo "== stalled ${age}s after: $(tail -1 $L/pgmoneta-test.log)"
    echo "== ps"; ps -eo pid,ppid,stat,etime,wchan:32,args
    echo "== pg_stat_activity"
    su - postgres -c "/usr/pgsql-$V/bin/psql -h /tmp -X -c \"select pid, backend_type, state, wait_event_type, wait_event, now()-state_change as since, left(query,150) as query from pg_stat_activity\""
    for p in $(pgrep -f '^pgmoneta: |/pgmoneta -c|pgmoneta-test'); do
      echo "== gdb $p: $(tr '\0' ' ' < /proc/$p/cmdline)"
      gdb -batch -p $p -ex 'thread apply all bt' 2>&1 | grep -vE '^\[|^Using host|^warning'
    done
    echo "== ss (Recv-Q/Send-Q per socket), twice 20 s apart"
    ss -tanpie; sleep 20; echo "-- 20 s later"; ss -tanpie
    for p in $(pgrep -f '^pgmoneta: backup'); do
      echo "== fds of $p"; ls -l /proc/$p/fd
      for f in /proc/$p/fd/*; do echo "$f: $(grep -E '^flags' /proc/$p/fdinfo/${f##*/} 2>/dev/null)"; done
      echo "== strace $p (5 s)"; timeout 5 strace -f -tt -p $p 2>&1 | head -40
    done
    for p in $(pgrep -f 'postgres: (walsender|checkpointer)'); do
      echo "== gdb $p: $(tr '\0' ' ' < /proc/$p/cmdline)"
      gdb -batch -p $p -ex bt 2>&1 | grep -vE '^\[|^Using host|^warning'
    done
  } > $OUT/diag.txt 2>&1
  cp /pglog/logfile $OUT/postgresql.log 2>/dev/null
  cp $L/pgmoneta.log $OUT/pgmoneta.log 2>/dev/null
  cat $OUT/diag.txt
  exit 0
done
