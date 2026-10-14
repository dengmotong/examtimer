# ExamTimer (考核计时录屏器) - build for Windows XP SP3 and newer
#
#   make            cross compile with mingw-w64 (32 bit)
#   make CC=gcc     native build (for host side unit tests)
#   make clean

# prefix of the cross toolchain; "make CROSS=" builds natively
CROSS   ?= i686-w64-mingw32-
CC       = $(CROSS)gcc
STRIP    = $(CROSS)strip
WINDRES  = $(CROSS)windres

TARGET   = ExamTimer.exe
SRCDIR   = src
BUILDDIR = build

# _WIN32_WINNT=0x0501 -> Windows XP.  Only XP-era APIs are used, so the
# resulting binary runs on XP SP3, Vista, 7, 8, 10 and 11.
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu99 \
          -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 \
          -fno-strict-aliasing -finput-charset=UTF-8 \
          -I$(SRCDIR)
LDFLAGS = -mwindows -static -static-libgcc -Wl,--nxcompat -Wl,--dynamicbase
LIBS    = -lwinmm -lgdi32 -luser32 -lshell32 -lcomdlg32 -ladvapi32 -lole32 -lcomctl32

OBJS = $(BUILDDIR)/main.o \
       $(BUILDDIR)/util.o \
       $(BUILDDIR)/overlay.o \
       $(BUILDDIR)/recorder.o \
       $(BUILDDIR)/audio.o \
       $(BUILDDIR)/jpeg_enc.o \
       $(BUILDDIR)/avi.o \
       $(BUILDDIR)/version.o

HEADERS = $(SRCDIR)/examtimer.h $(SRCDIR)/avi.h $(SRCDIR)/jpeg_enc.h

all: $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# every object depends on every header: the Config layout is shared, and a
# stale object file silently reads the wrong struct offsets.
$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(HEADERS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# version information shown in the Windows file properties dialog
$(BUILDDIR)/version.o: $(SRCDIR)/version.rc | $(BUILDDIR)
	$(WINDRES) -c 65001 -i $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	@echo "built $@"

strip: $(TARGET)
	$(STRIP) --strip-unneeded $(TARGET)

clean:
	rm -rf $(BUILDDIR) $(TARGET)

.PHONY: all clean strip
