#!/bin/sh
# Development launcher: place beside Yakumo, game/, overlays/ and fonts/.
set -eu
port_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$port_dir"
[ -f game/EBOOT.ELF ] && [ -f game/disc.iso ] || {
    echo 'Yakumo needs the prepared Japanese HD game in game/.' >&2
    exit 1
}
if [ ! -e game/settings.ini ]; then
    cat > game/settings.ini <<'SETTINGS'
video.fullscreen=1
video.aspect=original
input.mouse=0
SETTINGS
fi
ulimit -s 65536
export MHP3RD_GAME_DIR="$port_dir/game"
export MHP3RD_DATA_DIR="$port_dir/game"
export MHP3RD_OVERLAY_DIR="$port_dir/overlays"
export MHP3RD_FONT="$port_dir/fonts/NotoSansCJKjp-Regular.otf"
export MHP3RD_INTERNAL_SCALE=1
export MHP3RD_TEXTURE_PACK=0
export MHP3RD_FRAME_RATE=30
export SDL_VIDEODRIVER=mali
unset SDL_AUDIODRIVER SDL_AUDIO_DRIVER AUDIODEV SDL_AUDIO_ALSA_DEFAULT_PLAYBACK_DEVICE
exec "$port_dir/Yakumo" "$port_dir/game"
