WIN_CC = $(shell which x86_64-w64-mingw32-gcc 2>/dev/null || echo /tmp/llvm-mingw-20240619-ucrt-ubuntu-20.04-x86_64/bin/x86_64-w64-mingw32-gcc)
CFLAGS = -O2 -Wall -Wextra -std=c11
LDLIBS = -lwinmm -lole32 -luser32 -ladvapi32 -lshell32
SOURCES = restless_virtuoso.c dsp.c ../analysis/cumulative_flux.c
HEADERS = ../analysis/cumulative_flux.h asio_abi.h dsp.h vst_abi.h dr_flac.h dr_mp3.h stb_vorbis.c ../analysis/dr_wav.h

restless_virtuoso.exe: $(SOURCES) $(HEADERS)
	$(WIN_CC) $(CFLAGS) $(SOURCES) -o $@ $(LDLIBS)

padded: restless_virtuoso_padded.exe

restless_virtuoso_padded.exe: $(SOURCES) $(HEADERS)
	$(WIN_CC) $(CFLAGS) -DPAD_STEMS $(SOURCES) -o $@ $(LDLIBS)

clean:
	rm -f restless_virtuoso.exe restless_virtuoso_padded.exe

.PHONY: clean padded
