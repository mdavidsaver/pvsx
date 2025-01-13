TOP=..

_PVXS_BOOTSTRAP = YES

include $(TOP)/configure/CONFIG

CMAKE ?= cmake

ifdef T_A

ifneq (,$(wildcard $(CURDIR)/../libevent/CMakeLists.txt))
LIBEVENT_USE_CMAKE?=YES
else
LIBEVENT_USE_CMAKE?=NO
endif

CMAKEFLAGS = $(CMD_CMAKEFLAGS)

# override definitions from CONFIG_PVXS_MODULE, but not from CLI arguments
LIBEVENT = $(LIBEVENT_$(T_A))
LIBEVENT_$(T_A) = $(INSTALL_LOCATION)/bundle/usr/$(T_A)

CMAKEFLAGS += -DCMAKE_INSTALL_PREFIX:PATH="$(abspath $(LIBEVENT_$(T_A)))"

# not needed, and may not be available on embedded targets, so never try
CMAKEFLAGS += -DEVENT__DISABLE_MBEDTLS=ON

# not run, so why bother?
CMAKEFLAGS += -DEVENT__DISABLE_REGRESS=ON
CMAKEFLAGS += -DEVENT__DISABLE_SAMPLES=ON
CMAKEFLAGS += -DEVENT__DISABLE_TESTS=ON
CMAKEFLAGS += -DEVENT__DISABLE_BENCHMARK=ON

CMAKEFLAGS += -DCMAKE_MACOSX_RPATH=ON -DCMAKE_SKIP_INSTALL_RPATH=OFF

# searched for toolchain files
CMAKEFLAGS += -DCMAKE_MODULE_PATH:DIR="$(abspath $(TOP)/bundle/cmake)"

CMAKEFLAGS += -DCMAKE_MAKE_PROGRAM="$(MAKE)"

# check either $(HOST_OPT) or $(CROSS_OPT) depending on $(BUILD_CLASS)
IS_OPT = $($(BUILD_CLASS)_OPT)
# cmake build types: DEBUG RELEASE RELWITHDEBINFO MINSIZEREL
CMAKEFLAGS_DEBUG_YES += -DCMAKE_BUILD_TYPE=RELEASE
CMAKEFLAGS_DEBUG_NO += -DCMAKE_BUILD_TYPE=RELWITHDEBINFO -DCMAKE_DEBUG_POSTFIX:STRING=
CMAKEFLAGS += $(CMAKEFLAGS_DEBUG_$(IS_OPT))

ifeq ($(OS_CLASS),WIN32)
# On Win32 we must choose between only static and only shared
CMAKEFLAGS_SHARED_YES += -DEVENT__LIBRARY_TYPE=SHARED
else
CMAKEFLAGS_SHARED_YES += -DEVENT__LIBRARY_TYPE=BOTH
endif
CMAKEFLAGS_SHARED_NO += -DEVENT__LIBRARY_TYPE=STATIC
CMAKEFLAGS += $(CMAKEFLAGS_SHARED_$(SHARED_LIBRARIES))

ifneq (,$(filter linux-%,$(EPICS_HOST_ARCH)))
# cross mingw hosted on linux
CMAKE_TOOLCHAIN_windows-x64-mingw ?= x86_64-w64-mingw32
endif

ifneq (,$(filter win%,$(EPICS_HOST_ARCH)))
CMAKEFLAGS += -G "MinGW Makefiles" -DCMAKE_C_COMPILER="$(CC)"
endif

ifneq (,$(filter RTEMS%,$(T_A)))
CMAKE_TOOLCHAIN_$(T_A) = $(T_A).cmake
CMAKE_TOOLCHAIN_D_$(T_A) = $(CMAKE_TOOLCHAIN_$(T_A))
endif

# toolchain file when cross compiling
CMAKE_TOOLCHAIN_D ?= $(CMAKE_TOOLCHAIN_D_$(T_A))
CMAKE_TOOLCHAIN ?= $(CMAKE_TOOLCHAIN_$(T_A))
CMAKEFLAGS += $(if $(CMAKE_TOOLCHAIN),-DCMAKE_TOOLCHAIN_FILE="$(CMAKE_TOOLCHAIN)")

# flags for cmake --build
# eg. "-j 2"  (that space is important)
CBUILDFLAGS +=

endif # T_A

all: help

help:
	$(ECHO) "Building of bundled libevent is not automatic!"
	$(ECHO) " Requires $(CMAKE)"
	$(ECHO) " Possible targets: $(CROSS_COMPILER_TARGET_ARCHS)"
	$(ECHO) ""
	$(ECHO) "Usage:"
	$(ECHO) " # alias for libevent.$(EPICS_HOST_ARCH)"
	$(ECHO) " make -C bundle libevent"
	$(ECHO) " # build for one target"
	$(ECHO) " make -C bundle libevent.<epics-arch>"
	$(ECHO) " # remove all target"
	$(ECHO) " make -C bundle clean  # remove all for targets"
	$(ECHO) " # remove one target"
	$(ECHO) " make -C bundle clean.<epics-arch> # remove for one target"

libevent: libevent.$(EPICS_HOST_ARCH)

clean:
	$(RMDIR) $(CURDIR)/O.* $(CURDIR)/usr

clean.%:
	$(RMDIR) $(CURDIR)/O.$* $(CURDIR)/usr/$*

ifndef T_A

# re-entry this Makefile with target configuration
libevent.%:
	$(MKDIR) $(CURDIR)/O.$*
	$(MAKE) -C $(CURDIR)/O.$* -f $(CURDIR)/Makefile TOP=$(TOP)/.. T_A=$* libevent.$*

else # T_A

ifeq (YES,$(LIBEVENT_USE_CMAKE))
# libevent >= 2.1 prefer CMake

libevent.%: $(CMAKE_TOOLCHAIN_D)
	$(CMAKE) $(CMAKEFLAGS) ../libevent
	$(CMAKE) -E env --unset=MAKEFLAGS --unset=CC $(CMAKE) --build . $(CBUILDFLAGS) --target install
ifneq (,$(filter %mingw,$(T_A)))
ifeq (NO,$(SHARED_LIBRARIES))
	$(CP) $(LIBEVENT_$(T_A))/lib/libevent_core.a $(LIBEVENT_$(T_A))/lib/event_core.lib
# HACK: when STATIC_BUILD=YES we (in configure/RULES_PVXS_MODULE) inject the event_core
#       dependency via <target>_LIBS += event_core which requires the existance of
#       $(event_core_DIR)/event_core.lib however, cmake outputs the *nix style name
#       libevent_core.a (probably because it can coexist with the dll import library
#       named libevent_core.dll.a).
#       As a crutch we make a copy
endif
endif

EXPANDFLAGS = $(foreach var,$(EXPANDVARS),-D$(var)="$($(var))")

ifneq (,$(filter RTEMS%,$(T_A)))
# cf. cmake/RTEMS.cmake@
EXPANDVARS+=RTEMS_CPU
EXPANDVARS+=RTEMS_BSP
EXPANDVARS+=RTEMS_VERSION
EXPANDVARS+=RTEMS_BASE
EXPANDVARS+=CPU_CFLAGS

$(T_A).cmake: ../cmake/RTEMS.cmake@
	$(PERL) $(TOOLS)/expandVars.pl -a $(T_A) -t $(INSTALL_LOCATION) $(EXPANDFLAGS) $< $@
# can't use $(EXPAND_TOOL) as this def is in a RULES_ file...
endif


else
# libevent 2.0 only autotools
# only host CC supported

CONFIGUREFLAGS = $(CMD_CONFIGUREFLAGS)

CONFIGUREFLAGS += --disable-openssl

# The definition from Base includes arguments '-ru' which confuses autoconf 2.69 (circa rhel7)
CONFIGUREFLAGS+=AR=$(firstword $(AR))

ifeq ($(OS_CLASS),WIN32)
# On Win32 we must choose between only static and only shared
CONFIGUREFLAGS_SHARED_YES += --enable-shared --disable-static
else
CONFIGUREFLAGS_SHARED_YES += --enable-shared --enable-static
endif
CONFIGUREFLAGS_SHARED_NO += --disable-shared --enable-static
CONFIGUREFLAGS += $(CONFIGUREFLAGS_SHARED_$(SHARED_LIBRARIES))

libevent.%: ../libevent/configure
	[ -f Makefile ] || ../libevent/configure --prefix=$(abspath $(LIBEVENT_$(T_A))) $(CONFIGUREFLAGS)
	$(MAKE) install

../libevent/configure:
	(cd ../libevent && autoreconf -ivf)

endif

endif # T_A

.PHONY: libevent libevent.%

# These rules support printing a Makefile variable values.
# Many variables are only set inside an O.<arch> build directory.
#   make PRINT.T_A

PRINT_Var = $(@:PRINT.%=%)
PRINT.%:
	@echo $(PRINT_Var) = '$($(PRINT_Var))'

.PHONY: PRINT PRINT.%
