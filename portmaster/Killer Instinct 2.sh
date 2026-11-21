#!/bin/bash
# Direct launcher for Killer Instinct 2 on PortMaster / Knulli / muOS
GAMEDIR="$(cd "$(dirname "$0")" && pwd)"
exec "$GAMEDIR/UltraCombo.sh" --game ki2 "$@"
