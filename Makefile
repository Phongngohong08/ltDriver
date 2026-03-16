KDIR    ?= /lib/modules/$(shell uname -r)/build
LTCRYPT  = driver/ltcrypt
USBMOUSE = driver/usbmouse
APP      = app

.PHONY: all drivers app clean load unload mknod

all: drivers app

drivers:
	$(MAKE) -C $(LTCRYPT)  M=
	$(MAKE) -C $(USBMOUSE) M=

app:
	$(MAKE) -C $(APP)

clean:
	$(MAKE) -C $(LTCRYPT)  M= clean
	$(MAKE) -C $(USBMOUSE) M= clean
	$(MAKE) -C $(APP)      clean

# Load / unload crypto driver
load:
	@echo "Loading ltcrypt..."
	sudo modprobe des_generic 2>/dev/null || true
	sudo insmod $(LTCRYPT)/ltcrypt.ko
	@sleep 0.2
	@ls /dev/ltcrypt && echo "Device created OK" || echo "/dev/ltcrypt not found — try: make mknod"
	@dmesg | tail -3

unload:
	@echo "Unloading drivers..."
	-sudo rmmod ltusbmouse 2>/dev/null
	-sudo rmmod ltcrypt

# Create device node manually if udev didn't do it
mknod:
	@MAJOR=$$(awk '$$2=="ltcrypt"{print $$1}' /proc/devices); \
	 if [ -z "$$MAJOR" ]; then \
	   echo "ltcrypt not found in /proc/devices. Is the module loaded?"; exit 1; \
	 fi; \
	 sudo mknod /dev/ltcrypt c $$MAJOR 0; \
	 sudo chmod 666 /dev/ltcrypt; \
	 echo "Created /dev/ltcrypt (major=$$MAJOR)"
