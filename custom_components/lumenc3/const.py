"""Constants for the LumenC3 integration."""

DOMAIN = "lumenc3"
CONF_PORT = "port"  # serial port path; set for USB entries
CONF_HOST = "host"  # IP / hostname; set for Wi-Fi entries
CONF_TCP_PORT = "tcp_port"

# USB ID of every ESP32-C3/S3/C6 native USB port (also listed in manifest.json)
ESP_USB_VID = 0x303A
ESP_USB_PID = 0x1001

SETUP_TIMEOUT = 15  # s to wait for the first STATE before retrying setup later
