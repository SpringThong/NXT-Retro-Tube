# NXT Retro Tube 2.1.2 — dArkOS (R36S và máy RK3326 cùng kiểu)

## Yêu cầu
- dArkOS trên R36S (và máy cùng bo mạch RG351MP / RK3326, tay cầm "GO-Super Gamepad"). Cần Wi-Fi (USB Wi-Fi trên R36S).
- Đã đối chiếu với mã nguồn dArkOS (christianhaitian/dArkOS, 10/2026): mã phím trong device tree `rk3326-rg351mp-linux`
  (SELECT = F1, START = F2, L3 = F3, R3/phím nóng = F4), daemon phím nóng `ogage`, `current_volume` / `amixer`,
  tự ngủ `auto_suspend.py`. Chưa chạy thử trên máy thật — bản muOS đã chạy được trên dArkOS nên phần còn lại giống bản đó.

## Cài đặt
Chép thư mục `ports` trong gói này vào thẻ nhớ chứa game (phân vùng EASYROMS — chép đè vào thư mục `ports` có sẵn), để có:
- `ports/NXT Retro Tube.sh`
- `ports/nxt-retro-tube/` (app)

Nếu bạn để game ở thẻ thứ hai (`/roms2`) thì chép vào `ports` của thẻ đó. Sau đó trong EmulationStation:
**Start → Game settings → Update gamelists** (hoặc khởi động lại). App nằm trong **Ports**.

Dữ liệu (lịch sử, xem sau, kênh, video đã tải) nằm trong `ports/nxt-retro-tube/data`.

## Mới ở 2.1.2
- **Sửa lỗi 2.1.1 không mở được live** ("Không mở được luồng"): YouTube để **tiếng của live ở một danh sách riêng**, nên mở riêng danh sách hình thì không có tiếng.
  Giờ app nhận ra kiểu này và mở cả hình lẫn tiếng, vẫn **chỉ tải đúng chất lượng đã chọn** (không còn 240p). Nếu một cách mở bị lỗi, app tự thử lại bằng danh sách đầy đủ.
  Nghe nền / Chỉ âm thanh với live: chỉ tải phần tiếng.

## Mới ở 2.1.1
- **Sửa lỗi xem live chỉ được 240p:** trước đây app đưa cả danh sách chất lượng của luồng trực tiếp cho bộ phát, bộ phát tự chọn mức thấp (thường 240p).
  Giờ app đọc danh sách đó trước và mở **đúng chất lượng đang chọn** (Tự động = 480p trên R36S; chọn 1080p / 720p / 480p / 360p cũng được áp dụng cho live).
  Live cũng vào nhanh hơn vì bộ phát chỉ tải một mức chất lượng. Khi nghe nền / tắt màn hình / Chỉ âm thanh, live tự chuyển sang mức nhỏ nhất và không giải mã hình (mát máy, ít dữ liệu).

## Mới ở 2.1
- **Hẹn giờ tắt:** trong **Bảng nhanh** hoặc **Tùy chọn video (SELECT) → Hẹn giờ tắt**, bấm A để chọn 15 / 30 / 45 / 60 / 90 phút hoặc **Hết video này**.
  Hết giờ thì **thoát app** (mặc định) hoặc **dừng phát** — chọn ở Cài đặt → **Khi hết giờ hẹn**. Còn 1 phút sẽ hiện nhắc, bấm **A** để thêm 15 phút;
  30 giây cuối tiếng nhỏ dần. Vẫn đếm khi tắt màn hình / nghe nền. Khi bật Chế độ trẻ em, hết giờ chỉ dừng phát (thoát app vẫn cần PIN).
- **60 hình/giây thật (thử nghiệm):** Cài đặt → bật để video 60 hình hiện đủ 60 hình (mượt hơn, nóng và tốn pin hơn). Mặc định tắt.
  Nếu máy giải mã không kịp, app tự lùi về ~30 hình cho video đó để không giật, lệch tiếng.
- **Bàn phím có ký tự đặc biệt:** phím **#+=** (hàng dưới) mở trang ký hiệu ! @ # $ % ^ & * ( ) - _ = + / \ : ; ' " , . ? ~ [ ]; phím **abc** quay lại chữ.
- Từ bản 2.0.1, các bản mới đến **qua OTA**: Cài đặt → Cập nhật (OTA) → A.

## Mới ở 2.0.1
- Sửa lỗi Cập nhật (OTA) báo "không kiểm tra được, cần Wi-Fi": thư viện bảo mật của app chưa hỗ trợ kiểu chữ ký RSA-PSS mà máy chủ tải tệp của GitHub dùng (TLS 1.3). Cũng sửa luôn việc tải gói chữ phụ đề Hàn / Nhật / Trung / Thái.

## Mới ở 2.0
- **Cập nhật OTA ngay trên máy:** Cài đặt → **Cập nhật (OTA)** → A. App tự nhận ra ROM và máy đang dùng để tải đúng bản; chỉ tải phần nào có bản mới.
- **Video thiếu nhi (made for kids) nhanh hơn nhiều:** bộ giải mã JavaScript chạy thường trú và nhớ trình phát của YouTube, nên chỉ video đầu tiên sau khi YouTube đổi trình phát còn chậm; các video sau mở gần nhanh như video thường. App nhớ cách lấy luồng nào chạy được để thử trước và lấy sẵn video kế tiếp.

## Tính năng
- Giao diện kiểu app YouTube trên điện thoại, phong cách retro; tiếng Việt / English.
- Trang chủ "Dành cho bạn" theo lịch sử tìm kiếm và kênh đã xem; các mục Âm nhạc, Game retro, Chill & Lofi, Hoạt hình, Trực tiếp, Mới tải lên; danh sách không giới hạn.
- Tìm kiếm có gõ Telex (SELECT bật/tắt), chọn lại lượt tìm gần đây (L1/R1).
- Lấy luồng trực tiếp từ YouTube (khoảng 1–2 giây), tự lấy trước video đang chọn; yt-dlp chỉ dùng khi cần.
- Chất lượng Tự động (720p, 60 khung/giây nếu có), 1080p, 480p, 360p, chỉ âm thanh; đổi chất lượng không ngắt tiếng.
- Video thiếu nhi ("made for kids": hoạt hình như Doraemon của POPS Kids…) phát được: app tự chuyển sang luồng TV / nhúng của YouTube (video đầu tiên sau khi YouTube đổi trình phát chậm hơn một chút, các video sau gần nhanh như video thường).
- **Chế độ trẻ em** cho phụ huynh: PIN, Chế độ hạn chế của YouTube, giới hạn phút/ngày tự chỉnh, khung giờ, nhắc nghỉ mắt, chỉ kênh đã duyệt, từ khóa cấm, báo cáo xem.
- **Phụ đề dịch tự động (mới ở 1.5):** xem video nước ngoài sẽ tự hiện phụ đề tiếng Việt (hoặc ngôn ngữ bạn chọn) do YouTube dịch.
- Mát máy, tiết kiệm pin: CPU chỉ chạy hết tốc độ với video 60 khung/giây, 1080p hoặc khi máy giải mã không kịp; còn lại để hệ thống tự hạ xung. Màn hình chỉ vẽ lại khi có thay đổi (đúng nhịp khung hình của video, ~10 lần/giây khi đứng yên ở menu). Chất lượng hình và tiếng giữ nguyên.
- Tua gần như tức thì trong phần đã tải; tua xa nhảy tới khung hình chính gần nhất.
- Video 60 khung/giây luôn khớp tiếng (hiện tối đa ~30 hình/giây trên máy).
- Hàng "Tiếp theo" cuộn được, tự tải thêm video gợi ý; tự phát video kế.
- Nút âm lượng: hệ điều hành chỉnh loa / tai nghe, app hiện mức âm lượng; với DAC USB app tự chỉnh mức riêng.
- Tắt đen màn hình vẫn nghe (như YouTube Premium) từ Bảng nhanh (R3). Bấm nút bất kỳ (trừ âm lượng) để bật lại.
- R3 (phím nóng) bấm riêng: Bảng nhanh (Tiếp tục · Tắt màn hình · Nghe nền · Thoát).
- Nghe nền (về menu máy, tiếng vẫn phát), Xem sau, Theo dõi kênh, Tải về máy (720p) xem không cần mạng.
- USB DAC qua PipeWire của hệ thống, ở cả hai cổng USB-C (cổng sạc: bật trong Cài đặt).
- Cài đặt: ngôn ngữ, chất lượng mặc định, tự phát tiếp, lấy luồng trước, Telex, bù trễ tiếng–hình, đầu ra âm thanh, cổng sạc cho DAC, kiểu trang chủ, đổi phím, xóa bộ nhớ đệm / lịch sử / lượt tìm, phiên bản, tác giả · ủng hộ, phụ đề (chế độ, ngôn ngữ, cỡ chữ), chế độ trẻ em.


## Phím
| Màn hình | Phím |
|---|---|
| Mọi nơi | R3 (bấm riêng): Bảng nhanh · R3 + Lên/Xuống: độ sáng (dArkOS) · Âm lượng ± |
| Trang chủ / Thư viện | D-pad chọn · A xem · Y tìm kiếm · X Xem sau · L1/R1 đổi mục con · L2/R2 đổi mục chính · START làm mới · SELECT mở lại video đang phát · B thoát (đang phát thì hỏi Nghe nền / Thoát) |
| Xem video | A phát/dừng · Trái/Phải tua 10 giây · L2/R2 tua 60 giây · L1/R1 video trước/sau · X chất lượng · START tốc độ · SELECT tùy chọn · Y toàn màn hình · Xuống: hàng Tiếp theo · B về |
| Hàng Tiếp theo | Trái/Phải chọn (không tua) · L2/R2 nhảy 4 video · A xem · Lên quay lại |
| Tìm kiếm | A gõ (phím #+= mở ký hiệu, abc về chữ) · B xóa chữ / về · Y dấu cách · X xóa hết · SELECT Telex · START tìm · L1/R1 lượt tìm gần đây · L2/R2 đổi mục chính |


## Khác với bản NextUI / NX Redux
- **SELECT và START** dùng được (trên R36S chúng là phím F1 / F2 của tay cầm, bản muOS không đọc được nên bị lỗi).
- **R3 (phím nóng)** bấm riêng rồi thả: mở **Bảng nhanh** (Tiếp tục · Tắt màn hình · Nghe nền · Thoát) — R36S không có nút Home / logo.
  R3 + Lên/Xuống vẫn là chỉnh độ sáng của dArkOS (app không di chuyển khi đó).
- Màn hình 640×480 và CPU Cortex-A35: chất lượng **Tự động = 480p 30 khung/giây** (vừa khít màn hình, nhẹ máy, mát). Vẫn chọn được 720p trong menu chất lượng.
- Âm thanh qua ALSA của dArkOS; nút âm lượng do dArkOS (ogage) xử lý, app hiện mức âm lượng (đọc bằng `current_volume`); Bảng nhanh Trái/Phải chỉnh âm lượng hệ thống.
- Trong lúc phát video, app giữ cho dArkOS không tự ngủ vì không bấm phím (giống khi xem bằng ffplay/Kodi).
- Không cần thư viện cài thêm: app chỉ dùng SDL2 có sẵn của dArkOS.

## Chế độ trẻ em (phụ huynh quản lý)
Vào **Cài đặt → Chế độ trẻ em**. Lần đầu bật sẽ yêu cầu đặt **mã PIN phụ huynh 4 số** (nhập 2 lần).
Nhập số: Lên/Xuống đổi số · Trái/Phải chuyển ô · A xác nhận · X nhập lại · B hủy.

Khi Chế độ trẻ em bật:
- **Lọc nội dung:** mọi lượt tìm và trang chủ dùng **Chế độ hạn chế của YouTube**; trang chủ thành các mục cho bé
  (Cho bé · Hoạt hình · Nhạc thiếu nhi · Học tiếng Anh · Truyện cổ tích · Khoa học vui · Kênh đã duyệt).
- **Mã PIN** cần để vào Cài đặt, thoát app (nút thoát / Bảng nhanh / hộp thoại thoát) và cho xem thêm giờ.
- **Giới hạn mỗi ngày:** bạn tự chỉnh (mặc định 60 phút, Trái/Phải đổi 15 phút, tối đa 8 giờ, hoặc "Không giới hạn").
  Hết giờ: video dừng, hiện "Hết giờ xem hôm nay rồi!"; phụ huynh bấm A + PIN để cho thêm 15 phút
  (hoặc Cài đặt → Chế độ trẻ em → "Cho xem thêm 15 phút hôm nay"). Thời gian chỉ tính lúc video đang phát.
- **Khung giờ được xem:** "Được xem từ … đến …" (mặc định cả ngày). Ngoài giờ app khóa xem, PIN mở cho hôm nay.
  Cần đồng hồ máy đúng giờ (bật Wi-Fi để máy tự chỉnh giờ / chỉnh trong cài đặt máy).
- **Nhắc nghỉ mắt:** Tắt / 15 / 20 / 30 / 45 / 60 phút (mặc định 30): xem liền chừng ấy thì tạm dừng 1 phút, sau đó bấm A xem tiếp.
- **Chỉ xem kênh đã duyệt:** bật lên thì bé chỉ thấy video của các kênh phụ huynh đã duyệt (an toàn nhất).
  Thêm kênh: Kênh đã duyệt → **Y** → chọn từ các kênh đang theo dõi / đã xem / kênh của video đang phát. X bỏ duyệt.
  Mẹo: trước khi bật, mở vài video của kênh muốn cho bé xem (ví dụ POPS Kids) rồi theo dõi kênh đó để dễ chọn.
- **Từ khóa cấm:** Từ khóa cấm → **Y** → gõ từ (có Telex) → START/Thêm. Video có tiêu đề hoặc tên kênh chứa từ đó bị ẩn,
  tìm từ đó bị chặn. Không phân biệt hoa thường và dấu ("ma" chặn cả "Ma", "mà", "má").
- **Báo cáo xem của bé:** số phút xem 7 ngày gần nhất và danh sách video bé đã xem (giờ, kênh, thời lượng). X xóa danh sách.

Lưu ý: app chỉ khóa được **bên trong app** — bé vẫn có thể thoát ra menu máy bằng phím của hệ thống hoặc tắt máy.
Chế độ hạn chế của YouTube không lọc được 100%: muốn chắc chắn hãy bật "Chỉ xem kênh đã duyệt".
Quên mã PIN: xóa tệp `kids.json` trong thư mục dữ liệu của app (bằng máy tính) — các cài đặt trẻ em sẽ về mặc định.

## Phụ đề dịch tự động (mới ở 1.5)
Xem video nói tiếng nước ngoài, app tự hiện **phụ đề dịch sang ngôn ngữ của bạn** ở cuối khung hình
(ví dụ bạn dùng app tiếng Việt, xem video tiếng Anh/Nhật/Hàn… → phụ đề tiếng Việt). Phụ đề và bản dịch lấy từ YouTube
(phụ đề của kênh hoặc phụ đề tự động của YouTube, YouTube dịch máy), nên **cần mạng** và chỉ có với video YouTube có phụ đề.

**Cài đặt → Phụ đề:**
- **Tự động** (mặc định): chỉ hiện khi video nói tiếng khác ngôn ngữ của bạn. **Luôn bật**: hiện cả với video cùng ngôn ngữ. **Tắt**.
- **Ngôn ngữ phụ đề:** "Theo app" (Tiếng Việt hoặc English theo ngôn ngữ giao diện), hoặc chọn: Tiếng Việt, English, Español,
  Français, Deutsch, Português, Italiano, Bahasa Indonesia (có sẵn), **Tiếng Hàn, Tiếng Nhật, Tiếng Trung, Tiếng Thái** (cần tải gói chữ).
- **Tải gói chữ:** chọn ngôn ngữ đó rồi bấm **A** (một lần; Hàn ~10 MB, Nhật ~9 MB, Trung ~17 MB, Thái <1 MB). Phần ghi chú hiện % đang tải.
  Gói chữ (font Noto Sans của Google, giấy phép SIL OFL) được kiểm tra mã SHA-256 rồi lưu vào `ports/nxt-retro-tube/data/fonts`. Xóa thư mục đó để gỡ.
- **Cỡ chữ phụ đề:** Nhỏ / Vừa / Lớn.

**Đổi riêng cho video đang xem:** SELECT (Tùy chọn video) → **Phụ đề**, bấm A để đổi: Theo cài đặt → Bật · dịch (sang ngôn ngữ của bạn)
→ Ngôn ngữ gốc (phụ đề gốc, không dịch) → Tắt. Dòng ghi chú cho biết đang hiện gì (ví dụ "Phụ đề: Tiếng Việt (dịch từ English)") hoặc vì sao không có.

Lưu ý: video không có phụ đề (kể cả phụ đề tự động) thì không dịch được; bản dịch là dịch máy của YouTube nên có chỗ chưa mượt;
video đã tải về máy xem không cần mạng thì không có phụ đề.

## Cập nhật OTA (mới ở 2.0)
- **Cài đặt → Cập nhật (OTA)**: bấm **A** để kiểm tra. Có bản mới thì dòng ghi chú hiện số phiên bản, dung lượng và nội dung thay đổi;
  bấm **A hai lần** để tải và cài. Xong bấm **A** lần nữa để khởi động lại app với bản mới.
- **Tự kiểm tra cập nhật** (mặc định Bật): mỗi ngày một lần khi có Wi-Fi, app chỉ **báo** có bản mới, không tự cài.
- App tự nhận ra ROM và loại máy (Brick, Brick Pro, Smart Pro S, R36S…) để tải đúng gói. Chỉ phần thay đổi được tải:
  app (vài MB), yt-dlp (~30 MB, khi YouTube thay đổi) hoặc Deno.
- An toàn: bản cập nhật được tác giả **ký số**; app kiểm tra chữ ký và mã SHA-256 của từng tệp, sai là bỏ. Bản cũ được giữ lại:
  nếu bản mới không mở được 2 lần liên tiếp, app tự quay về bản cũ.
- Cần Wi-Fi và còn trống khoảng 100 MB trên thẻ nhớ khi cập nhật yt-dlp / Deno.

## Nghe nền
Đang phát video, bấm B ở trang chủ → "Nghe nền (chỉ tiếng)" (hoặc Home → Nghe nền): app tắt hình, về EmulationStation, tiếng vẫn phát và tự chuyển video tiếp.
Mở lại app để quay về đúng chỗ đang nghe.

## Tắt màn hình vẫn nghe
Bấm R3 → Bảng nhanh → **Tắt màn hình · vẫn nghe**: màn hình tắt, tiếng vẫn phát. Bấm nút bất kỳ (trừ âm lượng) để bật lại.
Nút nguồn vẫn là của dArkOS (cho máy ngủ — khi đó tiếng dừng).

## Đổi phím
Cài đặt → Đổi phím: chọn chức năng, bấm A rồi bấm nút muốn gán (hai chức năng sẽ đổi chỗ cho nhau).
Y = đổi chỗ A/B nhanh, X = về mặc định. D-pad, R3 (phím nóng) và nút âm lượng không đổi được.

## Tác giả & ủng hộ
NXT Retro Tube do **Nguyễn Xuân Thông** làm. Nếu thấy app hữu ích, bạn có thể ủng hộ tác giả:
- MoMo: **0339838267**
- Ngân hàng Vietcombank (VCB): **0581000786219**
- PayPal: **thongkhung2@gmail.com**

Trong app: Cài đặt → **Tác giả · Ủng hộ**.

## Khi có lỗi
Gửi lại `NXT_Tube_Log.txt` (trong `ports/nxt-retro-tube`), `data/cache/nxt-retro-tube-run.txt` và `data/cache/ytdlp-helper-0.txt`.

Ứng dụng không liên kết với YouTube/Google. Giấy phép: thư mục `licenses`.
