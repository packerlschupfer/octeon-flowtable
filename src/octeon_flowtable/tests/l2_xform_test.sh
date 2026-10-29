#!/bin/bash
# Host unit test for octeon_ft_l2_xform (VLAN/QinQ/PPPoE encap rewrite).
# Extracts the function verbatim from ../octeon_flowtable.c, wraps it in a
# minimal WQE shim and checks every encap/decap combination byte-for-byte.
set -e
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
SRC=$(dirname "$0")/../octeon_flowtable.c
python3 - "$SRC" "$D/t.c" "$(dirname "$0")/l2_xform_cases.c" <<'PY'
import sys
src=open(sys.argv[1]).read()
a=src.index('static inline void octeon_ft_l2_xform(')
b=src.index('\twork->word1.len -= shift;\n}\n',a)+len('\twork->word1.len -= shift;\n}\n')
cases=open(sys.argv[3]).read()
hdr, test = cases.split('/* @@FUNCTION@@ */')
open(sys.argv[2],'w').write(hdr+src[a:b].replace('octeon_ft_l2_xform(','octeon_ft_l2_xform_t(',1)+test)
PY
gcc -O1 -Wall -o "$D/t" "$D/t.c" && "$D/t"
