# Build the guide's example applications against a Lagom build of LibGUI
# produced by libgui-wayland's scripts/build-libgui.sh.
#
#   make                 build every example into ./bin
#   make hello-world     build one example
#   make clean
#
# Override LAGOM or SERENITY if your build tree lives elsewhere:
#
#   make LAGOM=/opt/libgui-wayland/build/lagom

LAGOM ?= $(abspath $(CURDIR)/../build/lagom)
SERENITY ?= $(abspath $(CURDIR)/../serenity)

CXX ?= c++
CXXFLAGS ?= -std=c++26 -fno-exceptions -O2 -g -Wall -Wextra -Wno-literal-suffix
CPPFLAGS += \
	-I$(SERENITY) \
	-I$(SERENITY)/Userland \
	-I$(SERENITY)/Userland/Libraries \
	-I$(SERENITY)/Userland/Services \
	-I$(LAGOM) \
	-I$(LAGOM)/Userland/Libraries \
	-I$(LAGOM)/Userland/Services \
	-I$(CURDIR)/../compat

# The Wayland support library installs the toolkit's platform hooks from a
# static initializer, so it must be linked with --no-as-needed. Its filename
# (libwindowserver.so) is historical; despite the name, it is an in-process
# library, not a system service. The rpath keeps the examples runnable from the
# build tree without LD_LIBRARY_PATH.
LDFLAGS += -L$(LAGOM)/lib -Wl,--no-as-needed -Wl,-rpath,$(LAGOM)/lib
LDLIBS += \
	-llagom-gui \
	-llagom-gfx \
	-llagom-core \
	-llagom-corebasic \
	-llagom-ipc \
	-llagom-main \
	-llagom-ak \
	-lwindowserver

EXAMPLES := hello-world greeter notes clock
BIN := bin

.PHONY: all clean
all: $(addprefix $(BIN)/,$(EXAMPLES))

$(BIN):
	mkdir -p $@

$(BIN)/%: %/main.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
	rm -rf $(BIN)
