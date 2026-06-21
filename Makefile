OUT := $(HOME)/rk3506_linux6.1_sdk_v1.2.0/buildroot/output/rockchip_hd_rk3506g_evm_nand
OVERLAY := $(HOME)/rk3506_linux6.1_sdk_v1.2.0/buildroot/board/vanxoak/hd_rk3506g_evm_nand/fs-overlay
SYSROOT := $(OUT)/host/arm-buildroot-linux-gnueabihf/sysroot
CC  := $(OUT)/host/bin/arm-buildroot-linux-gnueabihf-gcc

CFLAGS := --sysroot=$(SYSROOT) -O2 \
	-I$(SYSROOT)/usr/include/lvgl \
	-I$(SYSROOT)/usr/include/lvgl/lv_drivers \
	-I$(SYSROOT)/usr/include/libdrm \
	-I$(SYSROOT)/usr/include/rkadk \
	-I$(SYSROOT)/usr/include/rockchip
DEFINES := -DLV_CONF_INCLUDE_SIMPLE -DUSE_EVDEV=1 -DUSE_RKADK=1

LIBS := -llvgl -llv_drivers -lrkadk -lrockit -levdev -ldrm -lrga -lfreetype \
	-lavcodec -lavformat -lavutil -lswscale -lswresample \
	-lasound -lpthread -lm

SRCS := $(filter-out lvgl/%, $(wildcard *.c))
OBJS := $(SRCS:.c=.o)

all: install

play: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

install: play
	cp play $(OVERLAY)/usr/bin/play

%.o: %.c
	$(CC) $(CFLAGS) $(DEFINES) -c -o $@ $<

push: all
	adb push play /usr/bin/ && adb shell chmod +x /usr/bin/play

run: push
	adb shell "killall play 2>/dev/null; killall -9 ffmpeg 2>/dev/null; play"

clean:
	rm -f play *.o
