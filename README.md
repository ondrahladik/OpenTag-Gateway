# OpenTag Gateway

Gateway firmware for OpenTag electronic shelf labels. The Gateway consists of two ESP32 boards: the ESP32-C3 provides Wi-Fi connectivity, API communication, and the local web interface, while the ESP32-C6 communicates with the tags over IEEE 802.15.4.

The Gateway forwards content prepared by the server to the tags. The tag itself renders the image.

## Features

- Discovers tags on the radio network and forwards their status to the server, including battery level, RSSI, and display information.
- Regularly communicates with the OpenTag API and forwards image jobs to the ESP32-C6.
- Provides a local web interface for configuring Wi-Fi, the API token, and a separate update password.
- Exposes synchronization, tag, and event status through a local API.
- Supports uploading tag OTA firmware and queuing it for delivery at the tag's next check-in.
- Stores the API token in persistent ESP32-C3 memory, so it does not need to be entered again after a restart.
- Supports OTA updates for ESP32-C3 and ESP32-C6 firmware through the web interface.

## Architecture

| Component | Role |
| --- | --- |
| `esp32c3/` | Wi-Fi, HTTPS communication with the API, web configuration, and forwarding data to C6 over UART. |
| `esp32c6/` | Radio communication with tags, discovered tag list, image queue, and OTA firmware. |
| ESP32-C3 ↔ ESP32-C6 | UART, 115200 baud, 8N1. |
| Tags | Communicate with C6 over IEEE 802.15.4. The default radio channel is 25. |

C3 and C6 firmware are built and uploaded separately.

## Repository contents

- `esp32c3/` - Wi-Fi Gateway firmware and local web interface.
- `esp32c6/` - radio Gateway firmware and tag communication.
- `LICENSE` - project license.

Tag firmware and its bootloader are maintained in the separate [OpenTag-nRF52811](https://github.com/ondrahladik/OpenTag-nRF52811) repository.

## Connecting the ESP32-C3 and ESP32-C6

Cross-connect the UART lines, connecting each board's TX to the other board's RX:

| ESP32-C3 | ESP32-C6 | Function |
| --- | --- | --- |
| GPIO 21 (TX) | GPIO 10 (RX) | Data from C3 to C6 |
| GPIO 20 (RX) | GPIO 11 (TX) | Data from C6 to C3 |
| 5V | 5V | Power for the other board |
| GND | GND | Common ground |

Connect 5V/VBUS between the boards so that one powered board supplies 5 V to the other. Use only one power source, and do not connect USB or another 5 V source to both boards at the same time, as this could cause backfeeding. Check the pin labels and 5 V capabilities of the specific boards before connecting them.

UART uses **115200 baud, 8N1**. The boards must share a ground and use compatible 3.3 V logic levels. Do not connect 5 V to the UART pins.

## Building and uploading

Use the Arduino IDE and the ESP32 board package from Espressif Systems. Open and build each part separately in the Arduino IDE:

1. `esp32c3/esp32c3.ino` for the ESP32-C3 board.
2. `esp32c6/esp32c6.ino` for the ESP32-C6 board.

Install the following libraries:

- **ArduinoJson** - used by both C3 and C6 firmware.
- **WiFiManager** by tzapu - used by C3 firmware for Wi-Fi configuration.

Select the corresponding ESP32-C3 or ESP32-C6 board for each part and upload its firmware. To view C3 diagnostic output over native USB, enable **USB CDC On Boot** in the board settings. Application logs from C3 are not displayed when this option is disabled.

## First startup

1. Connect C3 to C6 as shown in the table above.
2. If C3 has no saved Wi-Fi credentials, it starts the **OpenTag-Gateway** configuration access point. Connect to it and select your Wi-Fi network.
3. After C3 connects to Wi-Fi, find its IP address in the serial output or in your router.
4. Open `http://<C3-IP-address>/` in a browser and set the API token. The token is stored in C3 memory and is not shown in the interface. To change it, enter a new token. Leave the field blank to keep the current token unchanged. The page does not provide a way to remove the token.
5. You can also set an update password (8-128 characters) in the same form. Enter the API token when setting the password for the first time. For later password changes, an active login at `/update` can be used instead. The password is stored in NVS as a salted PBKDF2-HMAC-SHA-256 hash, so the API token does not need to be entered for every upload.
6. C3 connects to the API at `https://opentag.cz/api` and starts regularly synchronizing tag status and image jobs.

Changing the API token does not require the previous token. Therefore, make the configuration page available only on a trusted local network.

The local web interface provides a token configuration page and an update page:

- `http://<C3-IP-address>/` - configure the API token and update password.
- `http://<C3-IP-address>/update` - after signing in with the update password, update tags or ESP32-C3/C6 firmware.

The default C6 radio channel is **25**, and the default tag check-in interval is **5 minutes**. The server can change the interval later.

An `/update` login lasts one hour. After five failed attempts, access is blocked for one minute. The server checks the session on every upload, including tag OTA uploads. The web interface uses HTTP without TLS, so use it only on a trusted local network.

## Updating tag firmware over OTA

1. Use the `tag_fw/Newton_M3_Universal-ota.bin` file.
2. Open `http://<C3-IP-address>/update`, select the target tag, and choose the OTA BIN file.
3. Upload the firmware. The Gateway forwards it to C6 and queues it for the selected tag.
4. The tag downloads the update at its next check-in.

For OTA, use the **application OTA BIN**, not a `*-full-flash.bin` file or a HEX file intended for direct programming over SWD. C6 verifies the target tag, file size, checksum, and basic structure of the nRF52811 firmware.

## Updating Gateway firmware over OTA

Sign in with the update password at `http://<C3-IP-address>/update`. After signing in, forms are available for ESP32-C3, ESP32-C6, and tag updates.

The repository contains prepared firmware files in the `firmware/` directory. Select the corresponding OTA file on the `/update` page:

- **ESP32-C3:** `firmware/esp32c3-v1.0.0-ota.bin` is written to the inactive OTA partition, and C3 restarts when the update is complete.
- **ESP32-C6:** C3 forwards `firmware/esp32c6-v1.0.0-ota.bin` in blocks over UART. C6 verifies the checksum, writes the OTA partition, and restarts.

The `*-full.bin` files are for complete flashing over USB, do not use them for OTA. Before updating, the board must have a partition table with OTA metadata and OTA partitions large enough for the application. If the board has no OTA partitions or the firmware does not fit, the upload will fail. Perform the initial installation or change the partition table over USB.

To update C6, C3 and C6 must be connected and powered as shown in the UART table above. Do not interrupt power or the serial connection during the C6 update. C3 restarts after its update, so if the browser loses its connection, wait a few seconds and reload the page.

## Troubleshooting

- **C3 does not connect to Wi-Fi:** connect to the `OpenTag-Gateway` access point and configure the network again.
- **The web interface is unavailable:** check C3's IP address and make sure the phone or computer is on the same network.
- **Tags are not visible:** check the crossed TX/RX connections, common ground, 115200 baud UART speed, and radio channel 25.
- **API synchronization fails:** check the Wi-Fi connection, API token, and service status at `https://opentag.cz/api`.
- **C3 application logs are missing from the serial monitor:** check **USB CDC On Boot** and the 115200 baud rate.
- **A tag OTA update fails:** check that you are using `*-ota.bin`, the selected tag has appeared in the list, and C3 can communicate with C6.
- **Cannot sign in to `/update`:** make sure an update password is set on the home page. After five failed attempts, wait one minute.
- **C3/C6 OTA update fails:** check the update password login, the correct application `.bin`, OTA support, and partition table capacity. For C6, also check the UART connection.

## License

This project is distributed under the [CC BY-NC-SA 4.0](LICENSE) license. Attribution and terms of use are provided in the license file.
