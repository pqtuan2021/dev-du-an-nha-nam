import json
import time
import random
from datetime import datetime
import paho.mqtt.client as mqtt

# =========================
# MQTT CONFIG
# =========================
MQTT_HOST = "mqtt.fuvitech.vn"
MQTT_PORT = 1883
MQTT_KEEPALIVE = 60

# 2 thiết bị cảm biến
DEVICES = [
    {
        "id_device": "SEN79001",
        "name": "Cảm biến khu A"
    },
    {
        "id_device": "SEN79002",
        "name": "Cảm biến khu B"
    }
]


def make_sensor_payload(device_id: str):
    """
    Tạo dữ liệu cảm biến mẫu.
    Mỗi thiết bị có:
    temperature, humidity, co2, light, voltage, current, frequency, power
    """

    voltage = round(random.uniform(218, 222), 1)
    current = round(random.uniform(0.5, 2.0), 2)
    power = round(voltage * current, 1)

    return {
        "type": "SENSOR_DATA",
        "id_device": device_id,
        "device_id": device_id,

        "temperature": round(random.uniform(25, 38), 1),
        "humidity": round(random.uniform(55, 85), 1),
        "co2": random.randint(450, 1200),
        "light": random.randint(300, 1000),

        "voltage": voltage,
        "current": current,
        "frequency": 50,
        "power": power,

        "timestamp": datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    }


def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("✅ Connected MQTT broker")
    else:
        print(f"❌ Connect failed, rc={rc}")


def publish_device_data(client):
    for device in DEVICES:
        device_id = device["id_device"]

        topic = f"maydokhongkhi/{device_id}/data"
        payload = make_sensor_payload(device_id)

        client.publish(topic, json.dumps(payload), qos=0, retain=False)

        print("📤 Publish:")
        print("Topic:", topic)
        print(json.dumps(payload, indent=2, ensure_ascii=False))
        print("-" * 50)


def main():
    client = mqtt.Client(client_id="python_test_2_sensor_devices")
    client.on_connect = on_connect

    client.connect(MQTT_HOST, MQTT_PORT, MQTT_KEEPALIVE)
    client.loop_start()

    try:
        while True:
            publish_device_data(client)
            time.sleep(5)  # gửi mỗi 5 giây

    except KeyboardInterrupt:
        print("🛑 Stop publish")

    finally:
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()