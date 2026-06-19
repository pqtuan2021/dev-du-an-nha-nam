# KIẾN TRÚC ĐÚNG V10 – KHU VỰC → TỦ ĐIỀU KHIỂN → THIẾT BỊ CẢM BIẾN

## 1. Hiểu đúng bài toán

Mục **"Tổng hệ sinh thái sẽ có các thiết bị"** là để mô tả toàn bộ hệ thống cho đội phát triển hiểu, **không phải** để đưa thành các ô/thẻ riêng trên giao diện khách hàng.

Hệ thống được hiểu như sau:

- **Tủ điều khiển**: STM32 dùng ESP32 làm cầu MQTT/UART. Hai phần này được xem là **một thiết bị chính**, dùng **chung một mã tủ**.
- **Thiết bị đa cảm biến Fuviair**: chỉ thu thập dữ liệu cảm biến và publish MQTT `maydokhongkhi/ID/data`.
- **Web UI**: giao diện quản trị/vận hành.
- **HMI UI UART**: giao diện tại chỗ, sẽ bổ sung sau nhưng hiện tại vẫn phải dành sẵn phần cấu hình và đồng bộ.

---

## 2. Mô hình quản lý đúng trên web

Giao diện phải quản lý theo cấp:

```text
Khu vực
  └── Tủ điều khiển
        └── Nhiều thiết bị đa cảm biến
```

### Ví dụ

- **Xưởng A**
  - Tủ A01
    - SEN_A_01
    - SEN_A_02
    - SEN_A_03

- **Xưởng B**
  - Tủ B01
    - SEN_B_01
    - SEN_B_02
  - Tủ B02
    - SEN_B_03
    - SEN_B_04
    - SEN_B_05

---

## 3. Quy tắc định danh

### 3.1 Tủ điều khiển

ESP32 cầu và STM32 dùng **chung mã tủ** để web và Node-RED quản lý thống nhất.

Ví dụ:

- `control_id = TU_A01`
- ESP32 gateway dùng `TU_A01`
- STM32 cũng gắn với `TU_A01`

> `stm_id` vẫn có thể tồn tại nội bộ để debug, nhưng khi quản lý trên web và đồng bộ cấu hình thì lấy `control_id`/`gateway_id` làm chính.

### 3.2 Thiết bị đa cảm biến

Mỗi thiết bị cảm biến có mã riêng, ví dụ:

- `SEN_A_01`
- `SEN_A_02`
- `SEN_A_03`

Thiết bị cảm biến publish MQTT:

```text
maydokhongkhi/SEN_A_01/data
maydokhongkhi/SEN_A_02/data
```

---

## 4. Cách dùng trên giao diện

### 4.1 Tổng quan

- Có **Danh sách khu vực**
- Chọn **khu vực** → hiện danh sách **tủ điều khiển** trong khu vực đó
- Chọn **tủ** → mới xem được:
  - trạng thái tủ
  - dữ liệu cảm biến thuộc tủ
  - điện năng sử dụng
  - trạng thái relay/quạt

**Bỏ khối Gateway ESP32** khỏi Tổng quan.

### 4.2 Phòng / thiết bị

Đổi thành logic:

- **Khu vực**
- **Tủ điều khiển**
- **Thiết bị cảm biến thuộc tủ**

Luồng đăng ký:

1. Thêm **khu vực**
2. Thêm **tủ điều khiển** vào khu vực
3. Thêm **thiết bị cảm biến**
4. Gán thiết bị cảm biến vào tủ tương ứng

### 4.3 Điều khiển STM32

- Có **Danh sách khu vực**
- Chọn khu vực → chọn tủ
- Sau đó mới thao tác cho tủ đó:
  - Thủ công
  - Lịch trình
  - Theo cảm biến
  - Quạt làm mát
  - Đồng bộ cấu hình

### 4.4 Kỹ thuật

Gộp chung:

- Lệnh hệ thống
- Lịch sử
- Log

Màn này yêu cầu đăng nhập kỹ thuật:

- Tài khoản: `dev_fuvitech`
- Mật khẩu: `fuvitech2026`

---

## 5. Thuật ngữ tiếng Việt phải dùng

- Khu vực
- Tủ điều khiển
- Thiết bị cảm biến
- Dữ liệu cảm biến
- Chế độ thủ công
- Chế độ lịch trình
- Chế độ theo cảm biến
- Quạt làm mát tủ
- Lưu cấu hình
- Đồng bộ cấu hình
- Nhật ký kỹ thuật

Không hiển thị các chú thích kỹ thuật khó hiểu kiểu:

- “Gửi format compact hoặc schedule vào ESP, ESP tự chuẩn hóa”
- “ABOVE dùng onAbove/offBelow”

Thay bằng hướng dẫn dễ hiểu cho khách hàng.

---

## 6. Luồng dữ liệu đúng

### 6.1 Điều khiển thủ công

```text
Web UI
→ Node-RED
→ MQTT đến mã tủ điều khiển
→ ESP32 cầu
→ UART
→ STM32
→ Bật/tắt relay
→ ACK/STATE trả về UI
→ Node-RED lưu trạng thái cuối
```

### 6.2 Cài đặt lịch trình / theo cảm biến

```text
Web UI
→ Node-RED lưu DB
→ Node-RED gửi ngay xuống tủ điều khiển
→ ESP32 cầu
→ UART
→ STM32 lưu RAM/Flash
→ ACK OK
→ UI báo đã lưu
```

### 6.3 Dữ liệu từ thiết bị đa cảm biến

```text
Thiết bị Fuviair
→ MQTT: maydokhongkhi/ID/data
→ ESP32 cầu nhận theo danh sách thiết bị đã gán cho tủ
→ UART xuống STM32
→ STM32 tự tính rule / lịch / relay
→ Gửi trạng thái về Node-RED/UI
```

### 6.4 Khi tủ khởi động lại

```text
ESP32 cầu boot
→ hỏi danh sách thiết bị của tủ
→ hỏi cấu hình của tủ
→ Node-RED đọc DB
→ trả danh sách thiết bị + lịch + rule
→ ESP32 gửi xuống STM32
→ STM32 khôi phục cấu hình
```

---

## 7. Cấu trúc dữ liệu mới

### 7.1 Bảng chính

- `areas` – khu vực
- `control_units` – tủ điều khiển
- `sensor_devices` – thiết bị cảm biến
- `control_unit_devices` – gán thiết bị vào tủ
- `sensor_data` – dữ liệu cảm biến
- `relay_states` – trạng thái cuối relay
- `relay_logs` – lịch sử relay
- `sensor_rules` – rule theo cảm biến
- `timer_rules` – lịch trình

### 7.2 Quan hệ

```text
areas 1 --- n control_units
control_units 1 --- n control_unit_devices
sensor_devices 1 --- n control_unit_devices
```

---

## 8. Topic MQTT chuẩn V10

### Tủ điều khiển

```text
maydokhongkhi/TU_A01/gateway/status
maydokhongkhi/TU_A01/gateway/devices/request
maydokhongkhi/TU_A01/gateway/devices/response
maydokhongkhi/TU_A01/stm32/cmd
maydokhongkhi/TU_A01/stm32/ack
maydokhongkhi/TU_A01/stm32/status
maydokhongkhi/TU_A01/stm32/relay/state
maydokhongkhi/TU_A01/stm32/config/sync/request
maydokhongkhi/TU_A01/stm32/config/sync/response
```

### Thiết bị cảm biến

```text
maydokhongkhi/SEN_A_01/data
maydokhongkhi/SEN_A_02/data
```

---

## 9. Chế độ vận hành trên web

Khi người dùng vào **Xưởng A** → chọn **Tủ A01**, tủ đó sẽ chạy một trong ba chế độ:

- **Thủ công**
- **Lịch trình**
- **Theo cảm biến**

Rule luôn gắn với **tủ đang chọn** và **thiết bị cảm biến cụ thể**.

Ví dụ:

- Khu vực: Xưởng A
- Tủ: Tủ A01
- Thiết bị cảm biến: `SEN_A_01`
- Điều kiện: Nhiệt độ > 30 thì bật Relay 1, < 25 thì tắt

---

## 10. Phần cần sửa trong code

### ESP32 cầu

- Dùng `CONTROL_ID` làm mã chính của tủ
- Sub danh sách thiết bị theo `control_unit_devices`
- Khi nhận sensor data từ các thiết bị thuộc tủ, forward xuống STM32
- Khi boot, gửi `DEVICE_LIST_REQUEST` và `CONFIG_SYNC_REQUEST`

### STM32

- ACK/status phải trả theo `control_id`
- Manual relay không chờ DB
- Rule/timer lưu nội bộ
- Sync lại khi boot từ Node-RED

### Web UI

- Màn hình theo cấp `Khu vực → Tủ → Thiết bị`
- Không đưa “tổng hệ sinh thái” thành các ô hiển thị
- Giao diện dùng toàn bộ nhãn tiếng Việt

---

## 11. Kết luận chốt

Bản đúng phải là:

```text
Khu vực
→ nhiều tủ điều khiển
→ mỗi tủ gắn nhiều thiết bị đa cảm biến
→ người dùng chọn khu vực, chọn tủ, rồi mới xem và điều khiển tủ đó
```

Trong hệ thống:

- **ESP32 cầu + STM32 = một tủ điều khiển**
- **Thiết bị Fuviair = nguồn dữ liệu cảm biến**
- **Web UI = nơi cấu hình và vận hành**
- **HMI UART = sẽ thêm sau nhưng đã chừa cơ chế đồng bộ**
