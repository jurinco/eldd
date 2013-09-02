KDIR ?= /lib/modules/`uname -r`/build

default:
	$(MAKE) -C $(KDIR) M=$$PWD

read_rtc: read_rtc.c
	gcc -o read_rtc read_rtc.c
