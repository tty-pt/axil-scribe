#!/bin/bash
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
AXIL_BIN="${AXIL_BIN:-$DIR/../axil/bin/axil}"
if [ ! -x "$AXIL_BIN" ]; then
	AXIL_BIN="$(command -v axil)"
fi

echo "=== axil-scribe Live E2E Test Suite (No-JS SSR + JSON + Exports) ==="

MODULE_PATH="$DIR/lib/libaxil-scribe"
if [ ! -f "$MODULE_PATH.so" ]; then
	echo "FAIL: $MODULE_PATH.so not built"
	exit 1
fi
if [ ! -f "$DIR/htdocs/scribe_ux.wasm" ]; then
	echo "FAIL: $DIR/htdocs/scribe_ux.wasm not built"
	exit 1
fi

# Generate synthetic PNG fixture (zero real PII)
SYNTH_DIR="$(mktemp -d)"
DATA_DIR="$(mktemp -d)"
MOCK_FILE="$(mktemp)"
DICT_FILE="$(mktemp)"
LOG_FILE="$(mktemp)"
SERVER_PID=""

cleanup() {
	if [ -n "$SERVER_PID" ]; then
		kill "$SERVER_PID" 2>/dev/null || true
		wait "$SERVER_PID" 2>/dev/null || true
	fi
	rm -rf "$SYNTH_DIR" "$DATA_DIR" "$MOCK_FILE" "$DICT_FILE" "$LOG_FILE"
}
trap cleanup EXIT INT TERM

python3 "$DIR/tests/generate_synthetic_certs.py" --out "$SYNTH_DIR" >/dev/null
SAMPLE_PNG="$SYNTH_DIR/01_confirmation_clean.png"
if [ ! -f "$SAMPLE_PNG" ]; then
	echo "FAIL: synthetic fixture $SAMPLE_PNG missing"
	exit 1
fi

# 2-pass mock fixture: Pass 1 and Pass 2 disagree on sponsor -> CONFLICT!
cat > "$MOCK_FILE" <<'EOF'
[
  {
    "entries": [
      {
        "fields": {
          "person_name": "Maria das Graças Silva",
          "confirmation_name": "Teresa",
          "confirmation_date": "1954-06-24",
          "parish": "São João",
          "sponsor": "Joao Pereyra",
          "parents": "António Silva e Conceição Costa",
          "officiant": "Pe. Manuel",
          "baptism_date": "1942-04-10",
          "baptism_parish": "São João",
          "book_page_entry": "Livro-1954 fl.12 #1"
        },
        "bbox": {
          "sponsor": [0.10, 0.55, 0.65, 0.08]
        },
        "marginal_notes": "Margem: ver fl. 12"
      }
    ]
  },
  {
    "entries": [
      {
        "fields": {
          "person_name": "Maria das Graças Silva",
          "confirmation_name": "Teresa",
          "confirmation_date": "1954-06-24",
          "parish": "São João",
          "sponsor": "João Pereira",
          "parents": "António Silva e Conceição Costa",
          "officiant": "Pe. Manuel",
          "baptism_date": "1942-04-10",
          "baptism_parish": "São João",
          "book_page_entry": "Livro-1954 fl.12 #1"
        }
      }
    ]
  }
]
EOF

# Read-only advisory dictionary fixture
cat > "$DICT_FILE" <<'EOF'
person	João Pereira	1900	1980
place	São João	1800	2000
EOF

# Pick an ephemeral free port (never touches host :8080 or :4242)
TEST_PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
echo "--- Starting ephemeral axil on 127.0.0.1:$TEST_PORT ---"

LIB_PATHS="$DIR/lib:$DIR/../libcorm/lib:$DIR/../liblemma/lib:$DIR/../libhyle/lib:$DIR/../libbud/lib:$DIR/../libstele/lib:$DIR/../libxylem/lib:$DIR/../axil/lib"
LD_LIBRARY_PATH="$LIB_PATHS:${LD_LIBRARY_PATH:-}" \
SCRIBE_DATA_DIR="$DATA_DIR" \
SCRIBE_TEMPLATES_DIR="$DIR/templates" \
SCRIBE_DICT_PATH="$DICT_FILE" \
SCRIBE_MOCK_JSON="$MOCK_FILE" \
SCRIBE_WASM_PATH="$DIR/htdocs/scribe_ux.wasm" \
"$AXIL_BIN" -A -p "$TEST_PORT" -m "$MODULE_PATH" >"$LOG_FILE" 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 40); do
	if python3 -c "import socket; s=socket.create_connection(('127.0.0.1',$TEST_PORT),0.1); s.close()" 2>/dev/null; then
		break
	fi
	sleep 0.1
done

if ! python3 -c "import socket; s=socket.create_connection(('127.0.0.1',$TEST_PORT),0.2); s.close()" 2>/dev/null; then
	echo "FAIL: ephemeral axil failed to bind on port $TEST_PORT"
	cat "$LOG_FILE"
	exit 1
fi

BASE="http://127.0.0.1:$TEST_PORT"

# 1. SSR Queue + Static CSS + WASM asset checks
echo "--- 1. Checking SSR Queue, CSS, and WASM assets ---"
QUEUE_HTML=$(curl -fsS "$BASE/")
echo "$QUEUE_HTML" | grep -q 'action="/ingest"'
echo "$QUEUE_HTML" | grep -q 'enctype="multipart/form-data"'
echo "$QUEUE_HTML" | grep -q 'confirmation_starter'

CSS_CT=$(curl -fsS -o /dev/null -w "%{content_type}" "$BASE/assets/scribe.css")
echo "$CSS_CT" | grep -qi "text/css"

WASM_TMP="$SYNTH_DIR/downloaded.wasm"
WASM_CT=$(curl -fsS -o "$WASM_TMP" -w "%{content_type}" "$BASE/assets/scribe_ux.wasm")
echo "$WASM_CT" | grep -qi "application/wasm"
cmp -s "$DIR/htdocs/scribe_ux.wasm" "$WASM_TMP"
echo "PASS: SSR Queue, CSS, and WASM served cleanly."

# 2. Multipart binary scan ingest with 2-pass conflict routing
echo "--- 2. Testing POST /ingest (multipart PNG + 2-pass conflict routing) ---"
INGEST_JSON=$(curl -fsS -H "Accept: application/json" \
	-F "file=@$SAMPLE_PNG;type=image/png" \
	-F "template_id=confirmation_starter" \
	-F "book_ref=Livro-1954" \
	-F "page_num=12" \
	-F "extract=1" \
	"$BASE/ingest")

ENTRY_ID=$(echo "$INGEST_JSON" | python3 -c 'import sys,json; d=json.load(sys.stdin); assert d["ok"]; assert len(d["entries"])==1; e=d["entries"][0]; assert e["status"]=="needs_review"; print(e["id"])')
PAGE_ID=$(echo "$INGEST_JSON" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d["pages"][0]["id"])')
PAGE_SHA=$(echo "$INGEST_JSON" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d["pages"][0]["sha256"])')
echo "PASS: Ingested page=$PAGE_ID entry=$ENTRY_ID status=needs_review"

# 3. Split-screen SSR verification workbench renders CONFLICT + Raw OCR + bbox + advisory chip
echo "--- 3. Testing GET /entries/$ENTRY_ID (Split-screen SSR Workbench) ---"
ENTRY_HTML=$(curl -fsS "$BASE/entries/$ENTRY_ID")
echo "$ENTRY_HTML" | grep -q 'scribe-workbench'
echo "$ENTRY_HTML" | grep -q 'CONFLICT'
echo "$ENTRY_HTML" | grep -q 'Raw OCR: Joao Pereyra'
echo "$ENTRY_HTML" | grep -q 'scribe-bbox'
echo "$ENTRY_HTML" | grep -q 'João Pereira'
echo "$ENTRY_HTML" | grep -q "src=\"/pages/$PAGE_ID/image\""
echo "PASS: Split-screen workbench rendered CONFLICT, Raw OCR, bbox, and advisory suggestion."

# 4. Immutable scan image serving + ETag + 304 Not Modified
echo "--- 4. Testing GET /pages/$PAGE_ID/image (bytes, ETag, Cache-Control, 304) ---"
IMG_OUT="$SYNTH_DIR/served.png"
IMG_HDR="$SYNTH_DIR/served.hdr"
curl -fsS -D "$IMG_HDR" -o "$IMG_OUT" "$BASE/pages/$PAGE_ID/image"
cmp -s "$SAMPLE_PNG" "$IMG_OUT"
grep -qi "ETag: \"$PAGE_SHA\"" "$IMG_HDR"
grep -qi "Cache-Control: public, max-age=31536000, immutable" "$IMG_HDR"
CODE_304=$(curl -s -o /dev/null -w "%{http_code}" -H "If-None-Match: \"$PAGE_SHA\"" "$BASE/pages/$PAGE_ID/image")
[ "$CODE_304" = "304" ]
echo "PASS: Scan image byte-identical with immutable ETag and 304 support."

# 5. Multi-entry bound register workflow (1 page -> 2 entries)
echo "--- 5. Testing POST /pages/$PAGE_ID/entries (1 page -> N entries) ---"
SECOND_JSON=$(curl -fsS -H "Accept: application/json" \
	-X POST -d "template_id=confirmation_starter" \
	"$BASE/pages/$PAGE_ID/entries")
ENTRY2_ID=$(echo "$SECOND_JSON" | python3 -c 'import sys,json; d=json.load(sys.stdin); assert d["ok"]; assert d["entry"]["page_id"]=="'"$PAGE_ID"'"; print(d["entry"]["id"])')
[ "$ENTRY2_ID" != "$ENTRY_ID" ]
echo "PASS: Created second entry $ENTRY2_ID on existing page $PAGE_ID."

# 6. No-JS HTML form verification (application/x-www-form-urlencoded -> 303 redirect)
echo "--- 6. Testing POST /entries/$ENTRY_ID/verify (No-JS Form -> Verified + Audit Trail) ---"
VERIFY_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
	-X POST "$BASE/entries/$ENTRY_ID/verify" \
	--data-urlencode "field_person_name=Maria das Graças Silva" \
	--data-urlencode "field_confirmation_name=Teresa" \
	--data-urlencode "field_confirmation_date=1954-06-24" \
	--data-urlencode "field_parish=São João" \
	--data-urlencode "field_sponsor=João Pereira" \
	--data-urlencode "field_parents=António Silva e Conceição Costa" \
	--data-urlencode "field_officiant=Pe. Manuel" \
	--data-urlencode "field_baptism_date=1942-04-10" \
	--data-urlencode "field_baptism_parish=São João" \
	--data-urlencode "field_book_page_entry=Livro-1954 fl.12 #1" \
	--data-urlencode "marginal_notes=Verificado no livro original" \
	--data-urlencode "reviewed_by=Archivist-A" \
	--data-urlencode "action=verify")
[ "$VERIFY_CODE" = "303" ]

VERIFIED_JSON=$(curl -fsS "$BASE/entries/$ENTRY_ID?format=json")
echo "$VERIFIED_JSON" | python3 -c '
import sys, json
d = json.load(sys.stdin)
e = d["entry"]
assert e["status"] == "verified", e["status"]
assert e["reviewed_by"] == "Archivist-A", e["reviewed_by"]
assert e["reviewed_at"] > 0
fmap = {f["key"]: f for f in e["fields"]}
assert fmap["sponsor"]["raw_value"] == "Joao Pereyra"
assert fmap["sponsor"]["verified_value"] == "João Pereira"
assert (fmap["sponsor"]["flags"] & 4) != 0  # SCRIBE_FLAG_EDITED
assert (fmap["sponsor"]["flags"] & 2) == 0  # SCRIBE_FLAG_CONFLICT cleared
'
echo "PASS: Form verification persisted verified_value, preserved raw_value, and stamped reviewer."

# 7. liblemma Search: Accent-Sensitive Exact vs ASCII-Folded
echo "--- 7. Testing GET /search (accent-sensitive exact vs folded) ---"
EXACT_HIT=$(curl -fsS --get --data-urlencode "q=Conceição" --data-urlencode "format=json" "$BASE/search")
EXACT_MISS=$(curl -fsS --get --data-urlencode "q=Conceicao" --data-urlencode "format=json" "$BASE/search")
FOLDED_HIT=$(curl -fsS --get --data-urlencode "q=Conceicao" --data-urlencode "folded=1" --data-urlencode "format=json" "$BASE/search")

echo "$EXACT_HIT" | python3 -c 'import sys,json; d=json.load(sys.stdin); assert len(d["entries"]) == 1'
echo "$EXACT_MISS" | python3 -c 'import sys,json; d=json.load(sys.stdin); assert len(d["entries"]) == 0'
echo "$FOLDED_HIT" | python3 -c 'import sys,json; d=json.load(sys.stdin); assert len(d["entries"]) == 1'
echo "PASS: liblemma search preserves accent sensitivity by default and matches when folded=1."

# 8. Open Exports: CSV, JSON, and Printable HTML Transcript
echo "--- 8. Testing /export.csv, /export.json, and /entries/$ENTRY_ID/transcript ---"
CSV_OUT=$(curl -fsS "$BASE/export.csv?status=verified")
echo "$CSV_OUT" | grep -q "entry_id,page_id,page_sha256"
echo "$CSV_OUT" | grep -q "João Pereira"
echo "$CSV_OUT" | grep -q "Joao Pereyra"

JSON_OUT=$(curl -fsS "$BASE/export.json?status=verified")
echo "$JSON_OUT" | python3 -c 'import sys,json; d=json.load(sys.stdin); assert d["version"]=="0.1.0"; assert len(d["entries"])==1'

TRANSCRIPT_HTML=$(curl -fsS "$BASE/entries/$ENTRY_ID/transcript")
echo "$TRANSCRIPT_HTML" | grep -q "Archival Transcript"
echo "$TRANSCRIPT_HTML" | grep -q "$PAGE_SHA"
echo "$TRANSCRIPT_HTML" | grep -q "Archivist-A"
echo "$TRANSCRIPT_HTML" | grep -q "João Pereira"
echo "$TRANSCRIPT_HTML" | grep -q "Joao Pereyra"
echo "PASS: CSV, JSON, and printable archival transcript verified."

echo "=== ALL AXIL-SCRIBE E2E TESTS PASSED ==="
