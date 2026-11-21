#!/bin/bash
# Direct launcher for Killer Instinct 1 on PortMaster / Knulli / muOS
GAMEDIR="$(cd "$(dirname "$0")" && pwd)"
exec "$GAMEDIR/UltraCombo.sh" --game ki1 "$@"
