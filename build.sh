#!/bin/sh

set -e

MODULE_NAME="sfc7120pol"
CHERI_ROOT="${CHERI_ROOT:-$HOME/cheri}"
CHERIBSD_SOURCE_PATH="${2:-$HOME/projects/cheribsd}"

# Dependency paths
CHERIBUILD_PATH="$HOME/cheribuild"
CLANG_PATH="$CHERI_ROOT/output/morello-sdk/bin/clang"
LD_PATH="$CHERI_ROOT/output/morello-sdk/bin/ld.lld"
OBJCOPY_PATH="$CHERI_ROOT/output/morello-sdk/bin/llvm-objcopy"
BMAKE_PATH="$CHERI_ROOT/build/cheribsd-morello-purecap-build/bmake-install/bin/bmake"
MAKEFILES_PATH="$CHERI_ROOT/output/rootfs-morello-purecap/usr/share/mk"
CHERIBSD_SYSDIR="$CHERI_ROOT/cheribsd/sys"

# Set Bear path based on the system
if [ "$(uname -s)" = "FreeBSD" ]; then
    BEAR_PATH="bear"
else
    CROSS_COMPILE=1
    BEAR_PATH="$CHERI_ROOT/output/bootstrap/bin/bear"
fi

check_binary() {
    local binary_path="$1"
    local binary_name="$2"
    local build_target="$3"

    if [ -x "$binary_path" ]; then
        echo "Found $binary_name at $binary_path"
        return 0
    fi

    echo "Missing $binary_name at $binary_path"
    if [ -n "$build_target" ]; then
        echo "  -> will build with: cheribuild.py $build_target"
    fi
    return 1
}

check_directory() {
    local dir_path="$1"
    local dir_name="$2"
    local build_target="$3"

    if [ -d "$dir_path" ]; then
        echo "Found $dir_name at $dir_path"
        return 0
    fi

    echo "Missing $dir_name at $dir_path"
    if [ -n "$build_target" ]; then
        echo "  -> will build with: cheribuild.py $build_target"
    fi
    return 1
}

check_cheribsd_build() {
    local cheribsd_kernel="$CHERI_ROOT/output/rootfs-morello-purecap/boot/kernel/kernel"
    local cheribsd_headers="$CHERI_ROOT/output/rootfs-morello-purecap/usr/include/sys/types.h"

    if [ -f "$cheribsd_kernel" ] && [ -f "$cheribsd_headers" ]; then
        echo "CheriBSD appears to be built"
        return 0
    fi

    echo "CheriBSD not fully built"
    echo "  -> Missing: $([ ! -f "$cheribsd_kernel" ] && echo "kernel " || echo "")$([ ! -f "$cheribsd_headers" ] && echo "headers" || echo "")"
    echo "  -> Will build with: cheribuild.py cheribsd-morello-purecap"
    return 1
}

build_missing_deps(){
    local temp_file=$(mktemp)
    local cheribsd_found=true
    local llvm_found=true

    if ! check_binary "$CLANG_PATH" "Clang" "morello-llvm"; then
        llvm_found=false
    fi

    if ! check_binary "$LD_PATH" "LD.LLD" "morello-llvm"; then
        llvm_found=false
    fi

    if ! check_binary "$OBJCOPY_PATH" "Objcopy" "morello-llvm"; then
        llvm_found=false
    fi

    if [ "$llvm_found" = "false" ]; then
        echo "morello-llvm" >> "$temp_file"
    fi

    if ! check_binary "$BEAR_PATH" "Bear" "bear"; then
        echo "bear" >> "$temp_file"
    fi

    if ! check_cheribsd_build; then
        cheribsd_found=false
    fi

    if ! check_binary "$BMAKE_PATH" "Bmake"; then
        cheribsd_found=false
    fi

    if ! check_directory "$MAKEFILES_PATH" "CheriBSD makefiles"; then
        cheribsd_found=false
    fi

    if [ "$cheribsd_found" = "false" ]; then
        echo "cheribsd-morello-purecap" >> "$temp_file"
    fi

    if [ ! -s "$temp_file" ]; then
        echo "No missing dependencies"
        rm "$temp_file"
        return
    fi

    echo ""
    echo "Building missing dependencies (this may take a while)..."

    if ! check_directory "$CHERIBUILD_PATH" "Cheribuild"; then
        echo "Cheribuild not found. Fetching source"
        cd "$HOME"
        git clone https://github.com/CTSRD-CHERI/cheribuild.git || {
            echo "Error: failed to clone cheribuild"
            exit 1
        }
    fi

    cd "$CHERIBUILD_PATH"

    while IFS= read -r target; do
        echo "Building $target..."
        python3 cheribuild.py $target || {
            echo "Error: failed to build $target"
            exit 1
        }
    done < "$temp_file"

    rm "$temp_file"

    echo "Done building dependencies"
    cd -
}

help_output(){
    echo "Usage: $0 {build|install|help}"
    echo "  build   - build the module (just compiles without installation)"
    echo "  install - builds and installs the module (Only works on CheriBSD, requires root). Also installs the headers"
    echo "  install-headers - install just the header files"
    echo "  bear    - generate compilation database with Bear for clangd"
    echo "  help    - displays this output"
    echo "  clean   - remove build output files"
    echo ""
    echo "Arguments:"
    echo "  cheribsd_path - Path to CheriBSD source (default: \$HOME/projects/cheribsd). This needs to be the actual path for source. No symlinks as the generated location uses the real path."
    echo ""
    echo "Examples:"
    echo "  $0 build                        # Use default path"
    echo "  $0 build /home/user/cheribsd    # Use custom path"
    echo "  $0 install-headers              # Install headers only"
    echo "  $0 bear                         # Generate compile_commands.json"
    echo "  sudo $0 install                 # install with root"
}

if [ "$(uname -s)" = "Linux" ]; then
    build_missing_deps
fi

case "${1:-build}" in
    "build")
        if [ "$(uname -s)" = "FreeBSD" ]; then
            make CROSS_COMPILE=0
        else
            "$BMAKE_PATH" -m "$MAKEFILES_PATH" CROSS_COMPILE=1 CHERIBSD_SOURCE_PATH=$CHERIBSD_SOURCE_PATH AWK=/usr/bin/awk XARGS=${HOME}/cheri/build/cheribsd-morello-purecap-build/home/khacker/cheri/cheribsd/arm64.aarch64c/tmp/legacy/bin/xargs
	    sync
        fi
        echo "Build Complete: ${MODULE_NAME}.ko"
        ;;
    "clean")
	    rm -f *.ko *.kld *.o
        ;;
    "install-headers")
        if [ "$(uname -s)" = "FreeBSD" ]; then
            if [ "$(id -u)" -ne 0 ]; then
                echo "Warning: Header installation may require root privileges"
            fi
            make CROSS_COMPILE=0 install-headers
        else
            "$BMAKE_PATH" -m "$MAKEFILES_PATH" CROSS_COMPILE=1 CHERIBSD_SOURCE_PATH=$CHERIBSD_SOURCE_PATH install-headers
        fi
        echo "Headers installed successfully"
        ;;
    "bear")
        echo "Generating compilation database with Bear..."

        echo "Cleaning previous build..."
        if [ "$(uname -s)" = "FreeBSD" ]; then
            make CROSS_COMPILE=0 clean
        else
            "$BMAKE_PATH" -m "$MAKEFILES_PATH" CROSS_COMPILE=1 CHERIBSD_SOURCE_PATH=$CHERIBSD_SOURCE_PATH clean
        fi

        if [ "$(uname -s)" = "FreeBSD" ]; then
            "$BEAR_PATH" -- make CROSS_COMPILE=0
        else
            "$BEAR_PATH" -- "$BMAKE_PATH" -m "$MAKEFILES_PATH" CROSS_COMPILE=1 CHERIBSD_SOURCE_PATH=$CHERIBSD_SOURCE_PATH
        fi
        echo "Compilation database generated: compile_commands.json"
        ;;
    "install")
        if [ "$(uname -s)" != "FreeBSD" ]; then
            echo "Error: install only supported on CheriBSD"
            exit 1
        fi
        if [ "$(id -u)" -ne 0 ]; then
            echo "Error: install needs root privileges"
            exit 1
        fi

        make CROSS_COMPILE=0
        make CROSS_COMPILE=0 install-headers

        if kldstat | grep -q "$MODULE_NAME"; then
            echo "Unloading $MODULE_NAME..."
            kldunload "$MODULE_NAME" || true
        fi

        echo "Loading ${MODULE_NAME}.ko"
        kldload "./${MODULE_NAME}.ko"
        echo "Module loaded successfully"
        ;;
    "help")
        help_output
        ;;
    *)
        help_output
        exit 1
        ;;
esac
