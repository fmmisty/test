CFLAGS  ?= -O2 -g -Wall -Wextra
CFLAGS  += -I../../protocol $(shell pkg-config --cflags libusb-1.0)
LDLIBS  += $(shell pkg-config --libs libusb-1.0) -lpthread

all: usblan_tap usblan_ota

usblan_tap: usblan_tap.c ../../protocol/usblan_proto.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

usblan_ota: usblan_ota.c ../../protocol/usblan_proto.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test_parser: test_parser.c ../../protocol/usblan_proto.h
	$(CC) $(CFLAGS) -o $@ $<

test: test_parser
	./test_parser

install-udev:
	install -m 0644 99-usblan.rules /etc/udev/rules.d/
	udevadm control --reload-rules

clean:
	rm -f usblan_tap usblan_ota test_parser

.PHONY: all test install-udev clean
