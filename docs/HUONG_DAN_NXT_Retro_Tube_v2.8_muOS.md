# NXT Retro Tube 2.8 — muOS (TrimUI Brick / Brick Pro)

## Yêu cầu
- muOS bản có hỗ trợ TrimUI Brick Pro (`tui-brick-pro`, ví dụ 2606 "Andromeda" trở lên). TrimUI Brick thường (`tui-brick`) cũng dùng được.
- Đã đối chiếu với mã nguồn muOS (MustardOS internal + frontend, 10/2026): cấu hình máy `tui-brick-pro`, dịch vụ phím `muinput`, PipeWire, cách chạy ứng dụng (`mux_launch.sh`), cờ nguồn/ngủ. Chưa chạy thử trên máy thật.

## Cài đặt
Chép thư mục `MUOS/application/NXT Retro Tube` vào `MUOS/application/` trên thẻ nhớ đang chứa thư mục `MUOS` (thẻ 1, hoặc thẻ 2 nếu bạn để dữ liệu muOS ở thẻ 2).
Mở từ **Applications** (Ứng dụng) → **NXT Retro Tube**.

Cần Wi-Fi. Dữ liệu (lịch sử, xem sau, kênh, video đã tải) nằm trong `MUOS/application/NXT Retro Tube/data`.

## Mới ở 2.8
- **Khung hình** (SELECT → Khung hình): **Mặc định** (đủ hình) hoặc **Lấp đầy màn hình** (phóng to cho kín màn, cắt bớt mép). Máy nhớ lựa chọn.
- **Zoom như trên điện thoại**: trong toàn màn hình bấm **Xuống**, rồi **R1** phóng to / **L1** thu nhỏ (×1 đến ×3),
  **D-pad** di chuyển để chọn chỗ xem, **X** đặt lại, **B** xong. Góc màn có bản đồ nhỏ cho biết đang xem phần nào.
- Video **4:3** không còn viền đen bốn phía khi xem toàn màn hình; video 4:3 bị chèn sọc đen sẵn thì chọn Lấp đầy là hết sọc.
- **Lưu ý cập nhật:** bản 2.8 đổi khoá ký cập nhật. Máy đang chạy 2.7 hoặc cũ hơn **cài tay 2.8 một lần** (chép đè như cài mới,
  dữ liệu vẫn giữ); từ 2.8 trở đi cập nhật OTA lại tự chạy.

## Mới ở 2.7
- **Danh sách phát của kênh**: trong trang kênh bấm **L1/R1** để đổi giữa **Video** và **Danh sách phát** (gom theo bộ).
  Mở một bộ: các tập xếp theo thứ tự, phát hết tập tự sang tập sau. Bộ nào kênh xếp tập mới lên đầu thì máy tự đảo lại
  để xem từ tập 1; **Y** đổi thứ tự, **X** lưu cả bộ vào Thư viện.
- **Tab Đăng ký có hàng kênh** ở trên (như YouTube trên web): từ hàng video đầu bấm **Lên**, **Trái/Phải** chọn kênh, **A** mở kênh.
- **Ảnh đại diện thật của kênh** thay cho chữ viết tắt (kênh đăng ký và kênh gặp trong kết quả tìm kiếm).
- **Bấm B khi đang xem** thì về đúng danh sách đã mở video (bộ, kênh, tìm kiếm, thư viện), không về thẳng trang chủ.
- **Chọn nút cạnh trình phát**: đang xem (chưa toàn màn hình) bấm **Lên** để sang Theo dõi · Xem sau · Tải về · Chất lượng · Tốc độ, **A** để bấm.
- **Hướng dẫn phím** ngay trong máy: Cài đặt → Hướng dẫn phím (8 phần, kể cả các thao tác bấm 2 lần, 3 lần).
- Thư viện chứa được **30 danh sách phát** (trước là 7); đứng ở một danh sách bấm **START hai lần** để xóa.

## Mới ở 2.6
- **Đăng ký kênh đồng bộ hai chiều** như nút Thích: kênh đăng ký trên điện thoại / máy tính hiện "đã đăng ký" trên máy và có trong tab Kênh;
  hủy đăng ký ở đâu thì nơi kia cũng theo. Khi mở app, các kênh bạn đăng ký trên tài khoản Google tự được thêm vào tab **Kênh**.
- "Báo video mới của kênh" nhẹ máy hơn khi có nhiều kênh: mỗi đợt kiểm tra 40 kênh, đợt sau kiểm tra tiếp.
- **Gửi lịch sử xem về tài khoản** (Cài đặt → Tài khoản & kênh): máy gửi theo cách của app YouTube trên TV, nhưng hiện YouTube
  chỉ thỉnh thoảng mới ghi vào lịch sử, nên video xem trên máy **có thể không hiện** trong lịch sử trên điện thoại / máy tính.
  Chiều ngược lại chạy ổn định: video xem dở trên điện thoại / máy tính hiện ở tab **Xem tiếp** trên máy.

## Mới ở 2.5
Bản 2.5 đưa toàn bộ tính năng của bản thử 2.4.1 lên cả 7 bản (NX Redux, NextUI, Stock, Stock Smart Pro S, muOS, Knulli, dArkOS R36S).

**Tính năng mới (từ bản thử 2.4.1)**
- **Điều khiển bằng điện thoại**: máy đóng vai một chiếc TV cho app YouTube trên điện thoại. Cài đặt → Tài khoản & kênh →
  Điều khiển bằng điện thoại → A lấy mã 12 số → nhập mã trên điện thoại:
  **Android**: app YouTube › ảnh đại diện › Cài đặt › Xem trên TV › Liên kết bằng mã TV.
  **iPhone / iPad**: bấm nút truyền (cast, hình màn hình có sóng) ở đầu app › Liên kết bằng mã TV (Add a device).
  Nếu iPhone không hiện nút truyền: Cài đặt iPhone › Quyền riêng tư & Bảo mật › Mạng cục bộ › bật cho YouTube.
  Sau đó bấm nút truyền trong video, chọn "NXT Retro Tube": video phát trên máy, điện thoại
  điều khiển phát / dừng / tua / bài tiếp. Không cần đăng nhập trên máy.
- **Thích / đăng ký vào tài khoản**: SELECT → Thích video này; Theo dõi kênh = đăng ký luôn trên tài khoản Google.
  Video đã thích trên điện thoại / máy tính: dòng "Thích" hiện sẵn "Bỏ thích", và Thư viện có tab **Đã thích (Google)**.
- **YouTube Music**: ở kết quả tìm bấm SELECT → Tìm trong: Bài hát (YouTube Music). SELECT → **Mix nhạc từ bài này** để có
  danh sách phát liên tục; ở chế độ nghe nhạc, hết danh sách thì app tự nối Mix.
- **Bộ chỉnh âm (EQ)**: Cài đặt → Âm thanh: Trầm mạnh, Giọng hát, Loa nhỏ của máy, Tai nghe, Rock / EDM.
- **Tắt màn hình, nghe tiếp** ngay trong SELECT (vẫn dùng được nút nguồn / bảng nhanh như trước).

**Cập nhật**: bản 2.8 cần cài tay một lần (đổi khoá ký); từ 2.8 trở đi máy tự nhận bản mới qua OTA (Cài đặt → Cập nhật & giới thiệu → Cập nhật).

## Mới ở 2.4
**Tài khoản Google & xem tiếp**
- **Đăng nhập Google** (Cài đặt → Tài khoản & kênh → Tài khoản Google): máy hiện mã + mã QR, trên điện thoại mở google.com/device, nhập mã, bấm Cho phép.
  "Dành cho bạn" là trang đề xuất của chính bạn, tab **"Đăng ký"** là video mới của kênh bạn đăng ký. Nên dùng tài khoản phụ (đây là cách đăng nhập của app YouTube trên TV).
- Tab **"Xem tiếp"**: video xem dở trên điện thoại / web / TV (khi đăng nhập) hoặc trên máy này; mở là phát tiếp đúng chỗ.
- "Dành cho bạn" / "Đăng ký" cuộn không dừng: hết trang của tài khoản thì nối tiếp gợi ý của app.

**Cài đặt gọn thành 9 mục**: Tài khoản & kênh · Phát video · Âm thanh · Phụ đề · Giao diện · Tải về & bộ nhớ · Pin, phím & hệ thống · Chế độ trẻ em · Cập nhật & giới thiệu.
A mở mục, B quay lại, L1/R1 sang mục bên cạnh.

**Giao diện**
- 6 bộ màu (Retro cam, YouTube đỏ, Đại dương, Rừng xanh, Neon hồng, Đen tuyền).
- Tùy biến: ẩn / đổi thứ tự tab trang chủ, thêm tối đa 4 tab theo từ khóa, ẩn từng biểu tượng trên thanh trạng thái.
- Nút gợi ý có màu (A đỏ, B vàng, X xanh dương, Y xanh lá).
- Tên video tiếng **Hàn, Nhật, Trung, Thái** hiện đúng chữ (tự tải phông một lần khi gặp, cần Wi-Fi).

**Tải về & file** (khi xem: SELECT → Tải về)
- Video 720p (xem trong app) · Video + file MP4 · nhạc M4A · Opus · MP3 128 kbps.
- File có tên dễ đọc ở thư mục **`NXT Tube`** gốc thẻ nhớ, mở được bằng máy tính / app khác. "Xuất video & nhạc đã tải" chép các bản đã tải trước đó.

**Khi xem video** (SELECT → Tùy chọn video)
- Bỏ qua đoạn tài trợ trong video (SponsorBlock: tự bỏ qua / chỉ báo / tắt), đoạn đánh dấu màu xanh lá trên thanh tua.
- Chương: vạch chia trên thanh tua, bảng Chương để nhảy; ảnh nhỏ xem trước khi tua (gọn cho màn nhỏ).
- Lặp 1 video / cả danh sách / ngẫu nhiên, lặp đoạn A–B.
- Âm lượng tới 250% có chống vỡ tiếng, Giọng nói rõ; khóa phím khi bỏ túi (SELECT ×3 để mở).
- Chế độ nghe nhạc: tắt hình, màn hình đĩa nhạc — mát máy, pin lâu. Tiết kiệm pin tự động khi pin dưới 20%.

**Danh sách phát, kênh, tìm kiếm**
- "Yêu thích" + tối đa 7 danh sách phát của bạn (Thư viện).
- Báo video mới của kênh theo dõi (2 giờ một lần khi có Wi-Fi); tùy chọn tự tải video mới khi đang sạc.
- Bộ lọc tìm kiếm (SELECT ở kết quả): thời gian tải lên, thời lượng, sắp xếp, chỉ trực tiếp.
- Nhập kênh từ Google Takeout (`subscriptions.csv` vào thư mục `NXT Tube`).

**Cập nhật**: máy đang chạy bản 2.1 trở lên tự nhận bản 2.4 qua OTA (bản cũ: Cài đặt → Cập nhật; từ 2.4: Cài đặt → Cập nhật & giới thiệu → Cập nhật).

## Mới ở 2.3
- **Giao diện full màn hình trên máy màn 16:9** (Smart Pro / Smart Pro S 1280×720, các máy 16:9 trên muOS / Knulli): trước đây giao diện
  4:3 bị viền đen hai bên; giờ trải hết bề ngang — Trang chủ / Thư viện 4 cột video, danh sách tìm kiếm, Cài đặt, màn xem video
  (5 video "Tiếp theo"), bảng nhanh… Máy màn 4:3 (Brick, R36S…) giữ nguyên như cũ.
- **Video không mở được vì YouTube từ chối đường dẫn (lỗi 403):** app không chờ thử lại lâu nữa mà tự lấy đường dẫn mới qua yt-dlp
  rồi phát lại (mỗi video một lần).

## Mới ở 2.2.1
- Chỉ thay đổi cho Smart Pro S (máy khác giữ nguyên như 2.2).

## Mới ở 2.2
- **Thanh trạng thái** ở góc trên (Trang chủ, Kênh / Thư viện, Cài đặt, Chế độ trẻ em, và khi xem toàn màn hình lúc hiện thanh điều khiển):
  **giờ**, **% pin** với biểu tượng pin (xanh lá khi đang cắm sạc, có tia sét; vàng khi còn ≤ 30%, đỏ khi ≤ 15%),
  **Wi-Fi** (số vạch theo độ mạnh sóng), **Bluetooth** khi có thiết bị đang kết nối, và **DAC** khi cắm DAC USB. Mục nào không có kết nối thì ẩn.
  Máy chưa chỉnh giờ (đồng hồ chưa đặt) thì không hiện giờ.
- **Tab Bóng đá** mới ở Trang chủ (highlights bóng đá). Hàng tab giờ **tự cuộn** theo tab đang chọn, nên các tab sau "Trực tiếp" (Bóng đá, Mới tải lên) hiện đầy đủ tên;
  có mũi tên mờ ở mép cho biết còn tab phía trước / sau. Chế độ trẻ em có thêm tab "Vận động cho bé".

## Mới ở 2.1.3
- Chỉ thay đổi cho Smart Pro S (máy khác giữ nguyên như 2.1.2).

## Mới ở 2.1.2
- **Sửa lỗi 2.1.1 không mở được live** ("Không mở được luồng"): YouTube để **tiếng của live ở một danh sách riêng**, nên mở riêng danh sách hình thì không có tiếng.
  Giờ app nhận ra kiểu này và mở cả hình lẫn tiếng, vẫn **chỉ tải đúng chất lượng đã chọn** (không còn 240p). Nếu một cách mở bị lỗi, app tự thử lại bằng danh sách đầy đủ.
  Nghe nền / Chỉ âm thanh với live: chỉ tải phần tiếng.

## Mới ở 2.1.1
- **Sửa lỗi xem live chỉ được 240p:** trước đây app đưa cả danh sách chất lượng của luồng trực tiếp cho bộ phát, bộ phát tự chọn mức thấp (thường 240p).
  Giờ app đọc danh sách đó trước và mở **đúng chất lượng đang chọn** (Tự động = 720p, 60 hình nếu live có; chọn 1080p / 720p / 480p / 360p cũng được áp dụng cho live).
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
- Nút Home bên hông: một lần bấm chỉ tính một lần (không còn mở rồi tắt ngay). Khi bảng Task Manager / OSD của hệ thống đang mở, app không nhận phím nữa.

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
- Nút nguồn: tắt đen màn hình, vẫn nghe (như YouTube Premium). Bấm nút bất kỳ (trừ âm lượng) để bật lại.
- Nút Home bên hông (Brick Pro): Bảng nhanh (Tiếp tục · Tắt màn hình · Nghe nền · Thoát). Nút MENU (logo) bấm riêng rồi thả: thoát.
- Nghe nền (về menu máy, tiếng vẫn phát), Xem sau, Theo dõi kênh, Tải về máy (720p) xem không cần mạng.
- USB DAC qua PipeWire của hệ thống, ở cả hai cổng USB-C (cổng sạc: bật trong Cài đặt).
- Cài đặt: ngôn ngữ, chất lượng mặc định, tự phát tiếp, lấy luồng trước, Telex, bù trễ tiếng–hình, đầu ra âm thanh, cổng sạc cho DAC, kiểu trang chủ, đổi phím, xóa bộ nhớ đệm / lịch sử / lượt tìm, phiên bản, tác giả · ủng hộ, phụ đề (chế độ, ngôn ngữ, cỡ chữ), chế độ trẻ em.


## Phím
| Màn hình | Phím |
|---|---|
| Mọi nơi | MENU (bấm riêng): thoát · MENU + Âm lượng: độ sáng (của hệ thống) · Home bên hông: Bảng nhanh · Nguồn: tắt/bật màn hình · Âm lượng ± |
| Trang chủ / Thư viện | D-pad chọn · A xem · Y tìm kiếm · X Xem sau · L1/R1 đổi mục con · L2/R2 đổi mục chính · START làm mới · SELECT mở lại video đang phát · B thoát (đang phát thì hỏi Nghe nền / Thoát) |
| Xem video | A phát/dừng · Trái/Phải tua 10 giây · L2/R2 tua 60 giây · L1/R1 video trước/sau · X chất lượng · START tốc độ · SELECT tùy chọn · Y toàn màn hình · Xuống: hàng Tiếp theo · B về |
| Hàng Tiếp theo | Trái/Phải chọn (không tua) · L2/R2 nhảy 4 video · A xem · Lên quay lại |
| Tìm kiếm | A gõ (phím #+= mở ký hiệu, abc về chữ) · B xóa chữ / về · Y dấu cách · X xóa hết · SELECT Telex · START tìm · L1/R1 lượt tìm gần đây · L2/R2 đổi mục chính |


## Khác với bản NextUI / NX Redux
- Âm thanh đi qua PipeWire của muOS; nút âm lượng do muOS xử lý, app chỉ hiện mức (Bảng nhanh: Trái/Phải chỉnh âm lượng hệ thống).
- Phím đọc theo mã phím (evdev) của thiết bị ảo `muOS-Keys`, nên đúng phím dù muOS đánh số nút khác TrimUI. Không chiếm phím của hệ thống.
- MENU là phím tắt hệ thống của muOS: app chỉ coi là "thoát" khi bấm MENU một mình rồi thả.
- Không cần SDL2_image hay thư viện cài thêm: app chỉ dùng SDL2 có sẵn của muOS.

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
  Gói chữ (font Noto Sans của Google, giấy phép SIL OFL) được kiểm tra mã SHA-256 rồi lưu vào `MUOS/application/NXT Retro Tube/data/fonts`. Xóa thư mục đó để gỡ.
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
Đang phát video, bấm B ở trang chủ → "Nghe nền (chỉ tiếng)" (hoặc Home → Nghe nền): app tắt hình, về menu muOS, tiếng vẫn phát và tự chuyển video tiếp.
Mở lại app để quay về đúng chỗ đang nghe.

## Tắt màn hình vẫn nghe
Bấm nút nguồn (nhấn nhanh) khi đang ở trong app: màn hình tắt đen, tiếng vẫn phát. Bấm nút bất kỳ (trừ âm lượng) để bật lại.
Trong lúc app mở, app báo cho muOS không ngủ khi nhấn nhanh nút nguồn (cờ `caffeine`) và tạm dừng tự tối/tự ngủ khi không bấm phím (`idle_game_inhibit`). Thoát app là trả lại như cũ.
Khi đang Nghe nền ở menu muOS, nút nguồn hoạt động bình thường của muOS (ngủ máy thì tiếng dừng).

## Đổi phím
Cài đặt → Đổi phím: chọn chức năng, bấm A rồi bấm nút muốn gán (hai chức năng sẽ đổi chỗ cho nhau).
Y = đổi chỗ A/B nhanh, X = về mặc định. D-pad, MENU, Home và nút âm lượng không đổi được.

## Tác giả & ủng hộ
NXT Retro Tube do **Nguyễn Xuân Thông** làm. Nếu thấy app hữu ích, bạn có thể ủng hộ tác giả:
- MoMo: **0339838267**
- Ngân hàng Vietcombank (VCB): **0581000786219**
- PayPal: **thongkhung2@gmail.com**

Trong app: Cài đặt → **Tác giả · Ủng hộ**.

## Khi có lỗi
Gửi lại `NXT_Tube_Log.txt` (trong thư mục app), `data/cache/nxt-retro-tube-run.txt` và `data/cache/ytdlp-helper-0.txt`.

Ứng dụng không liên kết với YouTube/Google. Giấy phép: thư mục `licenses`.
