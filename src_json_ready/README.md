# uibuilder src - Nông Trại Dashboard

Copy toàn bộ 3 file chính vào thư mục `src` của instance uibuilder đang dùng trong Node-RED.

Với flow hiện tại, instance uibuilder có URL là `maigia`, nên thường sẽ là một trong các đường dẫn sau trên server:

- `/data/uibuilder/maigia/src`
- hoặc đúng đường dẫn instance mà Node-RED đang hiển thị ở node uibuilder.

Các topic UI gửi về Node-RED:

- `login_request`
- `register_request`
- `room_list_request`
- `room_create_request`
- `room_delete_request`
- `history_request`
- `mode_request`
- `relay_request`
- `timer_request`
- `sensor_rule_request`
- `fan_request`
- `fan_threshold_request`
- `status_request`
- `full_state_request`
- `save_flash_request`
- `sync_request`
- `device_list_cmd_request`
- `stm32_cmd_request`

Các topic này đã match với flow JSON đã sửa để đẩy lệnh qua MQTT topic:

`maydokhongkhi/GW01/stm32/cmd`

Lưu ý: nếu trình duyệt báo chưa có uibuilder client, kiểm tra lại dòng script trong `index.html`:

```html
<script src="../uibuilder/uibuilder.iife.min.js"></script>
```

Với một số bản uibuilder cũ, có thể cần đổi sang đường dẫn client tương ứng của instance.


## Bổ sung bản usage_stats

Bản này thêm trang **Điện & thống kê**:
- Tổng điện năng kWh theo ngày/tháng/năm.
- Giá trị cảm biến và điện: temperature, humidity, co2, light, voltage, current, frequency, power.
- Ngày được tính từ 00:00:00 đến 23:59:59 theo giờ VN.

Cần import kèm flow `flows_usage_stats.json` để Node-RED xử lý topic `usage_request` và trả `usage_response`.
