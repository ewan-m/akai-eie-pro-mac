BUNDLE_NAME := AkaiEiePro.driver
EXECUTABLE_NAME := AkaiEieProHAL
BUNDLE_DIR := build/$(BUNDLE_NAME)
CONTENTS_DIR := $(BUNDLE_DIR)/Contents
MACOS_DIR := $(CONTENTS_DIR)/MacOS
EXECUTABLE := $(MACOS_DIR)/$(EXECUTABLE_NAME)

SDKROOT ?= /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
ARCH ?= arm64
CXX := /usr/bin/clang++

SRC := AkaiEieProHAL.cpp AkaiEieUsbTransport.cpp \
	../protocol/akai_eie_protocol.cpp

CXXFLAGS := -std=c++17 -arch $(ARCH) -isysroot $(SDKROOT) \
	-mmacosx-version-min=12.0 -fvisibility=hidden -fPIC \
	-I.. -Wall -Wextra -Wpedantic
LDFLAGS := -bundle -arch $(ARCH) -isysroot $(SDKROOT) \
	-mmacosx-version-min=12.0 \
	-Wl,-exported_symbol,_AkaiEieProFactory \
	-framework CoreAudio -framework CoreFoundation -framework IOKit

.PHONY: all bundle sign clean install

all: bundle

bundle: $(EXECUTABLE) $(CONTENTS_DIR)/Info.plist

$(EXECUTABLE): $(SRC) | $(MACOS_DIR)
	$(CXX) $(CXXFLAGS) $(SRC) $(LDFLAGS) -o $@

$(CONTENTS_DIR)/Info.plist: Info.plist | $(CONTENTS_DIR)
	cp Info.plist $@

$(MACOS_DIR):
	mkdir -p $@

$(CONTENTS_DIR):
	mkdir -p $@

sign: bundle
	codesign --force --sign - --timestamp=none $(BUNDLE_DIR)

install: sign
	./install.sh

clean:
	rm -rf build
