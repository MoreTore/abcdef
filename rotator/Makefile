# ---------------------------------------------------------------------------
# Minimal build script for rot_test.c               (Qualcomm SDM845 rotator)
#
#  • Native build      :  make
#  • Cross-compile ARM :  make CROSS=aarch64-linux-gnu- KDIR=~/sdm845-kernel
#
# The only assumption is that the UAPI header
#   media/msm_sde_rotator.h
# lives somewhere under $(KDIR)/include or $(KDIR)/drivers/…
# ---------------------------------------------------------------------------

# ----------- toolchain ------------------------------------------------------
CROSS      ?=           # e.g. aarch64-linux-gnu-
CC         := $(CROSS)gcc
AR         := $(CROSS)ar

# ----------- kernel headers -------------------------------------------------
# Default to the headers that match the build host; override for cross builds
KDIR       ?= /usr/src/linux-headers-$(shell uname -r)

# In practice you may need an extra -I to wherever Qualcomm drops
# msm_sde_rotator.h in your tree – add it here if the build complains.
INCLUDES   := -I$(KDIR)/include/uapi \
              -I$(KDIR)/include
			  

# ----------- build flags ----------------------------------------------------
CFLAGS     ?= -Wall -Wextra -O2
LDFLAGS    ?=

# ----------- targets --------------------------------------------------------
.PHONY: all clean
all: rot_test rot_test2

OBJS = rot_test.o ion_helpers.o
rot_test: $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@
rot_test2: $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

rot_test.o: rot_test.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@


clean:
	rm -f rot_test rot_test.o
