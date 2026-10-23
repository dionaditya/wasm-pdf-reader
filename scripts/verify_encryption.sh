#!/usr/bin/env bash
# Compare the file key this engine derives with qpdf's, per fixture and password.
set -u
cd "$(dirname "$0")/.."
QPDF=/tmp/qpdfbin/bin/qpdf
export LD_LIBRARY_PATH=/tmp/qpdfbin/lib

printf "%-30s %-14s %-6s %-34s %-34s %s\n" FILE PASSWORD BYTES "ours" "qpdf" MATCH
check() {
  local file=$1 pw=$2
  local ours r err out key
  out=$(/tmp/enc_probe "$file" "$pw" 2>&1)
  r=$(printf '%s\n' "$out" | sed -n 's/.*R=\([0-9]\).*/\1/p')
  if printf '%s' "$out" | grep -q 'ok=1'; then
    ours=$(printf '%s' "$out" | sed -n 's/.*key=\([0-9a-f]*\).*/\1/p')
  else
    ours=$(printf '%s' "$out" | sed -n 's/.*error=\([^ ]*\).*/REFUSED/p' | head -1)
    ours=${ours:-REFUSED}
  fi
  local keyline
  keyline=$("$QPDF" --password="$pw" --show-encryption --show-encryption-key "$file" 2>&1 \
              | sed -n 's/.*Encryption key = //p' | head -1)
  if [ -z "$keyline" ]; then keyline="(refused)"; fi
  local verdict
  if [ "$ours" = "REFUSED" ] && [ "$keyline" = "(refused)" ]; then verdict="both refused"
  elif [ "$ours" = "$keyline" ]; then verdict="MATCH"
  else verdict="differs"; fi
  printf "%-30s %-14s %-6s %-34s %-34s %s\n" "$(basename "$file")" "${pw:-<empty>}" "$r" "$ours" "$keyline" "$verdict"
}

for f in test/fixtures/enc-r2-rc4.pdf test/fixtures/enc-r4-rc4.pdf \
         test/fixtures/enc-r4-aes128.pdf test/fixtures/enc-r5-aes256.pdf \
         test/fixtures/enc-r6-aes256.pdf \
         test/fixtures/enc-r6-aes256-user-only.pdf test/fixtures/enc-r6-aes256-badperms.pdf; do
  check "$f" reader-fixture
  check "$f" owner-fixture
done
check test/fixtures/enc-r6-aes256.pdf wrong-password
check test/fixtures/enc-r6-aes256-user-only.pdf ""
