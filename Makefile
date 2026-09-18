SHELL := /bin/sh
PROJECT := $(CURDIR)
SDK ?= $(PROJECT)/.deps/MacKernelSDK
LILU ?= $(PROJECT)/.deps/Lilu/Lilu
TC := $(shell xcrun --find clang++ | xargs dirname)
export SDKROOT := $(shell xcrun --sdk macosx --show-sdk-path)
CXX := $(TC)/clang++
LD := $(TC)/ld
# Never build into Release/: that directory holds the physically tested artifact.
OUT := $(PROJECT)/build/poc-0.2.25
KEXT := $(OUT)/BroadcomVTD.kext
FLAGS := -target x86_64-apple-macos10.15 -std=c++14 -mkernel -nostdinc -nostdinc++ \
 -isystem $(SDK)/Headers -isystem $(LILU) -IConfig -IPOC \
 -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -D__APPLE__ \
 -DPRODUCT_NAME=BroadcomVTD -DMODULE_VERSION=0.2.25 \
 -O2 -fno-exceptions -fno-rtti -fno-builtin -fno-stack-protector \
 -fno-asynchronous-unwind-tables -fno-threadsafe-statics -mno-red-zone \
 -mno-sse -mno-mmx -fvisibility=hidden -Wall -Wextra -Werror \
 -Wno-unused-parameter -Wno-deprecated-declarations -Wno-unknown-pragmas \
 -Wno-deprecated-register -Wno-ossharedptr-misuse \
 -ffile-prefix-map=$(PROJECT)=BroadcomVTD-Trace
NAMES := Frontend TxQualification TxDisposition TxQuiescence MapperCore RxMapperCore Trace Module plugin_start
OBJECTS := $(addprefix $(OUT)/,$(addsuffix .o,$(NAMES)))
.PHONY: all dependencies verify test repeat
all: $(KEXT)/Contents/MacOS/BroadcomVTD
dependencies:
	python3 -B tools/fetch_dependencies.py
$(OUT):
	mkdir -p $(OUT)
$(OUT)/%.o: POC/%.cpp $(wildcard POC/*.hpp) Config/TargetGate.hpp Makefile | $(OUT)
	@test -f $(SDK)/Headers/Availability.h -a -f $(LILU)/Headers/kern_api.hpp || (echo 'Missing dependencies: run make dependencies'; exit 1)
	$(CXX) $(FLAGS) -c $< -o $@
$(OUT)/plugin_start.o: $(LILU)/Library/plugin_start.cpp Makefile | $(OUT)
	$(CXX) $(FLAGS) -c $< -o $@
$(KEXT)/Contents/MacOS/BroadcomVTD: $(OBJECTS) POC/Info.plist
	install -d $(KEXT)/Contents/MacOS
	install -m 644 POC/Info.plist $(KEXT)/Contents/Info.plist
	$(LD) -arch x86_64 -kext -no_adhoc_codesign -undefined dynamic_lookup \
	 -platform_version macos 10.15 26.0 -o $@ $(OBJECTS) -L$(SDK)/Library/x86_64 -lkmod
	codesign --force --sign - --timestamp=none $(KEXT)
test:
	python3 -B tools/test_public.py
verify: all
	python3 -B tools/validate_public_build.py
	python3 -B tools/verify_release.py
repeat:
	python3 -B tools/repeat_public_build.py
