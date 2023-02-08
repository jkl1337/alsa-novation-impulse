KDIR ?= /lib/modules/`uname -r`/build
MDIR = kernel/sound/usb

default:
	$(MAKE) -C $(KDIR) M=$$PWD

clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean


install:
	$(MAKE) INSTALL_MOD_PATH=$(DESTDIR) INSTALL_MOD_DIR=$(MDIR) \
		-C $(KDIR) M=$$PWD modules_install
	depmod -a
