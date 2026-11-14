# Hoozz Play ESP8266 Smart IR
Intelligent learning IR, and can be controlled through the network

[![ESP8266 CI](https://github.com/huxiangjs/hoozz_play_esp8266_smart_ir/actions/workflows/esp8266.yml/badge.svg)](https://github.com/huxiangjs/hoozz_play_esp8266_smart_ir/actions/workflows/esp8266.yml)

# ESP8266_RTOS_SDK Version
```shell
$ git branch
* release/v3.3

$ git log --oneline | head -n 3
2bc42667 Merge branch 'bugfix/constrain_cryptography_v3.3' into 'release/v3.3'
4cab385e Tools: Constrain the cryptography package for avoiding breaking changes
6c438bce Merge branch 'bugfix/queue_arith_overflow_v3.3' into 'release/v3.3'

Toolchain version: crosstool-ng-1.22.0-100-ge567ec7b
Compiler version: 5.2.0
```

## Pull source code
```shell
git clone https://github.com/huxiangjs/hoozz_play_esp8266_smart_ir.git --recurse-submodules
```

## Build and Flash

* first step: open the sdkconfig file, modify `CONFIG_ESPTOOLPY_PORT` to your serial.

```shell
cd hoozz_play_esp8266_smart_ir/MCU
make -j$(nproc)
make flash
make monitor    # Monitor UART log output
```

## You can also directly burn the released bin file

1. Download the latest release zip package from the Releases column
2. Unzip the zip and you will see the following files:
   ```
   $ tree
   .
   |-- bootloader
   |   `-- bootloader.bin
   |-- ota_data_initial.bin
   |-- ota_file
   |-- partitions.bin
   `-- smart_ir.bin

   1 directory, 5 files
   ```
   (`ota_file` only names the app bin in the zip, it is not flashed)
3. Open the latest official download tool `flash_download_tool` (official download address: [other-tools](https://www.espressif.com/en/support/download/other-tools))
4. Use the following options: (1) `Chip Type [ESP8266]`; (2) `WorkMode [Develop]`; (3) `LoadMode [UART]`;
5. Set the burn parameters according to the following table:

   |    Parameters  |     Value   |
   | :------------: | :---------: |
   | bootloader.bin       | 0x0000      |
   | ota_data_initial.bin | 0x10000     |
   | smart_ir.bin         | 0x12000     |
   | partitions.bin       | 0x8000      |
   | SPI SPEED            | 40MHz       |
   | SPI MODE             | DIO         |
   | DoNotChgBin          | √           |
   | COM                  | (Your port) |
   | BAUD                 | 115200      |

6. Finally click **Start** to burn

## OTA Firmware Update

The firmware supports over-the-air updates from the Hoozz Play app. The
current firmware version (the git commit of this repo) is reported to the
app and printed to the serial log at startup. The update is downloaded
over the encrypted tunnel and written to the second OTA partition; if the
new firmware fails to boot, the device rolls back to the previous one.

To publish a new firmware, push a version tag (e.g. `v1.0`); the CI build
uploads the release zip to GitHub Releases, which the app then uses to
update the devices.

