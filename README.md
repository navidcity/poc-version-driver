# poc-version-driver
Driver for version register in PoC library

## Building

Against the running kernel on your machine (needs its headers installed):

    make                # poc-version.ko in the repo root
    make INJECTION=1    # test build with Injection through debugfs
    make clean

With a Yocto SDK (kernel 5.15 or newer, see ADR-0002), whose kernel sources
come from `kernel-devsrc`: source its `environment-setup-*` script, run
`make modules_prepare` once in `$SDKTARGETSYSROOT/usr/src/kernel`, then

    unset LDFLAGS && make KDIR="$SDKTARGETSYSROOT/usr/src/kernel"

The SDK exports userspace `LDFLAGS` that kbuild would pass to `ld`.

`make modules_install INSTALL_MOD_PATH=<dir>` installs the module below
`<dir>` without root or `depmod`; `make install` is the host helper that
installs into the running system with `sudo`.

### Yocto

`meta-poc-version/` is a minimal layer with the recipe `poc-version`, for
Poky 4.1 (langdale) and newer. Add it to the build and the image:

    bitbake-layers add-layer /path/to/poc-version-driver/meta-poc-version

    # local.conf or the image recipe
    IMAGE_INSTALL:append = " poc-version"

The recipe fetches this repo from GitHub (`main`) and builds through this
Makefile, which takes the kernel from Yocto's `KERNEL_SRC`. The module is
not loaded by force: udev loads it when the device tree has a node with
`compatible = "plc2,poc-git-version-register"`. To build a local clone
or a fixed commit, set in `local.conf`:

    POC_VERSION_GIT_URI = "git:///path/to/poc-version-driver;protocol=file;branch=main"
    SRCREV:pn-poc-version = "<commit>"

Release images should pin `SRCREV`; by default the recipe follows the
branch head. `devtool modify poc-version` works too.

Test images build Injection and get the board test script as
`/usr/bin/poc-version-board-test.sh`:

    PACKAGECONFIG:pn-poc-version = "injection"
    IMAGE_INSTALL:append = " poc-version poc-version-board-test"

Never use `injection` in a production image.

## Checks against the kernel trees

The cross builds, style checks and KUnit run against kernel trees that
`scripts/setup-kernels.sh` prepares. They work on any Linux host; the
devcontainer (below) is only a shortcut that installs the same packages and
runs the same script.

### Host packages

On Debian or Ubuntu (other distributions have the same tools under similar
names):

    sudo apt install bc bison build-essential cpio flex git kmod libelf-dev \
        libncurses-dev libssl-dev perl python3 rsync xz-utils zstd \
        gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf \
        clang-format device-tree-compiler dt-schema yamllint

- The kernel build dependencies and `perl` build the trees and run
  checkpatch; `python3` runs `kunit.py`. User-Mode Linux for KUnit needs an
  x86_64 host.
- `gcc-aarch64-linux-gnu` and `gcc-arm-linux-gnueabihf` are the cross-compilers
  for the boards. The setup script skips an arch whose compiler is missing,
  and that arch's targets then fail.
- `clang-format` 14 or newer is only for `make format-check`.
- `dt-schema` (the `dtschema` tools) and `yamllint` are only for
  `make dt-binding-check`.

### Preparing the trees

Prepare the trees once, into any directory, then pass the same directory to
make as `KERNELS`:

    scripts/setup-kernels.sh "$HOME/kernels"
    make matrix kunit-all KERNELS="$HOME/kernels"

The script shallow-clones `v5.15` (the minimum, see ADR-0002), `v6.4` and the
latest stable tag from git.kernel.org, runs `defconfig` + `modules_prepare`
for arm64 and arm, and runs `make kunit-all` once, which builds a User-Mode
Linux kernel for `v5.15` and one for the latest tree in `build/`, where every
later `make kunit` reuses them. The first run takes a while.

Run it again at any time: it only does what is missing, and it moves `latest`
to a newer stable tag when one has appeared. Knobs, all environment variables:
`ARCHES` (default `arm64 arm`), `KUNIT=0` skips the KUnit runs, `FULL_BUILD=1`
builds the whole kernel instead of `modules_prepare`, `KERNEL_GIT_URL` clones
from a mirror, `LATEST_TAG` fixes the latest tree, `JOBS` sets the make jobs.

Trees prepared with `modules_prepare` have no `Module.symvers`, so modpost
warns that it is missing and may list kernel symbols as undefined. Those
warnings come from the tree, not the driver; compiler warnings are what
counts. `FULL_BUILD=1` creates `Module.symvers` and silences them.

### Targets

Every target takes `KERNELS=<dir>`; set it once in the environment to leave it
out:

    make v5.15-arm64    # also v6.4-arm64, latest-arm64, and each with -arm
    make matrix         # all six; add INJECTION=1 for test builds
    make checkpatch     # authoritative style check, the latest tree's checkpatch
    make format-check   # clang-format --dry-run with the latest tree's .clang-format (advisory)
    make kunit          # KUnit under User-Mode Linux in the latest tree
    make kunit KUNIT_TREE="$KERNELS/src/v5.15"  # the same in v5.15
    make kunit-all      # both runs, v5.15 then latest; fails if either fails
    make dt-binding-check  # schema check of the device tree binding

A target whose tree is missing stops with a hint to run the setup script.

`make format-check` is advisory only: clang-format and checkpatch disagree in
places, checkpatch decides, so format-check differences are not errors and
no other target depends on it.

The repo keeps no copy of checkpatch or the kernel's `.clang-format`: both
come from the prepared latest tree, so they match upstream. Without the
trees, point them at any kernel checkout:

    make checkpatch CHECKPATCH=/path/to/linux/scripts/checkpatch.pl
    make format-check CLANG_FORMAT_STYLE=/path/to/linux/.clang-format

For editor integration, link the tree's `.clang-format` into the repo root
(`.clang-format` is ignored by git).

Each tree × arch target builds in its own directory under `build/`, so they
don't overwrite each other or the host build. `make kunit` keeps a symlink
farm and a UML build per tree there too (`kunit-src-v5.15`, `kunit-um-latest`,
...), so a `v5.15` run never rebuilds the latest one or the other way round;
a farm is rebuilt when `latest` moves to a new tag or the Makefile changes.
Extra kernel options for one tree's UML go in `.kunitconfig-<tree>` next to
the shared `.kunitconfig`: `v5.15` needs PCI over virtio there to get
`HAS_IOMEM`, which later trees' `kunit.py` turns on by itself. `POC_OUT=<dir>` moves
all of this elsewhere; a relative `POC_OUT` is taken from the repo root.

`make clean` removes `POC_OUT` by these rules:

- `build/` or a path under it (e.g. `POC_OUT=build/relout`) is always
  removed.
- A path outside the repo is removed only when `POC_OUT` is given on the make
  command line; one from the environment is left alone.
- `/`, the repo itself, a parent of it, and any other path in the repo
  outside `build/` (`.git`, `docs`, or a relative `POC_OUT=relout`) are
  never removed, wherever `POC_OUT` comes from. Keep in-repo builds under
  `build/`.

It also skips the kbuild clean when there are no host kernel headers, as in
the container. After `make clean` the next `make kunit` builds UML from
scratch.

### Device tree binding

`Documentation/devicetree/bindings/misc/plc2,poc-git-version-register.yaml`
describes the node: `compatible = "plc2,poc-git-version-register"` and one
`reg` region, nothing else. `make dt-binding-check` runs the kernel's
`dt_binding_check` for it in a symlink farm of the latest tree under
`build/` (like `make kunit`), so it also compiles and validates the example.
It needs `dtschema` and `yamllint` and fails if either is missing, and it
fails on any error or warning, which the kernel's target only prints.

Upstream `vendor-prefixes.yaml` has no `plc2`, so the farm gets a copy of it
with a `plc2` entry; a tree that already lists `plc2` (as the BSP kernels do)
is used as it is. The prepared tree itself is never changed.

To validate a board's device tree against the binding, copy the binding
into the board's kernel tree at the same path, make sure that tree's
`vendor-prefixes.yaml` has `plc2`, and run `make dtbs_check` there (with the
board's `ARCH`, `CROSS_COMPILE` and defconfig, and
`DT_SCHEMA_FILES=misc/plc2,poc-git-version-register.yaml` to check only this
binding). A node from the AMD/Xilinx device tree generator may also carry
`xlnx,*` or clock properties, which the binding doesn't allow.

### Devcontainer

`.devcontainer/` is a shortcut for the host setup above: a Debian bookworm
image with the packages listed there, nothing else. When the container is
created it runs `scripts/setup-kernels.sh` into the `poc-version-kernels`
named volume, which is the default of `KERNELS` (`/opt/kernels`), so the
targets need no `KERNELS=` inside it. Rebuilding the container skips the
tree setup, because the volume keeps the trees.

## Testing on a running board

The board's own device tree describes each Instance: a node with
`compatible = "plc2,poc-git-version-register"` and the IP core's address
range from Vivado's Address Editor in `reg`, as in the binding's example
(`Documentation/devicetree/bindings/misc/plc2,poc-git-version-register.yaml`,
see "Device tree binding" above). If the node comes from the AMD/Xilinx
device tree generator, add `"plc2,poc-git-version-register"` to its
`compatible` rather than adding a second node.

The bitstream with the IP core must be loaded, and its AXI clock running,
before the driver probes the node: probe reads the IP core straight away,
and an AXI access to unprogrammed logic can hang the board. With the node
in the boot device tree, that means the bitstream is loaded by the boot
loader, or the module is loaded only after the bitstream.

On the board, as root:

    insmod poc-version.ko
    ls /sys/bus/platform/drivers/poc-version/    # a0010000.version

Each bound Instance is listed by its device name, the unit address followed
by the node name. Then run the board test, which needs a module built with
`INJECTION=1`:

    sh poc-version-board-test.sh a0010000.version

`rmmod poc-version` unbinds every Instance again.
