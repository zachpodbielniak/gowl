#!/bin/sh
# Every top-level key the YAML parser asks for must be in the list the
# validator checks against, or a valid config warns about its own keys
# and `gowl --check-config' fails on it.  The list is what makes an
# unknown key a reported problem, so it has to be complete.
set -e
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
src="$root/src/config/gowl-config.c"
fail=0

asked=$(grep -oE 'yaml_mapping_has_member\(mapping, "[a-z0-9_-]+"\)' "$src" \
	| sed -E 's/.*"([^"]+)".*/\1/' | sort -u)
listed=$(sed -n '/^static const gchar \*const top_level_keys\[\] = {/,/^};/p' "$src" \
	| grep -oE '"[a-z0-9_-]+"' | tr -d '"' | sort -u)

n=$(printf '%s\n' "$asked" | grep -c . || true)
if [ "$n" -lt 50 ]; then
	echo "FAIL: found only $n has_member() keys; the extraction no longer matches"
	exit 1
fi
for k in $asked; do
	if ! printf '%s\n' "$listed" | grep -qx "$k"; then
		echo "FAIL: the parser reads '$k' but top_level_keys[] does not list it"
		fail=1
	fi
done
# The keys read through a table rather than a literal.
for k in xkb-layout xkb-variant xkb-model xkb-options xkb-rules xkb-file \
	evaluate_gowl_config_with_cmacs evaluate-gowl-config-with-cmacs \
	evaluate_c_config_with_cmacs evaluate-c-config-with-cmacs; do
	if ! printf '%s\n' "$listed" | grep -qx "$k"; then
		echo "FAIL: '$k' is read but top_level_keys[] does not list it"
		fail=1
	fi
done
[ "$fail" = 0 ] && echo "PASS: every key the parser reads is a known key"
exit $fail
