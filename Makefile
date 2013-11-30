SOURCES = plugin.c m3u8.c gsthlsdemux.c gsturidownloader.c
PACKAGES = gstreamer-1.0 gstreamer-base-1.0 openssl

CFLAGS = -g -O2 -std=gnu99 $(shell pkg-config --cflags $(PACKAGES))
LIBS = $(shell pkg-config --libs $(PACKAGES))

libgsthls.so:
	$(CC) -shared -o $@ $(CFLAGS) $(LDFLAGS) -fPIC $(SOURCES) $(LIBS)

$(prefix)/lib/gstreamer-1.0/libgsthls.so: libgsthls.so
	install -m644 -D $< $@

prefix = /usr

install: $(prefix)/lib/gstreamer-1.0/libgsthls.so
