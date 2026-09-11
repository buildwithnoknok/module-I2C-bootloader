# noknok stage-0 — standalone build (deliberately does NOT link ch32fun.c)
#
# ch32fun.mk always compiles ch32fun.c, which brings a 38-entry interrupt vector
# table and a general-purpose reset handler (248 B measured) that stage-0 cannot
# use — it never enables an interrupt. Stage-0 is frozen at manufacture, so we
# link exactly what we need and nothing else. ch32fun.h is still used, for its
# register definitions only (header-only, no code).
#
#   make build   -> noknok_stage0.bin  (must fit 1 KB — see stage0.ld)
#   make flash   -> SWD-flash via WCH-LinkE + minichlink
#   make size    -> report the size against the frozen budget

TARGET   = noknok_stage0
BUDGET   = 1024
CH32FUN  = ../ch32fun
MINICHLINK = ../minichlink/minichlink

PREFIX   = riscv64-unknown-elf
CC       = $(PREFIX)-gcc
OBJCOPY  = $(PREFIX)-objcopy
OBJDUMP  = $(PREFIX)-objdump

CFLAGS = -g -Os -flto \
         -ffunction-sections -fdata-sections -fmessage-length=0 \
         -msmall-data-limit=8 -fno-tree-loop-distribute-patterns \
         -march=rv32ec -mabi=ilp32e -DCH32V003 \
         -static-libgcc -nostdlib -Wall \
         -I. -I$(CH32FUN) -I$(CH32FUN)/../extralibs -I/usr/include/newlib

LDFLAGS = -T stage0.ld -Wl,--gc-sections \
          -Wl,--print-memory-usage -Wl,-Map=$(TARGET).map \
          -L$(CH32FUN)/../misc -lgcc

SRCS = start.S $(TARGET).c

all   : build
build : $(TARGET).bin

$(TARGET).elf : $(SRCS) stage0.ld
	$(CC) -o $@ $(SRCS) $(CFLAGS) $(LDFLAGS)

$(TARGET).bin : $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	$(OBJDUMP) -S $< > $(TARGET).lst
	@$(MAKE) --no-print-directory size

size :
	@sz=`stat -c %s $(TARGET).bin`; \
	 free=`expr $(BUDGET) - $$sz`; \
	 echo "stage-0: $$sz B of $(BUDGET) B budget ($$free B free)"

flash : $(TARGET).bin
	$(MINICHLINK) -w $(TARGET).bin flash -b

clean :
	rm -f $(TARGET).elf $(TARGET).bin $(TARGET).lst $(TARGET).map

.PHONY : all build size flash clean
