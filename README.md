# Hoozz Play ESP8266 Smart IR
Intelligent learning IR, and can be controlled through the network

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
# make monitor
```

