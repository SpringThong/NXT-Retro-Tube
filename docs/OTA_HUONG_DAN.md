# Cập nhật OTA cho NXT Retro Tube — hướng dẫn cho tác giả

Repo: **https://github.com/SpringThong/NXT-Retro-Tube**

Máy của người dùng tự hỏi `https://github.com/SpringThong/NXT-Retro-Tube/releases/latest/download/ota.json`
(tức là **Release mới nhất** của repo). Muốn đẩy bản mới cho mọi người, bạn chỉ cần **tạo một Release mới**.

## 1. Cài đặt một lần duy nhất: khóa ký (đã làm sẵn cho bản 2.0)

Mỗi bản cập nhật được ký bằng **khóa riêng** của bạn, tệp `ota_private.pem` (mình gửi riêng, **không đưa lên GitHub, không gửi cho ai**).
App chỉ cài bản có chữ ký đúng, nên dù ai đó chiếm được đường truyền cũng không cài bậy vào máy người dùng được.

Để GitHub tự ký giúp bạn mỗi lần phát hành:

1. Mở repo → **Settings** → **Secrets and variables** → **Actions** → **New repository secret**.
2. **Name:** `OTA_SIGNING_KEY`
3. **Secret:** mở `ota_private.pem` bằng Notepad, chép **toàn bộ** nội dung (kể cả dòng `-----BEGIN EC PRIVATE KEY-----` và `-----END EC PRIVATE KEY-----`) rồi dán vào.
4. Bấm **Add secret**.

Cất `ota_private.pem` ở nơi an toàn (USB, Google Drive riêng tư…). Mất khóa thì các máy đang cài sẽ **không nhận** bản mới
(phải cài tay một bản có khóa mới), nên đừng làm mất.

## 2. Phát hành bản mới (mỗi lần có phiên bản mới)

Khi có bản mới (ví dụ 2.1), bạn sẽ có một thư mục gồm:
- `ota-app-nxredux.zip`, `ota-app-nextui.zip`, `ota-app-stock.zip`, `ota-app-stock-sps.zip`, `ota-app-muos.zip`, `ota-app-knulli.zip`, `ota-app-darkos.zip`
- `ota-ytdlp.zip`, `ota-deno.zip`
- các gói cài tay `NXT_Retro_Tube_v2.1_*.zip` (cho người cài lần đầu)

Các bước:
1. Vào repo → **Releases** → **Draft a new release**.
2. **Choose a tag:** gõ `v2.1` → **Create new tag**.
3. **Release title:** `NXT Retro Tube 2.1`.
4. Ô mô tả: viết ngắn gọn bản này có gì mới (tiếng Việt). **Dòng này sẽ hiện trên máy người dùng.**
5. Kéo thả **tất cả** tệp `.zip` ở trên vào khung "Attach binaries".
6. Để tick **Set as the latest release** → bấm **Publish release**.
7. Đợi khoảng 1 phút: tab **Actions** chạy "OTA manifest" và tự gắn thêm `ota.json` + `ota.json.sig` vào Release.
   Khi hai tệp đó xuất hiện ở Release là xong. Máy người dùng sẽ thấy "Có bản 2.1" trong Cài đặt → Cập nhật.

Nếu bước 7 báo lỗi đỏ: kiểm tra lại secret `OTA_SIGNING_KEY` (mục 1), rồi vào **Actions** → **OTA manifest** → **Run workflow** → gõ tag `v2.1`.

### Không muốn dùng GitHub Actions?
Trên máy tính có Python 3 và OpenSSL:
```
python3 tools/ota/make_manifest.py --repo SpringThong/NXT-Retro-Tube --tag v2.1 --assets <thư mục chứa các zip> --notes ghichu.txt --key ota_private.pem
```
rồi tải `ota.json` và `ota.json.sig` lên cùng Release.

## 3. Đưa mã nguồn mới lên repo
Mã nguồn nằm ở nhánh `main`. Khi có bản mới, thay các thư mục `src/`, `py/`, `pak/`, `res/`, `docs/`… bằng bản mới
(GitHub web: **Add file → Upload files**, kéo thả cả thư mục), ghi chú "Bản 2.1" → **Commit changes**.

## 4. Cách app cập nhật (để giải thích cho người dùng)
- App biết mình là ROM nào (NX Redux, NextUI, Stock, muOS, Knulli, dArkOS) và máy nào (Brick / Brick Pro / Smart Pro S / R36S),
  nên chỉ lấy đúng gói `ota-app-<rom>.zip` của máy đó. Cài nhầm bản Stock Brick lên Smart Pro S thì OTA cũng tự sửa đúng gói.
- Chỉ tải phần có phiên bản khác: app (vài MB), yt-dlp, Deno.
- Tải xong kiểm tra SHA-256, rồi mới thay tệp; tệp cũ giữ ở `.ota/backup`. Nếu bản mới không mở được 2 lần liên tiếp,
  `launch.sh` tự trả lại bản cũ.
- Đồng hồ máy sai (máy không có pin đồng hồ) cũng không sao: chữ ký và SHA-256 mới là thứ quyết định.

## 5. Số phiên bản
- App: `#define APP_VERSION` trong `src/app.h` (ví dụ `"2.1"`). Máy chỉ cập nhật khi số **lớn hơn** bản đang có.
- yt-dlp / Deno: `YTDLP_VERSION`, `DENO_VERSION` khi chạy `package.sh` (máy cập nhật khi số **khác** bản đang có).
