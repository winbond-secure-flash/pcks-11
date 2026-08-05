#!/bin/sh
# ft23-target.sh — W77Q LUT Integrity Validation
#
# Checks every live LUT entry for:
#   FT23.1 — LUT accessibility and inventory
#   FT23.2 — Flash offset alignment (8-byte) and section bounds
#   FT23.3 — Duplicate flash offsets
#   FT23.4 — Record overlaps
#   FT23.5 — Magic bytes at each flash offset (0x574F424A = WOBJ)
#   FT23.6 — Header field consistency + XOR-rotate checksum
#
# Checksum (w77q_fs.c record_checksum):
#   p = hdr + 4; i=0..95, skip i=92..95 (checksum field):
#     v ^= p[i] << ((i & 3) * 8)
#   then for each data byte[i]:
#     v ^= data[i] << ((i & 3) * 8)
#
# Header layout (w77q_fs.h, 104 bytes, all LE):
#   [0..3]    magic      uint32  0x574F424A = WOBJ live
#   [4..7]    total_size uint32  ROUNDUP(104 + data_size, 8)
#   [8..23]   ta_uuid    16 B
#   [24..87]  obj_id     64 B zero-padded
#   [88]      obj_id_len uint8   1..64
#   [89..91]  _pad
#   [92..95]  data_size  uint32
#   [96..99]  checksum   uint32
#   [100..103] _pad2
#
# Deploy to /usr/bin/ft23-target.sh
case "$1" in --help|-h)
    echo "Usage: $(basename $0)"
    echo ""
    echo "  W77Q LUT integrity: alignment, bounds, duplicates, overlaps,"
    echo "  magic bytes, header field consistency and XOR-rotate checksum."
    echo ""
    echo "Pass/Fail criteria: PASS if all LUT entries pass all checks."
    exit 0 ;;
esac
. /usr/bin/tee-tests-common.sh

FT=FT23
PASS=1
TOTAL=0
OK=0
RESULTS=""

record() {
    TOTAL=$((TOTAL+1))
    if [ "$2" = "PASS" ]; then
        OK=$((OK+1))
        printf "  [PASS] %s\n" "$1"
    else
        printf "  [FAIL] %s  %s\n" "$1" "${3:-}"
        PASS=0
    fi
    RESULTS="${RESULTS}${1}=${2}\n"
}

# ---------------------------------------------------------------------------
# Constants (from w77q_fs.h / 0002-rcar-w77q-flash-driver.patch)
# ---------------------------------------------------------------------------
W77Q_HDR_SIZE=104
W77Q_HDR_ALIGN=8
W77Q_OBJ_AREA_START=4096           # 0x1000 — sector 0 reserved
W77Q_FS_SECTION_SIZE=16777216      # 16 * 1024 * 1024  (must match W77Q_LFS_SECTION_SIZE)
W77Q_MAGIC_LIVE=1467263818         # 0x574F424A decimal
W77Q_FS_MAX_LUT=244
MAX_READ=65000                     # w77q-dump read-raw hard cap

LUT_FILE=/tmp/ft23_lut.txt
ENTRY_FILE=/tmp/ft23_entries.txt

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Parse all hex bytes from w77q-dump read-raw output (one hex-pair per line).
# Outputs raw awk array assignment code consumed by checksum_awk below.
parse_bytes_awk() {
    # strips '[OK]' header and '|ascii|' suffix; emits bare hex tokens
    awk '/^\[OK\]/ { next }
         NF > 1 {
             for (f = 2; f <= NF; f++) {
                 v = $f
                 if (v ~ /^\|/) break
                 if (v ~ /^[0-9a-fA-F][0-9a-fA-F]$/)
                     printf "%s ", v
             }
         }'
}

# ---------------------------------------------------------------------------
# FT23.1 — LUT inventory
# ---------------------------------------------------------------------------
section "$FT.1: LUT inventory"
ensure_tee_supplicant

w77q-dump list-all > "$LUT_FILE" 2>&1
RC=$?
cat "$LUT_FILE"

if [ "$RC" -ne 0 ]; then
    record "LUT accessible" FAIL "w77q-dump returned $RC"
    exit 1
fi

# Extract entries: each line  "[NNN] ta_uuid=U  flash_off=0xXXX  data_size=D  obj_id=O"
grep -E '^\[' "$LUT_FILE" | awk '{
    off = ""; sz = ""
    for (i = 1; i <= NF; i++) {
        if ($i ~ /^flash_off=/)  off = substr($i, 11)
        if ($i ~ /^data_size=/)  sz  = substr($i, 11)
    }
    if (off != "" && sz != "")
        printf "%s %s\n", off, sz
}' > "$ENTRY_FILE"

N=$(wc -l < "$ENTRY_FILE")
N=$(echo "$N" | tr -d ' ')

echo "  entries: $N"
[ "$N" -eq "$W77Q_FS_MAX_LUT" ] && \
    echo "  WARNING: LUT full ($W77Q_FS_MAX_LUT entries) — some objects may not be visible"

record "LUT accessible" PASS "$N entries"

# ---------------------------------------------------------------------------
# FT23.2 — Alignment and bounds
# ---------------------------------------------------------------------------
section "$FT.2: Alignment and bounds"

FAIL_COUNT=$(awk -v hdr=$W77Q_HDR_SIZE \
                 -v align=$W77Q_HDR_ALIGN \
                 -v obj_start=$W77Q_OBJ_AREA_START \
                 -v sec_size=$W77Q_FS_SECTION_SIZE '
{
    off_hex = $1; sz = $2 + 0
    off = strtonum(off_hex)
    total = hdr + sz
    total = int((total + 7) / 8) * 8

    err = ""
    if (off % align != 0)
        err = err sprintf("  [FAIL] align  %s: not %d-byte aligned\n", off_hex, align)
    if (off < obj_start)
        err = err sprintf("  [FAIL] bounds %s: below 0x%x (sector 0 reserved)\n",
                          off_hex, obj_start)
    else if (off + total > sec_size)
        err = err sprintf("  [FAIL] bounds %s: end=0x%x > section end (0x%x)\n",
                          off_hex, off + total, sec_size)
    if (err != "") { printf "%s", err; fails++ }
}
END { print fails+0 }
' "$ENTRY_FILE")

if [ "$FAIL_COUNT" -eq 0 ]; then
    echo "  all $N entries aligned and within bounds"
    record "alignment + bounds" PASS
else
    record "alignment + bounds" FAIL "$FAIL_COUNT entries failed"
fi

# ---------------------------------------------------------------------------
# FT23.3 — Duplicate offsets
# ---------------------------------------------------------------------------
section "$FT.3: Duplicate flash offset check"

DUPS=$(awk '{print $1}' "$ENTRY_FILE" | sort | uniq -d)
if [ -z "$DUPS" ]; then
    echo "  all $N offsets unique"
    record "no duplicate offsets" PASS
else
    echo "$DUPS" | while IFS= read -r d; do
        echo "  [FAIL] duplicate flash_off $d"
    done
    DUP_CNT=$(echo "$DUPS" | wc -l | tr -d ' ')
    record "no duplicate offsets" FAIL "$DUP_CNT duplicates"
fi

# ---------------------------------------------------------------------------
# FT23.4 — Record overlap check
# ---------------------------------------------------------------------------
section "$FT.4: Record overlap check"

OVERLAP_COUNT=$(awk -v hdr=$W77Q_HDR_SIZE '
{
    off_hex[NR] = $1
    off[NR]     = strtonum($1)
    sz[NR]      = $2 + 0
}
END {
    # sort by offset (simple insertion sort — N <= 244)
    for (i = 2; i <= NR; i++) {
        for (j = i; j > 1 && off[j] < off[j-1]; j--) {
            tmp = off[j];   off[j]   = off[j-1];   off[j-1]   = tmp
            tmp = sz[j];    sz[j]    = sz[j-1];    sz[j-1]    = tmp
            tmp = off_hex[j]; off_hex[j] = off_hex[j-1]; off_hex[j-1] = tmp
        }
    }
    fails = 0
    for (i = 1; i < NR; i++) {
        total = hdr + sz[i]
        total = int((total + 7) / 8) * 8
        a_end = off[i] + total
        if (a_end > off[i+1]) {
            printf "  [FAIL] overlap: %s end=0x%x > next %s\n",
                   off_hex[i], a_end, off_hex[i+1]
            fails++
        }
    }
    print fails
}
' "$ENTRY_FILE")

if [ "$OVERLAP_COUNT" -eq 0 ]; then
    echo "  no overlaps among $N entries"
    record "no overlaps" PASS
else
    record "no overlaps" FAIL "$OVERLAP_COUNT overlapping pairs"
fi

# ---------------------------------------------------------------------------
# FT23.5 — Magic byte check
# ---------------------------------------------------------------------------
section "$FT.5: Magic byte verification"

MAGIC_FAIL=0
while IFS=' ' read -r OFF SZ; do
    MAGIC=$(w77q-dump read-raw "$OFF" 4 2>/dev/null | \
        awk '/^\[OK\]/ { next }
             NF >= 5 {
                 b0 = strtonum("0x" $2)
                 b1 = strtonum("0x" $3)
                 b2 = strtonum("0x" $4)
                 b3 = strtonum("0x" $5)
                 printf "%u\n", b0 + b1*256 + b2*65536 + b3*16777216
                 exit
             }')
    if [ "$MAGIC" = "$W77Q_MAGIC_LIVE" ]; then
        printf "  [OK]   %s  magic=0x574f424a (WOBJ)\n" "$OFF"
    else
        printf "  [FAIL] %s  magic=%s (expected 0x574f424a)\n" "$OFF" "$MAGIC"
        MAGIC_FAIL=$((MAGIC_FAIL + 1))
    fi
done < "$ENTRY_FILE"

if [ "$MAGIC_FAIL" -eq 0 ]; then
    record "magic bytes" PASS
else
    record "magic bytes" FAIL "$MAGIC_FAIL entries bad"
fi

# ---------------------------------------------------------------------------
# FT23.6 — Header fields + XOR-rotate checksum
# ---------------------------------------------------------------------------
section "$FT.6: Header fields and checksum"

HDR_FAIL=0
while IFS=' ' read -r OFF LUT_SZ; do
    LUT_SZ_INT=$(printf '%d' "$LUT_SZ" 2>/dev/null || echo 0)
    READ_LEN=$((W77Q_HDR_SIZE + LUT_SZ_INT))
    [ "$READ_LEN" -gt "$MAX_READ" ] && READ_LEN=$MAX_READ

    # Read header + payload bytes; parse through awk
    RESULT=$(w77q-dump read-raw "$OFF" "$READ_LEN" 2>/dev/null | \
        parse_bytes_awk | \
        awk -v hdr=$W77Q_HDR_SIZE \
            -v lut_sz="$LUT_SZ_INT" \
            -v max_read="$READ_LEN" \
            -v magic_live=$W77Q_MAGIC_LIVE '
        BEGIN { nbytes = 0; v = 0 }
        {
            n = split($0, tokens, " ")
            for (i = 1; i <= n; i++) {
                if (tokens[i] == "") continue
                bytes[nbytes++] = strtonum("0x" tokens[i])
            }
        }
        END {
            if (nbytes < hdr) {
                printf "ERR short_read=%d\n", nbytes; exit
            }
            # Extract header fields (LE uint32)
            magic = bytes[0] + bytes[1]*256 + bytes[2]*65536  + bytes[3]*16777216
            ts    = bytes[4] + bytes[5]*256 + bytes[6]*65536  + bytes[7]*16777216
            ds    = bytes[92]+ bytes[93]*256+ bytes[94]*65536 + bytes[95]*16777216
            oidl  = bytes[88]
            sto   = bytes[96]+ bytes[97]*256+ bytes[98]*65536 + bytes[99]*16777216

            # total_size must equal ROUNDUP(104 + ds, 8)
            exp_ts = int((hdr + ds + 7) / 8) * 8

            err = ""
            if (magic != magic_live)
                err = err "bad_magic"
            if (ts != exp_ts)
                err = err sprintf(" bad_total_size(%u!=%u)", ts, exp_ts)
            if (oidl == 0 || oidl > 64)
                err = err sprintf(" bad_obj_id_len(%u)", oidl)
            if (ds != lut_sz)
                err = err sprintf(" ds_mismatch(hdr=%u lut=%u)", ds, lut_sz)

            if (err != "") { printf "ERR %s\n", err; exit }

            # Checksum: p = hdr+4, i=0..95, skip 92..95
            for (i = 0; i < 96; i++) {
                if (i >= 92 && i < 96) continue
                v = xor(v, lshift(bytes[4+i], (i%4)*8))
            }
            # Data bytes (as many as we read)
            ndata = nbytes - hdr
            for (i = 0; i < ndata; i++)
                v = xor(v, lshift(bytes[hdr+i], (i%4)*8))

            truncated = (ds > 0 && nbytes < hdr + ds) ? 1 : 0
            printf "OK ts=%u ds=%u oidl=%u stored=0x%08x computed=0x%08x trunc=%d\n",
                   ts, ds, oidl, sto, v, truncated
        }
    ')

    case "$RESULT" in
        ERR*)
            printf "  [FAIL] %s: %s\n" "$OFF" "${RESULT#ERR }"
            HDR_FAIL=$((HDR_FAIL + 1))
            ;;
        OK*)
            # Extract fields from RESULT
            STORED=$(echo "$RESULT"   | grep -oE 'stored=0x[0-9a-f]+' | cut -d= -f2)
            COMPUTED=$(echo "$RESULT" | grep -oE 'computed=0x[0-9a-f]+' | cut -d= -f2)
            TRUNC=$(echo "$RESULT"    | grep -oE 'trunc=[01]' | cut -d= -f2)
            DS_INFO=$(echo "$RESULT"  | grep -oE 'ds=[0-9]+' | head -1 | cut -d= -f2)

            if [ "$TRUNC" = "1" ]; then
                # data_size > MAX_READ: header fields OK, checksum was partial — skip compare
                printf "  [SKIP] %s: ds=%s > %d — header OK, checksum partial\n" \
                       "$OFF" "$DS_INFO" "$MAX_READ"
            elif [ "$STORED" = "$COMPUTED" ]; then
                printf "  [OK]   %s: chk=%s  ds=%s\n" "$OFF" "$STORED" "$DS_INFO"
            else
                printf "  [FAIL] %s: stored=%s != computed=%s\n" \
                       "$OFF" "$STORED" "$COMPUTED"
                HDR_FAIL=$((HDR_FAIL + 1))
            fi
            ;;
        *)
            printf "  [FAIL] %s: unexpected output: %s\n" "$OFF" "$RESULT"
            HDR_FAIL=$((HDR_FAIL + 1))
            ;;
    esac
done < "$ENTRY_FILE"

if [ "$HDR_FAIL" -eq 0 ]; then
    record "header + checksum" PASS
else
    record "header + checksum" FAIL "$HDR_FAIL entries failed"
fi

# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------
rm -f "$LUT_FILE" "$ENTRY_FILE"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
section "$FT: Summary ($OK/$TOTAL passed)"
echo
printf '%b' "$RESULTS" | while IFS='=' read -r name result; do
    [ -z "$name" ] && continue
    printf "  %-38s : %s\n" "$name" "$result"
done
echo
if [ "$PASS" -eq 1 ]; then
    pass "$FT PASSED ($OK/$TOTAL)"
    exit 0
else
    FAILED=$((TOTAL - OK))
    fail "$FT FAILED ($FAILED/$TOTAL failed)"
    exit 1
fi
