SDKROOT ?= $(shell xcrun --sdk driverkit --show-sdk-path 2>/dev/null)
MACOSX_SDKROOT ?= $(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null)
CXX ?= $(shell xcrun --sdk driverkit --find clang++ 2>/dev/null)
SWIFTC ?= $(shell xcrun --sdk macosx --find swiftc 2>/dev/null)
IIG ?= $(shell xcrun --find iig 2>/dev/null)
ARCH ?= $(shell uname -m)
DEPLOYMENT_TARGET ?= 25.0
MACOS_DEPLOYMENT_TARGET ?= 12.0
TRIPLE ?= $(ARCH)-apple-driverkit$(DEPLOYMENT_TARGET)
MACOS_TRIPLE ?= $(ARCH)-apple-macosx$(MACOS_DEPLOYMENT_TARGET)

FRAMEWORK_NAME := AkaiEieDriver
SOURCE_DIR := Sources/AkaiEieDriver
GEN_DIR := /private/tmp/akai-eie-driverkit-iig
GEN_FRAMEWORK_DIR := $(GEN_DIR)/$(FRAMEWORK_NAME)
GEN_BIN_DIR := $(GEN_DIR)/bin
SWIFT_MODULE_CACHE := /private/tmp/akai-eie-swift-module-cache
BUILD_DIR := build/local
DEXT_BUNDLE_ID := com.interslice.akai-eie-pro.driver
APP_BUNDLE_ID := com.interslice.akai-eie-pro.activator
DEXT_EXECUTABLE_NAME := AkaiEieDriver
APP_EXECUTABLE_NAME := AkaiEieActivator
DEXT_BUNDLE := $(BUILD_DIR)/$(DEXT_BUNDLE_ID).dext
APP_BUNDLE := $(BUILD_DIR)/AkaiEieActivator.app
DEXT_EXECUTABLE := $(GEN_BIN_DIR)/$(DEXT_EXECUTABLE_NAME)

# Without an explicit level clang builds at -O0, which leaves the per-sample
# encode/decode loops on the real-time audio path unoptimised.
OPTFLAGS ?= -O2
CXXFLAGS := -std=c++17 -fblocks $(OPTFLAGS) -target $(TRIPLE) -isysroot $(SDKROOT) \
	-I$(GEN_FRAMEWORK_DIR) -I$(GEN_DIR) -I$(SOURCE_DIR) -I..
LDFLAGS := -target $(TRIPLE) -isysroot $(SDKROOT) \
	-framework DriverKit -framework AudioDriverKit -framework USBDriverKit
SWIFTFLAGS := -O -target $(MACOS_TRIPLE) -sdk $(MACOSX_SDKROOT) \
	-module-cache-path $(SWIFT_MODULE_CACHE) \
	-framework Cocoa -framework SystemExtensions
IIGFLAGS := --framework-name $(FRAMEWORK_NAME) \
	--deployment-target $(DEPLOYMENT_TARGET)
IIG_CLANG_FLAGS := -x c++ -std=c++17 -D__IIG=1 -isysroot $(SDKROOT) \
	-I$(SOURCE_DIR) -I..

HAND_WRITTEN_SOURCES := \
	$(SOURCE_DIR)/AkaiEieDevice.cpp \
	$(SOURCE_DIR)/AkaiEieDriver.cpp

IIG_SOURCES := \
	$(GEN_DIR)/AkaiEieDevice.iig.cpp \
	$(GEN_DIR)/AkaiEieDriver.iig.cpp

OBJECTS := \
	$(GEN_DIR)/AkaiEieDevice.o \
	$(GEN_DIR)/AkaiEieDriver.o \
	$(GEN_DIR)/AkaiEieDevice.iig.o \
	$(GEN_DIR)/AkaiEieDriver.iig.o \
	$(GEN_DIR)/akai_eie_protocol.o

.PHONY: check dext app clean

check: $(OBJECTS)

dext: $(DEXT_BUNDLE)

app: $(APP_BUNDLE)

$(GEN_FRAMEWORK_DIR):
	mkdir -p $@

$(GEN_BIN_DIR):
	mkdir -p $@

$(GEN_FRAMEWORK_DIR)/AkaiEieDevice.h $(GEN_DIR)/AkaiEieDevice.iig.cpp: $(SOURCE_DIR)/AkaiEieDevice.iig | $(GEN_FRAMEWORK_DIR)
	$(IIG) --def $< --header $(GEN_FRAMEWORK_DIR)/AkaiEieDevice.h \
		--impl $(GEN_DIR)/AkaiEieDevice.iig.cpp $(IIGFLAGS) -- $(IIG_CLANG_FLAGS)

$(GEN_FRAMEWORK_DIR)/AkaiEieDriver.h $(GEN_DIR)/AkaiEieDriver.iig.cpp: $(SOURCE_DIR)/AkaiEieDriver.iig $(SOURCE_DIR)/AkaiEieDevice.iig $(SOURCE_DIR)/AkaiEieDriverConfig.hpp | $(GEN_FRAMEWORK_DIR)
	$(IIG) --def $< --header $(GEN_FRAMEWORK_DIR)/AkaiEieDriver.h \
		--impl $(GEN_DIR)/AkaiEieDriver.iig.cpp $(IIGFLAGS) -- $(IIG_CLANG_FLAGS)

$(GEN_DIR)/AkaiEieDevice.o: $(SOURCE_DIR)/AkaiEieDevice.cpp $(SOURCE_DIR)/AkaiEieDriverConfig.hpp $(GEN_FRAMEWORK_DIR)/AkaiEieDevice.h $(GEN_FRAMEWORK_DIR)/AkaiEieDriver.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(GEN_DIR)/AkaiEieDriver.o: $(SOURCE_DIR)/AkaiEieDriver.cpp $(SOURCE_DIR)/AkaiEieDriverConfig.hpp ../protocol/akai_eie_protocol.hpp $(GEN_FRAMEWORK_DIR)/AkaiEieDriver.h $(GEN_FRAMEWORK_DIR)/AkaiEieDevice.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(GEN_DIR)/AkaiEieDevice.iig.o: $(GEN_DIR)/AkaiEieDevice.iig.cpp $(GEN_FRAMEWORK_DIR)/AkaiEieDevice.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(GEN_DIR)/AkaiEieDriver.iig.o: $(GEN_DIR)/AkaiEieDriver.iig.cpp $(GEN_FRAMEWORK_DIR)/AkaiEieDriver.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(GEN_DIR)/akai_eie_protocol.o: ../protocol/akai_eie_protocol.cpp ../protocol/akai_eie_protocol.hpp | $(GEN_FRAMEWORK_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(DEXT_EXECUTABLE): $(OBJECTS) | $(GEN_BIN_DIR)
	$(CXX) $(LDFLAGS) $(OBJECTS) -o $@

$(DEXT_BUNDLE): $(DEXT_EXECUTABLE) Info.plist.template AkaiEieDriver.entitlements.template
	rm -rf $@
	mkdir -p $@/Contents/MacOS
	sed -e 's/$$(DEVELOPMENT_LANGUAGE)/en/g' \
		-e 's/$$(EXECUTABLE_NAME)/$(DEXT_EXECUTABLE_NAME)/g' \
		-e 's/$$(PRODUCT_BUNDLE_IDENTIFIER)/$(DEXT_BUNDLE_ID)/g' \
		-e 's/$$(PRODUCT_NAME)/AKAI EIE Pro Driver/g' \
		Info.plist.template > $@/Contents/Info.plist
	cp $(DEXT_EXECUTABLE) $@/Contents/MacOS/$(DEXT_EXECUTABLE_NAME)
	codesign --force --sign - --timestamp=none \
		--entitlements AkaiEieDriver.entitlements.template $@

$(APP_BUNDLE): $(DEXT_BUNDLE) ActivatorApp/Sources/AkaiEieActivator.swift ActivatorApp/Sources/main.swift ActivatorApp/Info.plist ActivatorApp/AkaiEieActivator.entitlements
	rm -rf $@
	mkdir -p $@/Contents/MacOS $@/Contents/Library/SystemExtensions
	cp ActivatorApp/Info.plist $@/Contents/Info.plist
	$(SWIFTC) $(SWIFTFLAGS) ActivatorApp/Sources/AkaiEieActivator.swift ActivatorApp/Sources/main.swift \
		-o $@/Contents/MacOS/$(APP_EXECUTABLE_NAME)
	cp -R $(DEXT_BUNDLE) $@/Contents/Library/SystemExtensions/
	codesign --force --sign - --timestamp=none \
		--entitlements ActivatorApp/AkaiEieActivator.entitlements $@

clean:
	rm -rf $(GEN_DIR) $(SWIFT_MODULE_CACHE) $(BUILD_DIR)
