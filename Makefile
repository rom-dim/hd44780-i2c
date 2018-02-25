ifneq ($(KERNELRELEASE),)
	obj-m := hd44780.o
        hd44780-y := hd44780-i2c.o hd44780-dev.o hd44780-sysfs.o

else
    PWD := $(shell pwd)

default: ; $(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

clean: ; $(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean

endif

