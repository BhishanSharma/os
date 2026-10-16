#!/bin/sh
# mkdock.sh APPS_DIR - the Dock's apps (DOCK.CFG on the RAM disk), from every
# APPS_DIR/NAME/app.cfg that has a `dock = N` line, lowest N first:
#
#     N|name|program|icon|color
#
# The window server reads it at start (user/programs/windowserver.c).
set -e
dir=${1:-apps}
for cfg in "$dir"/*/app.cfg; do
    [ -f "$cfg" ] || continue
    program=$(basename "$(dirname "$cfg")")
    awk -v program="$program" -F '=' '
        /^[ \t]*#/ || NF < 2 { next }
        {
            key = $1; gsub(/[ \t\r]/, "", key)
            value = substr($0, index($0, "=") + 1)
            gsub(/^[ \t]+|[ \t\r]+$/, "", value)
            v[key] = value
        }
        END {
            if (v["dock"] == "") exit
            if (v["name"] == "") v["name"] = program
            if (v["icon"] == "") v["icon"] = program
            if (v["color"] == "") v["color"] = "8E8E93"
            printf "%s|%s|%s|%s|%s\n", v["dock"], v["name"], program, v["icon"], v["color"]
        }' "$cfg"
done | sort -t '|' -k1,1n
