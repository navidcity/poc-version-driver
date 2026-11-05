# SPDX-License-Identifier: GPL-2.0-only

obj-m += poc-version.o

# Injection into the Cached copy through debugfs, for testing only:
#   make INJECTION=1
ifeq ($(INJECTION),1)
ccflags-y += -DPOC_VERSION_INJECTION
endif

# Kernel build directory. Yocto's module class passes KERNEL_SRC instead.
ifneq ($(KERNEL_SRC),)
KDIR ?= $(KERNEL_SRC)
endif
KDIR ?= /lib/modules/$(shell uname -r)/build

# Current directory
PWD := $(shell pwd)

# Default target: build the module
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

bear:
	bear --append --output $(PWD)/.vscode/compile_commands.json -- $(MAKE) -C $(KDIR) M=$(PWD) modules

# Clean generated files. The build dir goes first, and the kbuild clean
# only runs when KDIR is there (the devcontainer has no host kernel
# headers), so a missing KDIR never keeps the build dir around.
clean:
	$(clean_poc_out)
ifneq ($(wildcard $(KDIR)/Makefile),)
	$(MAKE) -C $(KDIR) M=$(PWD) clean
else
	@echo "  SKIP    kbuild clean, no $(KDIR)/Makefile"
endif

# Install the module into the running kernel
install:
	sudo $(MAKE) -C $(KDIR) M=$(PWD) modules_install
	sudo depmod -a

# Install below INSTALL_MOD_PATH (or MODLIB), without root or depmod, as
# Yocto's module class and staging installs expect
modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

# Load/unload helpers
load:
	sudo modprobe poc-version

unload:
	sudo modprobe -r poc-version

.PHONY: all bear clean install modules_install load unload

# ---------------------------------------------------------------------------
# Cross builds against the prepared kernel trees, and style checks.
#
# The trees come from scripts/setup-kernels.sh. Point KERNELS at the root
# you gave the script, e.g. make matrix KERNELS=$$HOME/kernels; the default
# is where the devcontainer keeps them.
#
#   make v5.15-arm64 v6.4-arm64 latest-arm64 ...  one tree x arch, each tree
#                        also with -arm
#   make matrix          all six of them
#   make checkpatch      the latest tree's checkpatch.pl on every source file
#   make format-check    clang-format --dry-run with the latest tree's
#                        .clang-format; advisory only, checkpatch decides
#   make kunit           KUnit under User-Mode Linux in the latest tree;
#                        KUNIT_TREE=$(KERNELS)/src/v5.15 for the oldest
#   make kunit-all       KUnit in v5.15 and the latest tree (KUNIT_TREES)
#   make dt-binding-check  dt_binding_check of the device tree binding
#                        in the latest tree
#
# All build targets accept INJECTION=1. Each one builds in its own copy
# under $(POC_OUT)/<tree>-<arch>, so they don't clobber each other or the
# host build in the repo root, and "make -j matrix" is safe.
# ---------------------------------------------------------------------------
ifeq ($(KERNELRELEASE),)

KERNELS ?= /opt/kernels
POC_OUT ?= $(CURDIR)/build

# Every recipe uses the absolute poc_out, so a relative POC_OUT still works
# after "make -C <tree>"; the origin checks look at POC_OUT itself.
poc_out := $(abspath $(POC_OUT))
poc_build := $(CURDIR)/build

# "make clean" deletes $(poc_out):
#   - never /, this repo, a parent of it, or anything else in the repo
#     outside build/ (that holds tracked files, .git included);
#   - build/ or a path under it, whatever the origin;
#   - any other path only when POC_OUT is given on the make command line.
poc_out_holds_repo = $(or $(filter / $(CURDIR),$(poc_out)),$(filter $(poc_out)/%,$(CURDIR)))
poc_out_in_build = $(filter $(poc_build) $(poc_build)/%,$(poc_out))
poc_out_in_repo = $(filter $(CURDIR)/%,$(poc_out))
poc_out_cmdline = $(filter command line,$(origin POC_OUT))

poc_out_skip = @echo "  SKIP    rm $(poc_out): $(1)"
clean_poc_out = $(if $(poc_out), \
	$(if $(poc_out_holds_repo),$(call poc_out_skip,refused: it holds $(CURDIR)), \
	$(if $(poc_out_in_build),rm -rf $(poc_out), \
	$(if $(poc_out_in_repo),$(call poc_out_skip,refused: in $(CURDIR) but not under build/), \
	$(if $(poc_out_cmdline),rm -rf $(poc_out), \
	$(call poc_out_skip,outside $(CURDIR); give POC_OUT on the command line to remove it))))))

TREES := v5.15 v6.4 latest
ARCHES := arm64 arm
MATRIX := $(foreach t,$(TREES),$(foreach a,$(ARCHES),$(t)-$(a)))

CROSS_COMPILE_arm64 ?= aarch64-linux-gnu-
CROSS_COMPILE_arm ?= arm-linux-gnueabihf-

# Files a build needs; linked into each per-target build dir.
SRCS = $(wildcard $(CURDIR)/*.c $(CURDIR)/*.h)
BUILD_FILES = $(SRCS) $(wildcard $(CURDIR)/Makefile $(CURDIR)/Kbuild)

target_arch = $(lastword $(subst -, ,$(1)))

# A tree that only had modules_prepare has no Module.symvers, so modpost
# would call every kernel symbol undefined. Only warn about them there;
# a fully built tree (with Module.symvers) keeps the strict check.
modpost_warn = $(if $(wildcard $(KERNELS)/build/$(1)/Module.symvers),,KBUILD_MODPOST_WARN=1)

# v5.15's arch/arm/Makefile tests -march=armv7-a with cc-option before
# -msoft-float is in the flags. A hard-float GCC 11 or newer (Debian's
# arm-linux-gnueabihf-gcc) rejects that, so the build falls back to
# -march=armv5t and the assembler rejects isb. Later 5.15.y releases pass
# -march=armv7-a directly; KCFLAGS comes last, so it wins here.
KCFLAGS_v5.15-arm := -march=armv7-a

matrix: $(MATRIX)

$(MATRIX):
	@test -d $(KERNELS)/build/$@ || \
		{ echo "$(KERNELS)/build/$@ missing: run scripts/setup-kernels.sh $(KERNELS)" >&2; exit 1; }
	@mkdir -p $(poc_out)/$@
	@find $(poc_out)/$@ -maxdepth 1 -type l -delete
	@ln -s $(BUILD_FILES) $(poc_out)/$@/
	$(MAKE) -C $(KERNELS)/build/$@ M=$(poc_out)/$@ \
		ARCH=$(call target_arch,$@) \
		CROSS_COMPILE=$(CROSS_COMPILE_$(call target_arch,$@)) \
		$(if $(KCFLAGS_$@),KCFLAGS="$(KCFLAGS_$@)") \
		$(call modpost_warn,$@) modules

# Both come from the latest tree (checkpatch reads spelling.txt and
# const_structs.checkpatch next to itself). Point CHECKPATCH and
# CLANG_FORMAT_STYLE at any other kernel checkout to run them without the
# prepared trees.
CHECKPATCH ?= $(KERNELS)/src/latest/scripts/checkpatch.pl
CHECKPATCH_FLAGS ?= --no-tree --strict -f
CLANG_FORMAT ?= clang-format
CLANG_FORMAT_STYLE ?= $(KERNELS)/src/latest/.clang-format

tree_file_missing = { echo "$(1) missing: run scripts/setup-kernels.sh $(KERNELS), or set $(2)" >&2; exit 1; }

checkpatch:
	@command -v $(CHECKPATCH) >/dev/null || $(call tree_file_missing,$(CHECKPATCH),CHECKPATCH)
	$(CHECKPATCH) $(CHECKPATCH_FLAGS) $(notdir $(SRCS))

format-check:
	@test -f $(CLANG_FORMAT_STYLE) || $(call tree_file_missing,$(CLANG_FORMAT_STYLE),CLANG_FORMAT_STYLE)
	$(CLANG_FORMAT) --style=file:$(CLANG_FORMAT_STYLE) --dry-run --Werror $(notdir $(SRCS))

# KUnit under User-Mode Linux, against one tree (KUNIT_TREE, default the
# latest).
#
# The tree itself is never changed: $(KUNIT_SRC) is a symlink farm of it in
# which drivers/misc/{Kconfig,Makefile} are real copies that also pull in
# drivers/misc/poc-version/. That directory links to Kconfig and the sources
# here, so the run always tests the working copy. kunit.py resolves its own
# path, so its directory is copied rather than linked.
#
# The farm and UML build dirs are named after the tree (kunit-src-v5.15,
# kunit-um-latest, ...), so runs against different trees never rebuild each
# other. A farm is rebuilt when its tree path resolves somewhere new, e.g.
# src/latest points to a new tag (which also drops its UML build), or when
# this Makefile is newer than the farm, so a change to the recipe below
# never leaves old copies behind (the UML build is kept then). The first run
# builds a whole UML kernel in $(KUNIT_BUILD); later runs are incremental.
# scripts/setup-kernels.sh runs "make kunit-all" once after preparing the
# trees, so the first run afterwards is already incremental.
#
# kunit.py gets .kunitconfig plus, when there is one, the tree's own
# fragment .kunitconfig-<tree> (e.g. .kunitconfig-v5.15), concatenated into
# the farm: kunit.py before 6.0 takes a single --kunitconfig.
KUNIT_TREE ?= $(KERNELS)/src/latest
kunit_tree_name = $(notdir $(patsubst %/,%,$(KUNIT_TREE)))
KUNIT_SRC ?= $(poc_out)/kunit-src-$(kunit_tree_name)
KUNIT_BUILD ?= $(poc_out)/kunit-um-$(kunit_tree_name)
KUNIT_ARGS ?=
KUNIT_DIR := drivers/misc/poc-version
KUNIT_CONFIGS = $(CURDIR)/.kunitconfig \
	$(wildcard $(CURDIR)/.kunitconfig-$(kunit_tree_name))

# Trees under $(KERNELS)/src that "make kunit-all" runs, one after the other
KUNIT_TREES ?= v5.15 latest

kunit:
	@test -f $(KUNIT_TREE)/Makefile || \
		{ echo "$(KUNIT_TREE) missing: run scripts/setup-kernels.sh $(KERNELS)" >&2; exit 1; }
	@tree=$$(realpath $(KUNIT_TREE)); \
	stamp=$(KUNIT_SRC)/.poc-tree; \
	if [ "$$(cat $$stamp 2>/dev/null)" != "$$tree" ]; then \
		rm -rf $(KUNIT_BUILD); \
	elif [ $(CURDIR)/Makefile -nt $$stamp ]; then \
		echo "  FARM    Makefile changed"; \
	else \
		tree=; \
	fi; \
	if [ -n "$$tree" ]; then \
		echo "  FARM    $(KUNIT_SRC) <- $$tree"; \
		rm -rf $(KUNIT_SRC) && \
		mkdir -p $(KUNIT_SRC) && \
		cp -rs $$tree/. $(KUNIT_SRC)/ && \
		rm -rf $(KUNIT_SRC)/tools/testing/kunit && \
		cp -r $$tree/tools/testing/kunit $(KUNIT_SRC)/tools/testing/ && \
		for f in Kconfig Makefile; do \
			rm $(KUNIT_SRC)/drivers/misc/$$f && \
			cp $$tree/drivers/misc/$$f $(KUNIT_SRC)/drivers/misc/$$f || exit 1; \
		done && \
		echo 'source "$(KUNIT_DIR)/Kconfig"' >>$(KUNIT_SRC)/drivers/misc/Kconfig && \
		echo 'obj-$$(CONFIG_POC_VERSION) += poc-version/' >>$(KUNIT_SRC)/drivers/misc/Makefile && \
		mkdir -p $(KUNIT_SRC)/$(KUNIT_DIR) && \
		echo 'obj-$$(CONFIG_POC_VERSION) += poc-version.o' >$(KUNIT_SRC)/$(KUNIT_DIR)/Makefile && \
		echo "$$tree" >$$stamp || exit 1; \
	fi
	@ln -sf $(CURDIR)/Kconfig $(CURDIR)/poc-version.c $(CURDIR)/poc-version-test.c \
		$(KUNIT_SRC)/$(KUNIT_DIR)/
	@cat $(KUNIT_CONFIGS) >$(KUNIT_SRC)/.poc-kunitconfig
	python3 $(KUNIT_SRC)/tools/testing/kunit/kunit.py run --arch=um \
		--build_dir=$(KUNIT_BUILD) --kunitconfig=$(KUNIT_SRC)/.poc-kunitconfig \
		$(KUNIT_ARGS)

# Every tree in KUNIT_TREES, even after one fails; fails if any did
kunit-all:
	@failed=; \
	for t in $(KUNIT_TREES); do \
		echo "  KUNIT   $$t"; \
		$(MAKE) --no-print-directory kunit KUNIT_TREE=$(KERNELS)/src/$$t || \
			failed="$$failed $$t"; \
	done; \
	if [ -n "$$failed" ]; then \
		echo "kunit-all: failed on$$failed" >&2; exit 1; \
	fi

# Schema check of the device tree binding, against the latest tree.
#
# Like kunit, this runs in a symlink farm of the tree, $(DT_SRC), so the tree
# itself is never changed. The binding is linked in at its path, and
# vendor-prefixes.yaml is a real copy with a plc2 entry added when the tree
# has none; a tree that already lists plc2 keeps its own file. Both are
# redone on every run; the farm itself only when src/latest points to a new
# tag. The kernel's dt_binding_check reports schema and example errors
# without failing, so any output line that isn't a kbuild progress line
# fails the target.
DT_TREE ?= $(KERNELS)/src/latest
DT_SRC ?= $(poc_out)/dt-src
DT_BUILD ?= $(poc_out)/dt-build
DT_BINDINGS := Documentation/devicetree/bindings
DT_BINDING := misc/plc2,poc-git-version-register.yaml
DT_VENDORS := $(DT_BINDINGS)/vendor-prefixes.yaml

dt-binding-check:
	@test -f $(DT_TREE)/$(DT_VENDORS) || \
		{ echo "$(DT_TREE) missing: run scripts/setup-kernels.sh $(KERNELS)" >&2; exit 1; }
	@for t in dt-doc-validate dt-mk-schema dt-validate yamllint; do \
		command -v $$t >/dev/null || \
			{ echo "$$t not found: install dtschema and yamllint, see the README" >&2; exit 1; }; \
	done
	@tree=$$(realpath $(DT_TREE)); \
	stamp=$(DT_SRC)/.poc-tree; \
	if [ "$$(cat $$stamp 2>/dev/null)" != "$$tree" ]; then \
		echo "  FARM    $(DT_SRC) <- $$tree"; \
		rm -rf $(DT_SRC) $(DT_BUILD) && \
		mkdir -p $(DT_SRC) && \
		cp -rs $$tree/. $(DT_SRC)/ && \
		echo "$$tree" >$$stamp || exit 1; \
	fi; \
	rm -f $(DT_SRC)/$(DT_VENDORS); \
	if grep -q '"^plc2,' $$tree/$(DT_VENDORS); then \
		ln -s $$tree/$(DT_VENDORS) $(DT_SRC)/$(DT_VENDORS); \
	else \
		echo "  VENDOR  plc2 added to the farm's vendor-prefixes.yaml"; \
		LC_ALL=C awk -v key='"^plc2,.*":' ' \
			!done && /^  "\^[^"]*":$$/ && substr($$0, 3) > key { \
				print "  " key; print "    description: PLC2 GmbH"; done = 1 \
			} \
			{ print }' $$tree/$(DT_VENDORS) >$(DT_SRC)/$(DT_VENDORS) && \
		grep -q '"^plc2,' $(DT_SRC)/$(DT_VENDORS) || \
			{ echo "could not add plc2 to $(DT_VENDORS)" >&2; exit 1; }; \
	fi
	@ln -sf $(CURDIR)/$(DT_BINDINGS)/$(DT_BINDING) $(DT_SRC)/$(DT_BINDINGS)/$(DT_BINDING)
	@mkdir -p $(DT_BUILD); \
	{ $(MAKE) -C $(DT_SRC) O=$(DT_BUILD) dt_binding_check \
		DT_SCHEMA_FILES=$(DT_BINDING) 2>&1; echo $$? >$(DT_BUILD)/.poc-status; } | \
		tee $(DT_BUILD)/.poc-log; \
	status=$$(cat $(DT_BUILD)/.poc-status); \
	[ "$$status" = 0 ] || exit $$status; \
	if grep -v -E '^  [A-Z0-9_]+( \[[A-Z]\])? +[^ ]|^make\[[0-9]+\]: (Entering|Leaving) directory|^$$' \
		$(DT_BUILD)/.poc-log >/dev/null; then \
		echo "dt-binding-check: errors or warnings above" >&2; exit 1; \
	fi

.PHONY: matrix $(MATRIX) checkpatch format-check kunit kunit-all dt-binding-check

endif
