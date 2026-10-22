# RTDSP-F4 firmware: STM32F405 (Cortex-M4F), FreeRTOS V11, freestanding (no newlib).
#
#   make TARGET=qemu   -> build/fw/qemu/rtdsp.elf  (QEMU netduinoplus2)
#   make TARGET=hw     -> build/fw/hw/rtdsp.{elf,bin}  (real board, HSE 8 MHz)
#   make V=1           -> show full compiler command lines

TARGET   ?= qemu
ROOT     := ..
OUT      := $(ROOT)/build/fw/$(TARGET)
RTOS     := $(ROOT)/third_party/FreeRTOS-Kernel

PREFIX   ?= arm-none-eabi-
CC       := $(PREFIX)gcc
OBJCOPY  := $(PREFIX)objcopy
SIZE     := $(PREFIX)size

ifeq ($(TARGET),qemu)
  TARGET_DEFS := -DBSP_TARGET_QEMU=1
else ifeq ($(TARGET),hw)
  TARGET_DEFS := -DBSP_TARGET_QEMU=0 -DAPP_DEBUG_CMDS=0
else
  $(error TARGET must be qemu or hw)
endif

ARCH := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

# -ffp-contract=off: no silent a*b+c -> FMA fusion, so float32 results are
#  bit-identical to the host SIL build and the Python model (see fixedpoint.py).
# -fno-tree-loop-distribute-patterns: stop GCC turning mini_libc's loops into
#  calls to memcpy/memset (i.e. into themselves).
CFLAGS := $(ARCH) -std=gnu11 -O2 -g3 -ffunction-sections -fdata-sections -ffreestanding \
          -fno-common -fno-builtin -fno-tree-loop-distribute-patterns -ffp-contract=off \
          -fstack-usage -Wall -Wextra -Wshadow -Wdouble-promotion -Wundef -Werror \
          $(TARGET_DEFS)

INCLUDES := -Ilibc/include -Irtos -Ibsp/include -Iapp/include -Iapp/generated \
            -I$(ROOT)/libs/dsp/include -I$(ROOT)/libs/link/include \
            -I$(RTOS)/include -I$(RTOS)/portable/GCC/ARM_CM4F

APP_SRCS  := $(wildcard app/src/*.c) app/generated/fw_tables.c
BSP_SRCS  := $(wildcard bsp/src/*.c) startup/startup_stm32f405.c
LIBC_SRCS := $(wildcard libc/src/*.c)
LIB_SRCS  := $(wildcard $(ROOT)/libs/dsp/src/*.c) $(wildcard $(ROOT)/libs/link/src/*.c)
RTOS_SRCS := $(RTOS)/tasks.c $(RTOS)/queue.c $(RTOS)/list.c $(RTOS)/stream_buffer.c \
             $(RTOS)/event_groups.c $(RTOS)/portable/GCC/ARM_CM4F/port.c rtos/rtos_hooks.c

SRCS := $(APP_SRCS) $(BSP_SRCS) $(LIBC_SRCS) $(LIB_SRCS) $(RTOS_SRCS)
OBJS := $(foreach s,$(SRCS),$(OUT)/obj/$(subst ../,,$(s:.c=.o)))

# Third-party code is built with its own (upstream-clean) warning set.
$(OUT)/obj/third_party/%.o: CFLAGS := $(filter-out -Wshadow -Wundef -Werror,$(CFLAGS))

LDFLAGS := $(ARCH) -T linker/stm32f405rg.ld -nostdlib -Wl,--gc-sections \
           -Wl,-Map=$(OUT)/rtdsp.map -Wl,--print-memory-usage -Wl,--no-warn-rwx-segments

ELF := $(OUT)/rtdsp.elf
V ?= 0
ifeq ($(V),0)
  Q := @
else
  Q :=
endif

.PHONY: all clean
all: $(ELF) $(OUT)/rtdsp.bin

$(OUT)/obj/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(OUT)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(ELF): $(OBJS) linker/stm32f405rg.ld
	@echo "  LD  $@"
	$(Q)$(CC) $(LDFLAGS) $(OBJS) -lgcc -o $@
	$(Q)$(SIZE) $@

$(OUT)/rtdsp.bin: $(ELF)
	$(Q)$(OBJCOPY) -O binary $< $@

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)
