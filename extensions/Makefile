CC ?= gcc
CONTRIB_SO := sample.so erase_sample.so

all: $(CONTRIB_SO)

CFLAGS += -fPIC -shared -Wl,-T,../makedumpfile.ld

$(CONTRIB_SO): %.so: %.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(CONTRIB_SO)

