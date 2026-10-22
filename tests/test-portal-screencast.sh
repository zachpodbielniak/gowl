#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# End-to-end: the ScreenCast and Screenshot portals, against a real
# headless compositor, on a private session bus.
#
# ScreenCast: the freedesktop handshake a video call makes --
# CreateSession, SelectSources, Start -- produces a PipeWire node (and,
# for v6, its serial).  Then the v4 restore contract: a persistent
# session returns restore_data, presenting it again skips the chooser,
# the grant survives a portal restart because it is on disk (0600, in
# $XDG_STATE_HOME/gowl), it is single use, it is bound to the app it was
# given to, and one whose source has vanished falls back to the chooser.
# A cursor mode that is not advertised closes the session.
#
# Screenshot: a PNG of the output's real size, an area cropped to the
# size the picker said, and PickColor returning the colour the headless
# scene is known to have (the compositor's root background, 0.3 grey).
#
# The chooser: with no $GOWL_PORTAL_CHOOSER the portal puts the list on
# gowl's own menu card (`menu-choose'), and the test answers it the way
# a person would, through the card (`menu-pick').
#
# The private bus matters.  The backend takes
# org.freedesktop.impl.portal.desktop.gowl with REPLACE, so running this
# against the developer's own session bus would displace the portal
# their desktop is using.  dbus-run-session gives it a bus of its own,
# XDG_RUNTIME_DIR a private directory, and GOWL_SOCKET points at the
# headless compositor's socket -- never at a live session's, whose menu
# card would otherwise pop up on the developer's screen.
#
# Skips rather than fails when PipeWire is not running or the tools are
# missing: this is an integration test of three daemons, and a machine
# without them has not broken anything.

set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="${OUTDIR:-$root/build/release}"
portal="$out/xdg-desktop-portal-gowl"
gowl="$out/gowl"
msg="$out/gowl-msg"

if [ ! -x "$portal" ] || [ ! -x "$gowl" ] || [ ! -x "$msg" ]; then
	echo "SKIP: portal, compositor or gowl-msg not built"
	exit 0
fi
if ! command -v dbus-run-session >/dev/null 2>&1 \
   || ! command -v gdbus >/dev/null 2>&1; then
	echo "SKIP: dbus-run-session or gdbus is missing"
	exit 0
fi
if [ ! -S "${PIPEWIRE_RUNTIME_DIR:-${XDG_RUNTIME_DIR:-/run/user/$(id -u)}}/pipewire-0" ]; then
	echo "SKIP: PipeWire is not running"
	exit 0
fi

# PipeWire lives on the real runtime dir; the compositor gets a private
# one so its sockets (Wayland and gowl.sock) cannot collide with a live
# session's.
: "${PIPEWIRE_RUNTIME_DIR:=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}}"
export PIPEWIRE_RUNTIME_DIR
runtime=$(mktemp -d -p "${XDG_RUNTIME_DIR:-/tmp}" gowl-sc-test-XXXXXX)
unset GOWL_SOCKET

comp_pid=""
cleanup () {
	[ -n "$comp_pid" ] && kill -TERM "$comp_pid" 2>/dev/null || true
	if [ -n "$KEEP_RUNTIME" ]; then echo "kept $runtime"; else rm -rf "$runtime"; fi
}
trap cleanup EXIT INT TERM

# The menu module, so the chooser has gowl's card to use.
cat >"$runtime/gowl.yaml" <<'EOF'
modules:
  menu:
    enabled: true
EOF

GOWL_DISABLE_SYSTEMD=1 XDG_RUNTIME_DIR="$runtime" \
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
GOWL_MENU_FILE="$root/data/menu.yaml" GOWL_MENU_NO_HISTORY=1 \
LD_LIBRARY_PATH="$out" \
	"$gowl" --no-c-config --config "$runtime/gowl.yaml" \
	>"$runtime/compositor.log" 2>&1 &
comp_pid=$!

# Wait for the compositor's sockets rather than sleeping a guess.
i=0
display=""
while [ $i -lt 100 ]; do
	display=$(ls "$runtime" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1)
	[ -n "$display" ] && [ -S "$runtime/gowl.sock" ] && break
	i=$((i + 1))
	sleep 0.1
done
if [ -z "$display" ] || [ ! -S "$runtime/gowl.sock" ]; then
	echo "FAIL: the compositor never opened its sockets"
	cat "$runtime/compositor.log"
	exit 1
fi

# A chooser that takes the first offered source, and counts its runs:
# a restore that worked is a Start that never ran it.
cat >"$runtime/chooser.sh" <<'EOF'
#!/bin/sh
echo run >>"$RUNTIME/chooser.count"
echo $$ >"$RUNTIME/chooser.pid"
# Held: a human who has not chosen yet.
while [ -f "$RUNTIME/hold" ]; do sleep 0.05; done
head -1
EOF
# Pickers answering in slurp's format.
cat >"$runtime/area.sh" <<'EOF'
#!/bin/sh
echo "10,20 30x40"
EOF
cat >"$runtime/point.sh" <<'EOF'
#!/bin/sh
echo "5,5 1x1"
EOF
chmod +x "$runtime/chooser.sh" "$runtime/area.sh" "$runtime/point.sh"

script=$(cat <<'SCRIPT'
set -e
DEST=org.freedesktop.impl.portal.desktop.gowl
OBJ=/org/freedesktop/portal/desktop
SC=org.freedesktop.impl.portal.ScreenCast
SS=org.freedesktop.impl.portal.Screenshot
portal_pid=""
n=0

fail () {
	echo "FAIL: $*"
	echo "--- portal log"
	cat "$RUNTIME/portal.log" 2>/dev/null || true
	[ -n "$portal_pid" ] && kill -TERM "$portal_pid" 2>/dev/null || true
	exit 1
}

# Starts the portal with the given extra environment, and waits by
# LISTING names, not by calling a method: a method call to a name nobody
# owns makes the bus ACTIVATE it from a service file, and an installed
# xdg-desktop-portal-gowl would take the name from the one built here.
start_portal () {
	env "$@" "$PORTAL" >>"$RUNTIME/portal.log" 2>&1 &
	portal_pid=$!
	i=0
	while [ $i -lt 100 ]; do
		if gdbus call --session --dest org.freedesktop.DBus \
		     --object-path /org/freedesktop/DBus \
		     --method org.freedesktop.DBus.ListNames 2>/dev/null \
		     | grep -q "$DEST"; then
			# The interfaces are exported just after the name.
			if gdbus call --session --dest $DEST --object-path $OBJ \
			     --method org.freedesktop.DBus.Properties.Get \
			     $SS version >/dev/null 2>&1; then
				return 0
			fi
		fi
		i=$((i + 1))
		sleep 0.1
	done
	fail "the portal never came up"
}

stop_portal () {
	kill -TERM "$portal_pid" 2>/dev/null || true
	wait "$portal_pid" 2>/dev/null || true
	portal_pid=""
	# The name has to be gone before the next one claims it.
	i=0
	while [ $i -lt 50 ] && gdbus call --session --dest org.freedesktop.DBus \
	     --object-path /org/freedesktop/DBus \
	     --method org.freedesktop.DBus.ListNames 2>/dev/null \
	     | grep -q "$DEST"; do
		i=$((i + 1))
		sleep 0.1
	done
}

prop () {
	gdbus call --session --dest $DEST --object-path $OBJ \
		--method org.freedesktop.DBus.Properties.Get "$1" "$2"
}

expect_prop () {
	got=$(prop "$1" "$2") || fail "$1.$2 cannot be read"
	case "$got" in
		*"uint32 $3>"*) : ;;
		*) fail "$1.$2 is $got, wanted $3" ;;
	esac
}

chooser_runs () {
	if [ -f "$RUNTIME/chooser.count" ]; then
		wc -l <"$RUNTIME/chooser.count" | tr -d ' '
	else
		echo 0
	fi
}

# create APP: a new session; its path is in $session.
create () {
	n=$((n + 1))
	session=/org/freedesktop/portal/desktop/session/gowltest/s$n
	gdbus call --session --dest $DEST --object-path $OBJ \
		--method $SC.CreateSession \
		/org/freedesktop/portal/desktop/request/gowltest/c$n "$session" \
		"$1" "{}" >"$RUNTIME/create.txt"
	grep -q "uint32 0" "$RUNTIME/create.txt" \
		|| fail "CreateSession: $(cat "$RUNTIME/create.txt")"
}

# select APP OPTIONS: SelectSources; the reply is in select.txt.
select_sources () {
	gdbus call --session --dest $DEST --object-path $OBJ \
		--method $SC.SelectSources \
		/org/freedesktop/portal/desktop/request/gowltest/sel$n "$session" \
		"$1" "$2" >"$RUNTIME/select.txt"
}

# start APP: Start; the reply is in start.txt.
start () {
	gdbus call --session --dest $DEST --object-path $OBJ \
		--method $SC.Start \
		/org/freedesktop/portal/desktop/request/gowltest/st$n "$session" \
		"$1" "" "{}" >"$RUNTIME/start.txt"
}

close_session () {
	gdbus call --session --dest $DEST --object-path "$session" \
		--method org.freedesktop.impl.portal.Session.Close >/dev/null
}

# The token in the last Start's restore_data, or nothing.
token_of_start () {
	sed -n "s/.*'restore_data': <('gowl', uint32 1, <'\([0-9a-f]*\)'>)>.*/\1/p" \
		"$RUNTIME/start.txt"
}

restore_opts () {
	echo "{'types': <uint32 1>, 'cursor_mode': <uint32 2>, 'persist_mode': <uint32 2>, 'restore_data': <('gowl', uint32 1, <'$1'>)>}"
}

GRANTS="$XDG_STATE_HOME/gowl/screencast-grants.yaml"

start_portal GOWL_PORTAL_CHOOSER="$RUNTIME/chooser.sh" \
             GOWL_PORTAL_AREA_PICKER="$RUNTIME/area.sh" \
             GOWL_PORTAL_POINT_PICKER="$RUNTIME/point.sh"

# --- Properties: the versions and what they promise -----------------
expect_prop $SC version 6
# 1 = MONITOR, 2 = WINDOW.  Both, or the backend is no better than
# portal-wlr, which is the entire reason it exists.
expect_prop $SC AvailableSourceTypes 3
# HIDDEN | EMBEDDED, and not METADATA, which is not implemented.
expect_prop $SC AvailableCursorModes 3
expect_prop $SS version 3
# Screen, Window, Area (a picker is configured), Active Window (gowl's
# socket answers).
expect_prop $SS AvailableTargets 15
echo "ok: properties"

# --- A cursor mode that was not advertised closes the session -------
create test.app
select_sources test.app "{'types': <uint32 1>, 'cursor_mode': <uint32 4>}"
grep -q "uint32 2" "$RUNTIME/select.txt" \
	|| fail "METADATA was accepted: $(cat "$RUNTIME/select.txt")"
if gdbus call --session --dest $DEST --object-path "$session" \
     --method org.freedesktop.impl.portal.Session.Close >/dev/null 2>&1; then
	fail "the session survived a cursor mode it was refused"
fi
echo "ok: an unadvertised cursor mode closes the session"

# --- A persistent cast ----------------------------------------------
create test.app
select_sources test.app "{'types': <uint32 1>, 'cursor_mode': <uint32 2>, 'persist_mode': <uint32 2>}"
grep -q "uint32 0" "$RUNTIME/select.txt" \
	|| fail "SelectSources: $(cat "$RUNTIME/select.txt")"
start test.app
grep -q "uint32 0" "$RUNTIME/start.txt" || fail "Start: $(cat "$RUNTIME/start.txt")"
grep -q "'streams'" "$RUNTIME/start.txt" || fail "no streams: $(cat "$RUNTIME/start.txt")"
grep -q "'size'" "$RUNTIME/start.txt" || fail "no size: $(cat "$RUNTIME/start.txt")"
grep -q "'position'" "$RUNTIME/start.txt" || fail "a monitor stream has no position"
grep -q "'pipewire-serial': <uint64 [1-9]" "$RUNTIME/start.txt" \
	|| fail "no pipewire-serial (v6): $(cat "$RUNTIME/start.txt")"
if grep -q "(uint32 4294967295," "$RUNTIME/start.txt"; then
	fail "the node id is SPA_ID_INVALID: $(cat "$RUNTIME/start.txt")"
fi
grep -q "'persist_mode': <uint32 2>" "$RUNTIME/start.txt" \
	|| fail "Start did not return persist_mode"
token1=$(token_of_start)
[ -n "$token1" ] || fail "Start returned no restore_data: $(cat "$RUNTIME/start.txt")"
id1=$(sed -n "s/.*'id': <'\([^']*\)'>.*/\1/p" "$RUNTIME/start.txt")
size=$(sed -n "s/.*'size': <(\([0-9]*\), \([0-9]*\))>.*/\1 \2/p" "$RUNTIME/start.txt")
[ "$(chooser_runs)" = 1 ] || fail "the first cast did not ask"
[ -f "$GRANTS" ] || fail "no grants file at $GRANTS"
[ "$(stat -c %a "$GRANTS")" = 600 ] || fail "$GRANTS is mode $(stat -c %a "$GRANTS")"
grep -q "$token1" "$GRANTS" || fail "the grant is not in $GRANTS"
close_session
echo "ok: a persistent cast (node, serial, restore_data, 0600 grant)"

# --- Restoring it asks nobody, and is single use --------------------
create test.app
select_sources test.app "$(restore_opts "$token1")"
start test.app
grep -q "uint32 0" "$RUNTIME/start.txt" || fail "restored Start: $(cat "$RUNTIME/start.txt")"
[ "$(chooser_runs)" = 1 ] || fail "a valid restore still ran the chooser"
token2=$(token_of_start)
[ -n "$token2" ] && [ "$token2" != "$token1" ] \
	|| fail "a restore did not mint a fresh grant ($token1 -> $token2)"
grep -q "'id': <'$id1'>" "$RUNTIME/start.txt" \
	|| fail "the restored stream changed its id: $(cat "$RUNTIME/start.txt")"
if grep -q "$token1" "$GRANTS"; then
	fail "the consumed grant is still on disk"
fi
close_session
echo "ok: restore skips the chooser, keeps the stream id, is single use"

# --- On disk: it survives the portal restarting ---------------------
stop_portal
start_portal GOWL_PORTAL_CHOOSER="$RUNTIME/chooser.sh" \
             GOWL_PORTAL_AREA_PICKER="$RUNTIME/area.sh" \
             GOWL_PORTAL_POINT_PICKER="$RUNTIME/point.sh"
create test.app
select_sources test.app "$(restore_opts "$token2")"
start test.app
grep -q "uint32 0" "$RUNTIME/start.txt" || fail "Start after restart: $(cat "$RUNTIME/start.txt")"
[ "$(chooser_runs)" = 1 ] || fail "a grant did not survive a portal restart"
token3=$(token_of_start)
close_session
echo "ok: a persistent grant survives a restart"

# --- Bound to its application ---------------------------------------
create other.app
select_sources other.app "$(restore_opts "$token3")"
start other.app
grep -q "uint32 0" "$RUNTIME/start.txt" || fail "Start for another app: $(cat "$RUNTIME/start.txt")"
[ "$(chooser_runs)" = 2 ] || fail "another app's token restored without asking"
close_session
echo "ok: a grant is bound to the app it was given to"

# --- A vanished source asks again -----------------------------------
token4=$(token_of_start)
stop_portal
sed -i 's/output_name: ".*"/output_name: "GONE-1"/; s/model: ".*"/model: "nobody"/' "$GRANTS"
start_portal GOWL_PORTAL_CHOOSER="$RUNTIME/chooser.sh" \
             GOWL_PORTAL_AREA_PICKER="$RUNTIME/area.sh" \
             GOWL_PORTAL_POINT_PICKER="$RUNTIME/point.sh"
create other.app
select_sources other.app "$(restore_opts "$token4")"
start other.app
grep -q "uint32 0" "$RUNTIME/start.txt" || fail "Start for a gone source: $(cat "$RUNTIME/start.txt")"
[ "$(chooser_runs)" = 3 ] || fail "a grant for a vanished output did not ask"
close_session
echo "ok: a grant whose source is gone falls back to the chooser"

# --- persist_mode 1 lives in memory only ----------------------------
create test.app
select_sources test.app "{'types': <uint32 1>, 'persist_mode': <uint32 1>}"
start test.app
grep -q "'persist_mode': <uint32 1>" "$RUNTIME/start.txt" \
	|| fail "persist_mode 1 was not granted: $(cat "$RUNTIME/start.txt")"
token5=$(token_of_start)
[ -n "$token5" ] || fail "persist_mode 1 returned no restore_data"
if grep -q "$token5" "$GRANTS"; then
	fail "a transient grant was written to disk"
fi
close_session
echo "ok: a transient grant stays in memory"

# --- Screenshot ------------------------------------------------------
png_size () {
	# The IHDR chunk: width and height, big-endian, at bytes 16..23.
	od -An -tu1 -j16 -N8 "$1" | awk '{
		printf "%d %d\n", $1*16777216+$2*65536+$3*256+$4,
		                  $5*16777216+$6*65536+$7*256+$8 }'
}
png_path () {
	sed -n "s/.*'uri': <'file:\/\/\([^']*\)'>.*/\1/p" "$1" \
		| sed 's/%20/ /g'
}
is_png () {
	[ "$(od -An -tx1 -N8 "$1" | tr -d ' \n')" = "89504e470d0a1a0a" ]
}

gdbus call --session --dest $DEST --object-path $OBJ --method $SS.Screenshot \
	/org/freedesktop/portal/desktop/request/gowltest/shot1 test.app "" \
	"{}" >"$RUNTIME/shot.txt"
grep -q "uint32 0" "$RUNTIME/shot.txt" || fail "Screenshot: $(cat "$RUNTIME/shot.txt")"
shot=$(png_path "$RUNTIME/shot.txt")
[ -f "$shot" ] || fail "Screenshot named no file: $(cat "$RUNTIME/shot.txt")"
is_png "$shot" || fail "$shot is not a PNG"
[ "$(png_size "$shot")" = "$size" ] \
	|| fail "the screenshot is $(png_size "$shot"), the output is $size"
echo "ok: Screenshot is a PNG of the whole output ($size)"

gdbus call --session --dest $DEST --object-path $OBJ --method $SS.Screenshot \
	/org/freedesktop/portal/desktop/request/gowltest/shot2 test.app "" \
	"{'target': <uint32 4>}" >"$RUNTIME/shot.txt"
grep -q "uint32 0" "$RUNTIME/shot.txt" || fail "area Screenshot: $(cat "$RUNTIME/shot.txt")"
shot=$(png_path "$RUNTIME/shot.txt")
is_png "$shot" || fail "$shot is not a PNG"
[ "$(png_size "$shot")" = "30 40" ] \
	|| fail "the area shot is $(png_size "$shot"), the picker said 30x40"
echo "ok: an area Screenshot is the picked size"

gdbus call --session --dest $DEST --object-path $OBJ --method $SS.PickColor \
	/org/freedesktop/portal/desktop/request/gowltest/pick1 test.app "" \
	"{}" >"$RUNTIME/pick.txt"
grep -q "uint32 0" "$RUNTIME/pick.txt" || fail "PickColor: $(cat "$RUNTIME/pick.txt")"
# The root background is 0.3 grey; 0.3 * 255 rounds either way.
sed -n "s/.*'color': <(\([0-9.]*\), \([0-9.]*\), \([0-9.]*\))>.*/\1 \2 \3/p" \
	"$RUNTIME/pick.txt" | awk '{
		for (i = 1; i <= 3; i++)
			if ($i < 0.29 || $i > 0.31) exit 1
		exit (NF == 3 ? 0 : 1) }' \
	|| fail "PickColor is not the 0.3 grey background: $(cat "$RUNTIME/pick.txt")"
echo "ok: PickColor reads the known background"

# --- gowl's own menu as the chooser ---------------------------------
stop_portal
start_portal GOWL_PORTAL_CHOOSER_TIMEOUT=20
runs=$(chooser_runs)
create menu.app
select_sources menu.app "{'types': <uint32 1>}"
start menu.app &
start_pid=$!
# The card is up when the menu says a choice is pending.
i=0
while [ $i -lt 100 ]; do
	"$MSG" menu-choice 2>/dev/null | grep -q pending && break
	i=$((i + 1))
	sleep 0.1
done
"$MSG" menu-choice | grep -q pending || fail "the portal never opened the menu card"
"$MSG" menu-pick 1 | grep -q '^OK' || fail "the card would not take a pick"
wait $start_pid || fail "Start through the menu card failed"
grep -q "uint32 0" "$RUNTIME/start.txt" || fail "Start via the menu: $(cat "$RUNTIME/start.txt")"
[ "$(chooser_runs)" = "$runs" ] || fail "the external chooser ran instead of the card"
"$MSG" menu-choice | grep -q 'chosen 0' || fail "the card did not record the pick"
close_session
echo "ok: gowl's menu card is the chooser inside gowl"

# Closing the request takes the card down.
create menu2.app
select_sources menu2.app "{'types': <uint32 1>}"
start menu2.app &
start_pid=$!
i=0
while [ $i -lt 100 ]; do
	"$MSG" menu-choice 2>/dev/null | grep -q pending && break
	i=$((i + 1)); sleep 0.1
done
"$MSG" menu-choice | grep -q pending || fail "the second card never opened"
gdbus call --session --dest $DEST \
	--object-path /org/freedesktop/portal/desktop/request/gowltest/st$n \
	--method org.freedesktop.impl.portal.Request.Close >/dev/null \
	|| fail "the card's Start exported no Request object"
wait $start_pid || fail "the closed Start never answered"
grep -q "uint32 1" "$RUNTIME/start.txt" || fail "closed card Start: $(cat "$RUNTIME/start.txt")"
"$MSG" menu-choice | grep -q cancelled || fail "the card is still up: $("$MSG" menu-choice)"
close_session
echo "ok: a closed request takes the menu card down"

# --- The chooser does not block the portal --------------------------
stop_portal
start_portal GOWL_PORTAL_CHOOSER="$RUNTIME/chooser.sh"

# A cast, running.
create cast.app
select_sources cast.app "{'types': <uint32 1>}"
start cast.app
grep -q "uint32 0" "$RUNTIME/start.txt" || fail "the cast: $(cat "$RUNTIME/start.txt")"
cast_serial=$(sed -n "s/.*'pipewire-serial': <uint64 \([0-9]*\)>.*/\1/p" "$RUNTIME/start.txt")
cast_session=$session

# A second app's chooser, held open.
touch "$RUNTIME/hold"
runs=$(chooser_runs)
create held.app
select_sources held.app "{'types': <uint32 1>}"
held_session=$session
held_n=$n
start held.app &
held_pid=$!
i=0
while [ $i -lt 100 ] && [ "$(chooser_runs)" = "$runs" ]; do
	i=$((i + 1)); sleep 0.05
done
[ "$(chooser_runs)" -gt "$runs" ] || fail "the held chooser never ran"

# While it waits: the cast still produces frames, and other sessions
# are still answered.
#
# A screen cast is damage-driven: an idle output delivers its first
# frame and then nothing, correctly.  The menu card going up and down
# is the damage.  media.type=Video is what a real consumer sets and
# gst does not; without it WirePlumber will not link the two and says
# `target not found'.
if command -v gst-launch-1.0 >/dev/null 2>&1 \
   && gst-inspect-1.0 pipewiresrc >/dev/null 2>&1; then
	( i=0; while [ $i -lt 60 ]; do
		"$MSG" menu-open >/dev/null 2>&1; sleep 0.1
		"$MSG" menu-close >/dev/null 2>&1; sleep 0.1
		i=$((i + 1)); done ) &
	damage_pid=$!
	timeout 20 gst-launch-1.0 -q pipewiresrc target-object="$cast_serial" \
		stream-properties="props,media.type=Video,media.category=Capture" \
		num-buffers=8 ! fakesink >"$RUNTIME/gst.log" 2>&1 \
		|| { kill $damage_pid 2>/dev/null
		     fail "the cast produced no frames while a chooser waited: $(cat "$RUNTIME/gst.log")"; }
	kill $damage_pid 2>/dev/null || true
	wait $damage_pid 2>/dev/null || true
	echo "ok: a running cast delivers frames (8) while a chooser waits"
else
	echo "NOTE: no gst pipewiresrc; frames not checked while a chooser waits"
fi
expect_prop $SC version 6
create other2.app
select_sources other2.app "{'types': <uint32 1>}"
grep -q "uint32 0" "$RUNTIME/select.txt" \
	|| fail "SelectSources was not answered while a chooser waited"
echo "ok: other sessions are served while a chooser waits"

# Closing the request withdraws the pick: Start answers cancelled and
# the chooser process is gone.
chooser_pid=$(cat "$RUNTIME/chooser.pid")
gdbus call --session --dest $DEST \
	--object-path /org/freedesktop/portal/desktop/request/gowltest/st$held_n \
	--method org.freedesktop.impl.portal.Request.Close >/dev/null \
	|| fail "the pending Start exported no Request object"
wait $held_pid || fail "the closed Start never answered"
grep -q "uint32 1" "$RUNTIME/start.txt" \
	|| fail "a closed request did not answer cancelled: $(cat "$RUNTIME/start.txt")"
i=0
while [ $i -lt 40 ] && kill -0 "$chooser_pid" 2>/dev/null; do
	i=$((i + 1)); sleep 0.05
done
if kill -0 "$chooser_pid" 2>/dev/null; then
	fail "the withdrawn chooser (pid $chooser_pid) is still running"
fi
echo "ok: a closed request cancels its pick"

# Two picks at once: the second waits its turn, and both complete.
runs=$(chooser_runs)
session=$held_session
create q1.app
select_sources q1.app "{'types': <uint32 1>}"
q1=$session
gdbus call --session --dest $DEST --object-path $OBJ --method $SC.Start \
	/org/freedesktop/portal/desktop/request/gowltest/q1 "$q1" q1.app "" "{}" \
	>"$RUNTIME/q1.txt" &
q1_pid=$!
create q2.app
select_sources q2.app "{'types': <uint32 1>}"
q2=$session
gdbus call --session --dest $DEST --object-path $OBJ --method $SC.Start \
	/org/freedesktop/portal/desktop/request/gowltest/q2 "$q2" q2.app "" "{}" \
	>"$RUNTIME/q2.txt" &
q2_pid=$!
sleep 0.5
[ "$(chooser_runs)" = $((runs + 1)) ] \
	|| fail "two choosers were shown at once (or none): $(chooser_runs) vs $runs"
rm -f "$RUNTIME/hold"
wait $q1_pid || fail "the first queued Start failed"
wait $q2_pid || fail "the second queued Start failed"
grep -q "uint32 0" "$RUNTIME/q1.txt" || fail "q1: $(cat "$RUNTIME/q1.txt")"
grep -q "uint32 0" "$RUNTIME/q2.txt" || fail "q2: $(cat "$RUNTIME/q2.txt")"
[ "$(chooser_runs)" = $((runs + 2)) ] || fail "the queued pick never showed"
echo "ok: two queued picks both complete, one after the other"

stop_portal
echo "screencast + screenshot portal PASSED"
exit 0
SCRIPT
)

# XDG_DATA_DIRS is emptied into the throwaway tree on purpose: a private
# bus still reads the system's D-Bus service files, so an INSTALLED
# xdg-desktop-portal-gowl would be activated by the first method call and
# fight the one started here for the name.  Two backends, one of them
# built from other sources, is not what this is testing.
mkdir -p "$runtime/data" "$runtime/state" "$runtime/shots"
PORTAL="$portal" MSG="$msg" RUNTIME="$runtime" \
XDG_RUNTIME_DIR="$runtime" WAYLAND_DISPLAY="$display" \
GOWL_SOCKET="$runtime/gowl.sock" \
XDG_DATA_HOME="$runtime/data" XDG_DATA_DIRS="$runtime/data" \
XDG_STATE_HOME="$runtime/state" \
GOWL_PORTAL_SCREENSHOT_DIR="$runtime/shots" \
	dbus-run-session -- sh -c "$script"
