#!/bin/bash
# PortMaster launcher for UltraCombo (Killer Instinct & Killer Instinct 2)
# Targeted at Anbernic RG34XX (Allwinner H700 / Mali-G31) and similar AArch64
# Linux handhelds running muOS, Knulli, ROCKNIX, ArkOS, AmberELEC or Stock OS.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

GAMEDIR="$(cd "$(dirname "$0")" && pwd)/ultracombo"
if [ ! -d "$GAMEDIR" ] && [ -n "$directory" ]; then
  GAMEDIR="/$directory/ports/ultracombo"
fi
cd "$GAMEDIR" || exit 1

exec >> "$GAMEDIR/log.txt" 2>&1

USE_PORTMASTER=0
if [ -n "$controlfolder" ] && [ -f "$controlfolder/control.txt" ]; then
  source "$controlfolder/control.txt"
  [ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
  get_controls
  USE_PORTMASTER=1
fi

echo "=== UltraCombo ARM64 (KillerArm) ==="
echo "Date: $(date)"
echo "Device: ${DEVICE_NAME:-unknown}  CFW: ${CFW_NAME:-unknown}  Arch: $(uname -m)"

# Auto-extract ROM chips from any MAME zip in run/ if not already extracted
if [ -d "$GAMEDIR/run" ]; then
  for zipfile in "$GAMEDIR"/run/*.zip "$GAMEDIR"/run/*.ZIP; do
    [ -f "$zipfile" ] || continue
    if command -v unzip >/dev/null 2>&1; then
      echo "Unpacking ROM files from $(basename "$zipfile")..."
      unzip -n -q "$zipfile" "*.u*" -d "$GAMEDIR/run" 2>/dev/null || true
      unzip -n -q "$zipfile" "u*-*" -d "$GAMEDIR/run" 2>/dev/null || true
      unzip -n -q "$zipfile" "*_l1.*" -d "$GAMEDIR/run" 2>/dev/null || true
    fi
  done
fi

# Use the firmware's hardware-accelerated SDL2/Mali libraries; only append
# optional port-local libraries after system paths.
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$GAMEDIR/libs.aarch64"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
# Keep physical South/East/West/North positions consistent on Nintendo-labeled
# handhelds such as the Anbernic RG34XX.
export SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS=0
export SDL_RENDER_VSYNC=1
export KI2_GLES=1

# Lock CPU governor to performance while playing if permitted: prevents H700
# ondemand/schedutil frequency drops during vblank waits from causing frame dips.
ORIG_GOV=""
if [ -w /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
  ORIG_GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true)
  for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$g" 2>/dev/null || true
  done
elif command -v "$ESUDO" >/dev/null 2>&1 && [ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
  ORIG_GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true)
  $ESUDO sh -c 'for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$g" 2>/dev/null || true; done' 2>/dev/null || true
fi

pm_platform_helper "$GAMEDIR/ultracombo.aarch64" >/dev/null 2>&1 || true

./ultracombo.aarch64 --data-dir "$GAMEDIR" --fullscreen "$@"
RC=$?

# Restore previous CPU governor on exit
if [ -n "$ORIG_GOV" ]; then
  if [ -w /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
      echo "$ORIG_GOV" > "$g" 2>/dev/null || true
    done
  elif command -v "$ESUDO" >/dev/null 2>&1; then
    $ESUDO sh -c "for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo '$ORIG_GOV' > \"\$g\" 2>/dev/null || true; done" 2>/dev/null || true
  fi
fi

pm_finish >/dev/null 2>&1 || true
exit $RC
