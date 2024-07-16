VERSION=2.0

DEBUG= -g
CFLAGS+=-DVERSION=\"${VERSION}\" $(DEBUG)
LDFLAGS+=$(DEBUG) -lgpiod -lm -lrt

OBJS=error.o main.o

all: rpi_gpio_ntp

rpi_gpio_ntp: $(OBJS)
	$(CC) -Wall -W $(OBJS) $(LDFLAGS) -o rpi_gpio_ntp

install: rpi_gpio_ntp
	cp rpi_gpio_ntp /usr/local/bin

clean:
	rm -f $(OBJS) rpi_gpio_ntp

package: clean
	mkdir rpi_gpio_ntp-$(VERSION)
	cp *.c *.h Makefile readme.txt license.txt rpi_gpio_ntp-$(VERSION)
	tar czf rpi_gpio_ntp-$(VERSION).tgz rpi_gpio_ntp-$(VERSION)
	rm -rf rpi_gpio_ntp-$(VERSION)
