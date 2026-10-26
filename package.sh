#!/bin/sh
# Assemble the installer zips (one per ROM / device) and the OTA packages from build/
# plus the runtime downloads:
#   RUNTIME=/path/with/{deno,yt-dlp-runtime/} ./package.sh
# Output in dist/:
#   NXT_Retro_Tube_v<ver>_<Target>.zip   full installers (copy to the SD card)
#   ota-app-<channel>.zip                the app part, per ROM / device (OTA)
#   ota-ytdlp.zip, ota-deno.zip          the shared runtimes (OTA)
# Then tools/ota/make_manifest.py writes and signs ota.json (see docs/OTA_HUONG_DAN.md).
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
RUNTIME=${RUNTIME:-$ROOT/runtime}
VER=$(sed -n 's/#define APP_VERSION "\(.*\)"/\1/p' "$ROOT/src/app.h")
YTDLP_VERSION=${YTDLP_VERSION:-2026.10.21}
DENO_VERSION=${DENO_VERSION:-2.9.8}
[ -x "$RUNTIME/deno" ] && [ -x "$RUNTIME/yt-dlp-runtime/yt-dlp" ] || { echo "need $RUNTIME/deno and $RUNTIME/yt-dlp-runtime/yt-dlp"; exit 1; }
DIST=$ROOT/dist; rm -rf "$DIST"; mkdir -p "$DIST/stage" "$DIST/ota"

common() {   # $1 = pak dir, $2 = build (rom), $3 = pak template
    mkdir -p "$1/bin/tg5040" "$1/bin/yt-dlp-plugins"
    cp "$ROOT/build/$2/nxt-retro-tube.elf" "$1/bin/tg5040/"
    case "$2" in muos|knulli|darkos) ;; *) cp "$ROOT/third_party/libstdc++.so.6" "$1/bin/tg5040/" ;; esac
    cp "$RUNTIME/deno" "$1/bin/"; cp -a "$RUNTIME/yt-dlp-runtime" "$1/bin/"
    echo "$YTDLP_VERSION" > "$1/bin/yt-dlp-runtime/.nxt-version"; echo "$DENO_VERSION" > "$1/bin/.deno-version"
    cp -r "$ROOT/py/nxt" "$1/bin/yt-dlp-plugins/"
    cp -r "$ROOT/res" "$1/res"; mv "$1/res/icon.png" "$1/icon.png"
    cp -r "$ROOT/licenses" "$1/licenses"
    cp "$ROOT/pak/$3/"*.sh "$ROOT/pak/$3/"*.json "$1/" 2>/dev/null || :
    chmod +x "$1/bin/deno" "$1/bin/tg5040/nxt-retro-tube.elf"; [ -f "$1/launch.sh" ] && chmod +x "$1/launch.sh"
    [ "$2" = nxredux ] || rm -f "$1/licenses/NextUI-launcher-NOTICE.txt"   # the launcher ships with NX Redux only
    case "$2" in muos|knulli|darkos|stock_sps)   # no firmware settings library shipped: drop its notice line
        rm -f "$1/licenses/NextUI-GPL-3.0.txt" "$1/licenses/NextUI-launcher-NOTICE.txt"
        sed -i -e '/^bin\/tg5040\/libmsettings.so/d' "$1/licenses/NOTICE.txt" ;;
    esac
    case "$2" in muos|knulli|darkos)
        sed -i -e 's/^SDL2 and SDL2_image are provided by the device firmware (zlib)\./SDL2 is provided by the device firmware (zlib).\nstb_image 2.30 (PNG icons): public domain \/ MIT, Sean Barrett. https:\/\/github.com\/nothings\/stb/' \
            -e 's/^libstdc++ (GPL-3.0 with GCC Runtime Library Exception)\./libstdc++ \/ libgcc, linked statically (GPL-3.0 with GCC Runtime Library Exception)./' "$1/licenses/NOTICE.txt" ;;
    esac
}
ota_app() {   # $1 = pak dir, $2 = channel
    T=$DIST/ota/app-$2; rm -rf "$T"; mkdir -p "$T"
    (cd "$1" && tar cf - --exclude=./bin/deno --exclude=./bin/.deno-version --exclude=./bin/yt-dlp-runtime .) | (cd "$T" && tar xf -)
    printf '{"component": "app-%s", "version": "%s"}\n' "$2" "$VER" > "$T/component.json"
    (cd "$T" && zip -qrX9 "$DIST/ota-app-$2.zip" .)
}

P="$DIST/stage/nxredux/Tools/NXT Retro Tube.pak"; mkdir -p "$P"; common "$P" nxredux nxredux; cp -r "$ROOT/pak/nxredux/integration" "$P/"; ota_app "$P" nxredux
P="$DIST/stage/nextui/Tools/tg5040/NXT Retro Tube.pak"; mkdir -p "$P"; common "$P" nextui nextui; cp "$ROOT/third_party/msettings/nextui/libmsettings.so" "$P/bin/tg5040/"; ota_app "$P" nextui
P="$DIST/stage/stock/Apps/NXT_Retro_Tube"; mkdir -p "$P"; common "$P" stock stock; cp "$ROOT/third_party/msettings/stock/libmsettings.so" "$P/bin/tg5040/"; ota_app "$P" stock
P="$DIST/stage/stock_sps/Apps/NXT_Retro_Tube"; mkdir -p "$P"; common "$P" stock_sps stock_sps; ota_app "$P" stock-sps
P="$DIST/stage/muos/MUOS/application/NXT Retro Tube"; mkdir -p "$P"; common "$P" muos muos
cp "$ROOT/pak/muos/mux_launch.sh" "$ROOT/pak/muos/mux_lang.ini" "$P/"; cp -r "$ROOT/pak/muos/glyph" "$P/"; chmod +x "$P/mux_launch.sh"; ota_app "$P" muos
P="$DIST/stage/knulli/roms/ports/nxt-retro-tube"; mkdir -p "$P"; common "$P" knulli knulli; ota_app "$P" knulli
cp "$ROOT/pak/knulli/ports/NXT Retro Tube.sh" "$DIST/stage/knulli/roms/ports/"; chmod +x "$DIST/stage/knulli/roms/ports/NXT Retro Tube.sh"
P="$DIST/stage/darkos/roms/ports/nxt-retro-tube"; mkdir -p "$P"; common "$P" darkos darkos; ota_app "$P" darkos
cp "$ROOT/pak/darkos/ports/NXT Retro Tube.sh" "$DIST/stage/darkos/roms/ports/"; chmod +x "$DIST/stage/darkos/roms/ports/NXT Retro Tube.sh"

# shared runtimes (OTA)
T=$DIST/ota/ytdlp; mkdir -p "$T"; cp -a "$RUNTIME/yt-dlp-runtime" "$T/"; echo "$YTDLP_VERSION" > "$T/yt-dlp-runtime/.nxt-version"
printf '{"component": "ytdlp", "version": "%s"}\n' "$YTDLP_VERSION" > "$T/component.json"; (cd "$T" && zip -qrX9 "$DIST/ota-ytdlp.zip" .)
T=$DIST/ota/deno; mkdir -p "$T"; cp "$RUNTIME/deno" "$T/"; echo "$DENO_VERSION" > "$T/.deno-version"
printf '{"component": "deno", "version": "%s"}\n' "$DENO_VERSION" > "$T/component.json"; (cd "$T" && zip -qrX9 "$DIST/ota-deno.zip" .)
rm -rf "$DIST/ota"

for R in NXRedux:nxredux NextUI:nextui Stock:stock Stock_SmartProS:stock_sps muOS:muos Knulli:knulli dArkOS:darkos; do
    cp "$ROOT/docs/HUONG_DAN_NXT_Retro_Tube_v${VER}_${R%%:*}.md" "$DIST/stage/${R##*:}/" 2>/dev/null || :
done
(cd "$DIST/stage/nxredux" && zip -qry9 "$DIST/NXT_Retro_Tube_v${VER}_NXRedux.zip" Tools HUONG_DAN_NXT_Retro_Tube_v${VER}_NXRedux.md)
(cd "$DIST/stage/nextui" && zip -qry9 "$DIST/NXT_Retro_Tube_v${VER}_NextUI.zip" Tools HUONG_DAN_NXT_Retro_Tube_v${VER}_NextUI.md)
(cd "$DIST/stage/stock" && zip -qry9 "$DIST/NXT_Retro_Tube_v${VER}_Stock.zip" Apps HUONG_DAN_NXT_Retro_Tube_v${VER}_Stock.md)
(cd "$DIST/stage/stock_sps" && zip -qry9 "$DIST/NXT_Retro_Tube_v${VER}_Stock_SmartProS.zip" Apps HUONG_DAN_NXT_Retro_Tube_v${VER}_Stock_SmartProS.md)
(cd "$DIST/stage/muos" && zip -qry9 "$DIST/NXT_Retro_Tube_v${VER}_muOS.zip" MUOS HUONG_DAN_NXT_Retro_Tube_v${VER}_muOS.md)
(cd "$DIST/stage/knulli" && zip -qry9 "$DIST/NXT_Retro_Tube_v${VER}_Knulli.zip" roms HUONG_DAN_NXT_Retro_Tube_v${VER}_Knulli.md)
cp "$DIST/stage/darkos/HUONG_DAN_NXT_Retro_Tube_v${VER}_dArkOS.md" "$DIST/stage/darkos/roms/" 2>/dev/null || :
(cd "$DIST/stage/darkos/roms" && zip -qry9 "$DIST/NXT_Retro_Tube_v${VER}_dArkOS_R36S.zip" ports HUONG_DAN_NXT_Retro_Tube_v${VER}_dArkOS.md)   # EASYROMS root = /roms
rm -rf "$DIST/stage"
ls -l "$DIST"
