# NXT Retro Tube 2.0 — mã nguồn

**Tác giả:** Nguyễn Xuân Thông
**Ủng hộ tác giả:** MoMo 0339838267 · Vietcombank (VCB) 0581000786219 · PayPal thongkhung2@gmail.com

Ứng dụng xem YouTube cho TrimUI Brick / Brick Pro / Smart Pro S và máy RK3326 (R36S), một mã nguồn cho bảy bản:
NX Redux, NextUI, firmware gốc TrimUI (Stock) cho Brick / Brick Pro, Stock cho Smart Pro S (TG5050), muOS, Knulli, và dArkOS (R36S). Giấy phép: GPL-3.0-only (xem `licenses/`).

## Cấu trúc
| Thư mục | Nội dung |
|---|---|
| `src/` | Ứng dụng C (SDL2): giao diện, trình phát FFmpeg, API YouTube trực tiếp, Telex, âm lượng/màn hình/keymon; `input_evdev.c` (phím theo mã evdev, muOS/Knulli); `stb_image.h` (PNG, muOS/Knulli) |
| `py/nxt/` | Plugin yt-dlp `nxttube:serve` (tiến trình yt-dlp thường trú, dùng khi API trực tiếp không đủ) |
| `res/` | Font (Be Vietnam Pro, VT323 — OFL), icon Lucide (ISC), chứng chỉ gốc, icon app |
| `pak/nxredux`, `pak/nextui`, `pak/stock` | `launch.sh` và metadata cho từng ROM; `pak/nxredux/integration/` cài icon menu Tools |
| `pak/muos` | `mux_launch.sh`, `mux_lang.ini`, glyph SVG cho mục Applications của muOS |
| `pak/knulli` | `ports/NXT Retro Tube.sh` (mục Ports của EmulationStation) |
| `pak/darkos` | `ports/NXT Retro Tube.sh` cho dArkOS (R36S) |
| `docs/` | Hướng dẫn cài đặt / sử dụng cho từng ROM (tiếng Việt) |
| `launcher/` | Bản vá launcher NX Redux cho icon Tools: `nxredux-2.4.1-tiles-nxt-icons.patch` (2.4.1, icon NXT Retro Tube + NXT Retro Music) và bản cũ cho 2.1.2; launcher đã dựng nằm trong `pak/nxredux/integration/` |
| `third_party/` | Thư viện tĩnh dựng sẵn (Mbed TLS, SDL2_ttf + HarfBuzz/FreeType/FriBidi/utf8proc), header, `libmsettings.so` (NextUI, bản Stock), `libstdc++.so.6` |
| `deps/build-ffmpeg.sh` | Dựng FFmpeg 6.1.6 tĩnh (giải mã H.264/HEVC/VP9/AAC/Opus, HTTPS qua Mbed TLS) |

Mã ROM chọn lúc biên dịch: `-DNXT_TUBE_ROM_NXREDUX | NEXTUI | STOCK | MUOS | KNULLI | DARKOS`
- NX Redux: dùng `libmsettings.so` của firmware, có cài icon menu Tools.
- NextUI: dùng `libmsettings.so` kèm theo (NextUI).
- Stock: dùng `libmsettings.so` bản Stock, tạm dừng keymon khi app đang hiện (như NXT Retro Music), giữ cờ `stay_awake/stay_alive`, đọc trạng thái jack tai nghe, nút Home mở bảng nhanh của firmware.
- muOS / Knulli (`NXT_TUBE_PW`): âm thanh qua PipeWire (SDL pipewire → pulseaudio → alsa), DAC USB chọn theo tên thiết bị; âm lượng do hệ thống xử lý (đọc/đặt bằng `wpctl` hoặc `knulli-audio`); phím đọc thẳng từ evdev theo mã phím (muOS-Keys / TRIMUI Brick Controller đánh số nút khác nhau), không chiếm thiết bị; nút nguồn tắt màn hình nhờ cờ hệ thống (muOS `/run/muos/caffeine`, Knulli `/var/run/power-button.wake`), chặn tự tối/ngủ (muOS `idle_game_inhibit`, Knulli `battery-saver/*.pause`); không cần SDL2_image (icon PNG qua stb_image), libstdc++ liên kết tĩnh — chỉ cần SDL2 ≥ 2.0.18 của hệ thống.
- dArkOS (`NXT_TUBE_ROM_DARKOS`, dùng chung đường muOS/Knulli): phím SELECT/START/L3/R3 của "GO-Super Gamepad" là
  `BTN_TRIGGER_HAPPY1..4`; R3 (phím nóng của ogage) bấm riêng mở Bảng nhanh; Tự động = 480p30; âm lượng qua `current_volume`/`amixer`;
  đèn nền qua `/sys/class/backlight` (hoặc `brightnessctl`); tiến trình con tên `ffplay-nxttube` khi đang phát để `auto_suspend.py` không cho máy ngủ.

## Chế độ trẻ em (`src/kids.c`)
- Lưu trong `kids.json` (thư mục dữ liệu): bật/tắt, mã PIN (băm FNV-1a có muối, không lưu chữ thường), giới hạn phút/ngày,
  khung giờ, nhắc nghỉ mắt, chỉ kênh đã duyệt, danh sách kênh đã duyệt, từ khóa cấm, số giây xem từng ngày (14 ngày) và 200 video gần nhất.
- Tìm kiếm InnerTube gửi `context.user.enableSafetyMode = true` (Chế độ hạn chế của YouTube) khi bật.
- Lọc: `Kids_allowed()` áp vào trang chủ, kết quả tìm, trang kênh, hàng "Tiếp theo" và khi mở video; từ khóa so khớp sau khi bỏ dấu tiếng Việt.
- Giao diện trong `src/main.c`: trang Cài đặt → Chế độ trẻ em (`SC_KIDS`), hộp nhập PIN, màn khóa hết giờ / ngoài giờ, màn nghỉ mắt.

## Mới ở 2.0
- **OTA** (`src/ota.c`, `tools/ota/make_manifest.py`, `.github/workflows/ota-release.yml`, hướng dẫn `docs/OTA_HUONG_DAN.md`):
  `ota.json` ký ECDSA P-256 (khóa công khai nằm trong `ota.c`), ba thành phần `app-<kênh>` / `ytdlp` / `deno`, kênh = ROM + máy
  (`/proc/device-tree/compatible`: sun55i = Smart Pro S). Tải bằng FFmpeg avio (TLS kiểm tra chứng chỉ, sai đồng hồ thì vẫn an toàn nhờ chữ ký),
  giải nén zip (zlib) vào `.ota/stage`, đổi chỗ bằng rename, bản cũ ở `.ota/backup`, `launch.sh` khôi phục nếu bản mới lỗi 2 lần.
  Khởi động lại bằng `execv` cùng PID. Repo nhận cập nhật ghi trong `res/ota-repo.txt`.
- **Video thiếu nhi nhanh hơn:** `py/nxt/yt_dlp_plugins/extractor/nxt_jsc.py` — JS challenge provider cho yt-dlp, giữ **một** tiến trình Deno
  thường trú chạy bộ giải yt-dlp-ejs; mỗi phiên bản player chỉ tiền xử lý một lần (lưu cả ra đĩa: `nxt-jsc/pre-*.js`), các challenge sau
  trả lời trong vài ms. Lỗi thì yt-dlp tự quay về provider Deno gốc. `src/yt.c` nhớ client chạy được (`kids_best`) và mở lại prefetch.
- **Stock (Brick):** chụp và trả lại độ sáng / màu màn hình quanh `InitSettings()` của libmsettings (trước đây làm tối màn hình), tắt/bật màn hình trả đúng mức cũ.
- **Nút Home:** chống nhận 2 lần (450 ms); có `trimui_osdd` thì để keymon mở OSD (chỉ tự gửi `/tmp/show_osdd` khi keymon đang bị dừng)
  và bỏ mọi phím khi `/tmp/trimui_osd/osdd_show_up` tồn tại.
- **Smart Pro S (Stock, `ROM=stock_sps`, `-DNXT_TUBE_DEV_TG5050`):** cửa sổ 1280×720 (KMS/DRM), giao diện 4:3 ở giữa, video toàn màn hình
  vẽ ở độ phân giải gốc (`D_videoNative`); không dùng libmsettings, không dừng keymon; âm lượng `DAC Volume` qua amixer (luồng riêng);
  MENU mở Bảng nhanh. Glibc 2.33 / SDL2 2.32 của máy mới hơn bản ta liên kết nên dùng chung toolchain tg5040.

## Phụ đề dịch tự động (`src/subs.c`, mới ở 1.5)
- Lấy danh sách phụ đề của video (`captions.playerCaptionsTracklistRenderer.captionTracks` từ API trực tiếp, hoặc
  `subtitles` / `automatic_captions` qua plugin yt-dlp → khóa `nxt_captions`).
- Chọn track: phụ đề viết tay đúng ngôn ngữ người xem nếu có; nếu video nói tiếng khác thì lấy track ngôn ngữ gốc
  (thường là phụ đề tự động của YouTube) và thêm `&tlang=<ngôn ngữ>` để **YouTube dịch**; luôn tải dạng `fmt=json3`.
- Tải: gọi thẳng (`Innertube_get`, UA Chrome); YouTube từ chối thì nhờ yt-dlp (`op: fetch`, giả lập TLS trình duyệt khi có curl_cffi).
- Chế độ (`settings.json`): `sub_mode` 0 Tự động (chỉ hiện khi video khác ngôn ngữ) / 1 Luôn bật / 2 Tắt; `sub_lang` ("" = theo ngôn ngữ app);
  `sub_size` 0–2. Bảng Tùy chọn video đổi riêng cho video đang xem (theo cài đặt / dịch / gốc / tắt).
- Tiếng Việt và English dùng font sẵn có (Be Vietnam Pro). Gói chữ Hàn / Nhật / Trung / Thái (Noto Sans, SIL OFL) tải khi người dùng bấm A,
  từ `raw.githubusercontent.com/google/fonts` cố định commit `e7db6ca6…`, kiểm tra kích thước + SHA-256 (Mbed TLS), lưu vào `<dữ liệu>/fonts/`.
  Font biến thiên mở bằng named instance 5 (Medium) qua `TTF_OpenFontIndex(path, size, 5 << 16)`.
- Vẽ: `D_subtitle()` trong `src/draw.c` (ngắt dòng theo khoảng trắng, hoặc theo ký tự với chữ Hán/Nhật/Thái; nền tối từng dòng).
- Kiểm thử: `NXT_TUBE_FONTS_BASE` trỏ nơi tải gói chữ sang máy chủ thử.

## Lấy luồng YouTube
1. API InnerTube trực tiếp với client visionOS (nhanh, không cần JS) — `src/innertube.c`.
2. Nếu visionOS từ chối (ví dụ video "made for kids": hoạt hình, POPS Kids…), tiến trình yt-dlp thường trú
   (`py/nxt`) thử lần lượt `tv_samsung → web_embedded → tv → default → visionos-page → web_safari`;
   các client TV / nhúng cần trình phát JS của YouTube, giải bằng Deno (`--js-runtimes deno`).
3. Không có tiến trình thường trú thì gọi yt-dlp trực tiếp với cùng thứ tự client.

## Nhiệt độ / pin
- CPU (`Sys_cpuLevel` trong `src/system.c`): menu và video thường dùng governor on-demand; `performance` chỉ khi video 60 fps,
  1080p hoặc bộ giải mã từng bị trễ (giữ đến hết video đó); tắt màn hình / nghe nền: on-demand và giới hạn xung tối đa còn một nửa.
  Governor lúc mở app được trả lại khi thoát. Các `launch.sh` không còn ép `performance`.
- Vẽ màn hình (vòng lặp chính trong `src/main.c`): đủ 60 lần/giây khi đang bấm phím, theo nhịp khung hình khi video chạy
  (`Player_frameDue`), ~10 lần/giây khi đứng yên. Không lấy trước (prefetch) các video cần trình phát JS (Deno) — chỉ giải khi bạn mở video.

## Cần chuẩn bị
1. Toolchain GCC 8.3 tg5040 + sysroot TrimUI (đặt vào `sdk/compiler`, hoặc chỉ đường bằng `TOOLCHAIN=`):
   - https://github.com/LoveRetro/gcc-arm-8.3-aarch64-tg5040/releases (gcc-8.3.0-aarch64-nextui-linux-gnu)
   - sysroot: https://github.com/trimui/toolchain_sdk_smartpro/releases/download/20231018/SDK_usr_tg5040_a133p.tgz
2. FFmpeg 6.1.6: giải nén vào `deps/ffmpeg-n6.1.6`, chạy `deps/build-ffmpeg.sh`.
3. Thành phần chạy kèm (không có trong mã nguồn vì dung lượng), đặt trong `runtime/`:
   - `runtime/deno` — Deno 2.9.8 aarch64 (https://github.com/denoland/deno/releases, `deno-aarch64-unknown-linux-gnu.zip`)
   - `runtime/yt-dlp-runtime/` — yt-dlp 2026.10.21 bản thư mục cho Linux aarch64 (`yt-dlp_linux_aarch64.zip` trên https://github.com/yt-dlp/yt-dlp/releases)

## Dựng
```sh
TOOLCHAIN=/path/to/compiler ./build.sh          # cả bảy bản -> build/<rom>/nxt-retro-tube.elf
ROM=stock TOOLCHAIN=/path/to/compiler ./build.sh # một ROM
RUNTIME=./runtime ./package.sh                   # dist/NXT_Retro_Tube_v2.0_{NXRedux,NextUI,Stock,Stock_SmartProS,muOS,Knulli,dArkOS_R36S}.zip + dist/ota-*.zip
```
Bảy tệp `.elf` dựng từ mã nguồn này (với `SOURCE_DATE_EPOCH=1793034000`) trùng từng byte với bản phát hành 2.0.

## Dựng lại launcher NX Redux 2.4.1 (icon Tools)
```sh
git clone https://github.com/nx-redux/nx-handheld && cd nx-handheld && git checkout v2.4.1
patch -p0 < /path/to/launcher/nxredux-2.4.1-tiles-nxt-icons.patch   # hoặc sửa tay 2 dòng trong tiles.c
# môi trường: PLATFORM=tg5040 CROSS_COMPILE=<toolchain>/bin/aarch64-nextui-linux-gnu-
#   PREFIX=<sysroot>/usr PREFIX_LOCAL=<thư mục out> PKG_CONFIG_SYSROOT_DIR=<sysroot> (sdl2.pc, glesv2.pc)
cd workspace/tg5040/libmsettings && make
cd ../../all/libgametimedb && make
cd ../minarch/rcheevos && make build install CC=${CROSS_COMPILE}gcc AR=${CROSS_COMPILE}ar
cd ../../nextui && make CC="${CROSS_COMPILE}gcc -Wl,--as-needed"   # cần samplerate.h + libsamplerate.a trong PREFIX_LOCAL
```
Kết quả `build/tg5040/nextui.elf` = `pak/nxredux/integration/nextui-v2.4.1-nxt-icons.elf`.

## Kiểm tra không cần máy
Ứng dụng có móc kiểm thử: `NXT_TUBE_KEYS="A,,R#"` phát lại phím, `NXT_TUBE_SHOT=/tmp/shot` chụp màn hình (PNG; BMP trên bản muOS/Knulli),
`SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy`, chạy bằng `qemu-aarch64 -L <sysroot>`.
`NXT_TUBE_INNERTUBE_BASE` / `NXT_TUBE_INNERTUBE_URL` trỏ API sang máy chủ thử; `NXT_TUBE_OTA_BASE` trỏ OTA sang máy chủ thử.
