# tests/ Makefile
# Create various builds for testing the libraries

# We [and end-users] have to link against libunicorn, libpthread, and openssl.

CFLAGS		:= -g -I../src/ -lunicorn -lpthread -lcrypto
LIBSTFU_A	:= ../src/libstfu.a

# Apparently the ordering of source files seems to matter here.
# libstfu.a has to go *after* the test file (?) in `gcc` arguments?
# Just write test binaries in the project root for now.

all: test-boot0 test-boot1 stfu stfui
test-boot0:
	gcc $(CFLAGS) test_boot0.c $(LIBSTFU_A) -o ../$@
test-boot1:
	gcc $(CFLAGS) test_boot1.c $(LIBSTFU_A) -o ../$@
stfu:
	gcc $(CFLAGS) stfu.c $(LIBSTFU_A) -o ../$@
stfui:
	gcc $(CFLAGS) -lncurses stfui.c $(LIBSTFU_A) -o ../$@
clean:
	rm -fv ../test-boot0 ../test-boot1 ../stfu ../stfui

.PHONY: all clean
