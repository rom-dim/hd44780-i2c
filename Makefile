ifneq ($(KERNELRELEASE),)
	obj-m := hd44780.o
        hd44780-y := hd44780-i2c.o hd44780-dev.o

else
    PWD := $(shell pwd)

all: ; $(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

modules_install: ; $(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules_install

default: ; $(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

clean: ; $(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean

endif

