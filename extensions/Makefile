CC ?= gcc
CONTRIB_SO := sample.so erase_sample.so userstack.so elfheader.so

all: $(CONTRIB_SO)

CFLAGS += -fPIC -shared -Wl,-T,../makedumpfile.ld

$(CONTRIB_SO): %.so: %.c
	$(CC) $(CFLAGS) -o $@ $^

userstack.so: vma_rbtree.c vma_mtree.c

clean:
	rm -f $(CONTRIB_SO)

