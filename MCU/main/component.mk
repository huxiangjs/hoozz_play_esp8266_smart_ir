#
# "main" pseudo-component makefile.
#
# Only the sources this project actually uses are compiled. They live in the
# common submodules, so the source dirs are listed in COMPONENT_SRCDIRS (which
# generates the compile rules) and the exact object files are picked with
# COMPONENT_OBJS. The whole common dirs can't be used, because they contain
# modules this project doesn't use and a duplicate wifi.c in both of them.

COMPONENT_SRCDIRS := . esp32-common esp32-common/tiny-AES-c esp8266-common

COMPONENT_OBJS := \
	main.o \
	esp32-common/crypto.o \
	esp32-common/tiny-AES-c/aes.o \
	esp32-common/event_bus.o \
	esp32-common/keyboard.o \
	esp32-common/simple_ctrl.o \
	esp32-common/store.o \
	esp8266-common/gpio_led.o \
	esp8266-common/ir_ctrl.o \
	esp8266-common/wifi.o

COMPONENT_ADD_INCLUDEDIRS := . esp32-common/ esp8266-common/

# COMPONENT_OBJS := $(patsubst %.c,%.o,$(COMPONENT_SRCS))

# COMPONENT_ADD_LDFLAGS +=

# CFLAGS +=
