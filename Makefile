# Select a full Xcode installation using DEVELOPER_DIR if xcode-select points
# to the standalone Command Line Tools. See README for an example.
-include localconfig.mk

XCODEBUILD ?= xcodebuild
XCODEBUILD_FLAGS ?= CODE_SIGNING_ALLOWED=NO ONLY_ACTIVE_ARCH=NO

# The package signing certificate must either be set or explicitly disabled:
ifeq (,$(CODESIGN_INST))
    $(error Please set CODESIGN_INST variable to your Mac Installer \
      certificate or 'none' if you don't have any. \
      E.g. "export CODESIGN_INST=G3H8VBSL7A")
endif

# Command-line variables cannot be reassigned by makefile assignments.
INSTALLER_SIGN_ID := $(if $(filter none,$(CODESIGN_INST)),,$(CODESIGN_INST))

all: build/Release/HoRNDIS.kext build/pkg/_complete

clean:
	rm -rf build

# We now sign as part of the xcodebuild process.
build/Release/HoRNDIS.kext: $(wildcard *.cpp *.h *.plist HoRNDIS.xcodeproj/* *.lproj/*)
	$(XCODEBUILD) -project HoRNDIS.xcodeproj -configuration Release $(XCODEBUILD_FLAGS)

build/pkg/root: build/Release/HoRNDIS.kext
	rm -rf build/pkg/
	mkdir -p build/pkg/root/Library/Extensions
	cp -R build/Release/HoRNDIS.kext build/pkg/root/Library/Extensions/

build/pkg/HoRNDIS-kext.pkg: build/pkg/root
	pkgbuild --identifier com.joshuawise.kexts.HoRNDIS --scripts package/scripts --root $< $@

# The variable is to be resolved first time it's used:
VERSION = $(shell defaults read $(PWD)/build/Release/HoRNDIS.kext/Contents/Info.plist CFBundleVersion)

build/pkg/_complete: build/pkg/HoRNDIS-kext.pkg $(wildcard package/*)
	productbuild --distribution package/Distribution.xml --package-path build/pkg --resources package --version $(VERSION) $(if $(INSTALLER_SIGN_ID),--sign $(INSTALLER_SIGN_ID)) build/HoRNDIS-$(VERSION).pkg && touch build/pkg/_complete
