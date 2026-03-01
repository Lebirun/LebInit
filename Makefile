CC = i686-elf-gcc
STRIP = i686-elf-strip

V ?= 0
ifeq ($(V),0)
  Q = @
  MSG_CC    = @printf '  CC      %s\n' $<;
  MSG_LD    = @printf '  LD      %s\n' $@;
  MSG_STRIP = @printf '  STRIP   %s\n' $@;
else
  Q =
  MSG_CC =
  MSG_LD =
  MSG_STRIP =
endif

LIBC = ../../libc
LIBC_ABS = $(abspath $(LIBC))

CFLAGS = -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -m32 -Os -fomit-frame-pointer -nostdinc -ffunction-sections -fdata-sections
CPPFLAGS = -isystem $(LIBC_ABS)/leblibc/include -isystem $(LIBC_ABS)/leblibc/build-i386/obj/include -isystem $(LIBC_ABS)/leblibc/arch/i386 -isystem $(LIBC_ABS)/leblibc/arch/generic -I$(LIBC_ABS)/include -I$(LIBC_ABS)/src

CRT1 = $(LIBC)/leblibc/build-i386/lib/crt1.o
CRTI = $(LIBC)/leblibc/build-i386/lib/crti.o
CRTN = $(LIBC)/leblibc/build-i386/lib/crtn.o
LIBC_A = $(LIBC)/leblibc/build-i386/lib/libc.a

LD_SCRIPT = $(LIBC)/user.ld

SYSROOT_BIN = ../../root/bin
SYSROOT_SBIN = ../../root/sbin

SRCS = src/main.c src/log.c src/service.c
OBJS = $(SRCS:.c=.o)

LEBINIT_SRCS = src/lebinit/main.c
LEBINIT_OBJS = $(LEBINIT_SRCS:.c=.o)

.PHONY: all clean stage

all: init.bin lebinit.bin

%.o: %.c
	$(MSG_CC)$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

init.bin: $(OBJS) $(CRT1) $(CRTI) $(CRTN) $(LIBC_A)
	$(MSG_LD)$(CC) -nostdlib -static -Wl,-z,noexecstack -Wl,--gc-sections -T $(LD_SCRIPT) -L$(LIBC)/leblibc/build-i386/lib -o $@ $(CRT1) $(CRTI) $(OBJS) -lc $(CRTN) -lgcc

lebinit.bin: $(LEBINIT_OBJS) $(CRT1) $(CRTI) $(CRTN) $(LIBC_A)
	$(MSG_LD)$(CC) -nostdlib -static -Wl,-z,noexecstack -Wl,--gc-sections -T $(LD_SCRIPT) -L$(LIBC)/leblibc/build-i386/lib -o $@ $(CRT1) $(CRTI) $(LEBINIT_OBJS) -lc $(CRTN) -lgcc

stage: all
	$(Q)mkdir -p $(SYSROOT_BIN)
	$(Q)cp init.bin $(SYSROOT_BIN)/init
	$(MSG_STRIP)$(STRIP) -s $(SYSROOT_BIN)/init
	$(Q)mkdir -p $(SYSROOT_SBIN)
	$(Q)cp lebinit.bin $(SYSROOT_SBIN)/lebinit
	$(MSG_STRIP)$(STRIP) -s $(SYSROOT_SBIN)/lebinit

clean:
	rm -f $(OBJS) $(LEBINIT_OBJS) init.bin lebinit.bin
