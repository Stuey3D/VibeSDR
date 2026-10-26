#!/bin/bash
# ★ Every unit test the Windows build produces, run under Wine (amd64 Debian, wine 8.0) — the tests that need no
#   radio. Called by scripts/build-windows.sh --test. Prints one line per test and a summary; exit 1 if any failed.
# ★ Silent by construction: no test plays audio (Stuart's rule), and Wine here has no sound device anyway.
cd /b || exit 2
export WINEDEBUG=-all
wine cmd /c ver >/dev/null 2>&1   # ★ create the prefix once, outside every test's timing
pass=0; fail=0; skip=0; failed=""
for t in test-*.exe; do
  out="$(timeout ${TEST_TIMEOUT:-900} wine "./$t" 2>&1)"; rc=$?
  last="$(printf '%s\n' "$out" | grep -v '^wine:' | grep -v '^\s*$' | tail -1 | cut -c1-110)"
  if [ $rc -eq 0 ]; then pass=$((pass+1)); printf 'PASS  %-28s %s\n' "$t" "$last"
  elif [ $rc -eq 77 ]; then skip=$((skip+1)); printf 'SKIP  %-28s %s\n' "$t" "$last"
  else fail=$((fail+1)); failed="$failed $t"; printf 'FAIL  %-28s rc=%d  %s\n' "$t" "$rc" "$last"
       printf '%s\n' "$out" | grep -v '^wine:' | grep -iE 'fail|error|assert|✗' | head -8 | sed 's/^/        /'
  fi
done
echo "Wine test summary: $pass passed, $fail failed, $skip skipped${failed:+ — failed:$failed}"
[ $fail -eq 0 ]
