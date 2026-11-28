include(ExternalProject)
find_package(Git REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(OPENPS2BIOS_CLANG NAMES clang REQUIRED)
find_program(OPENPS2BIOS_LLD NAMES ld.lld REQUIRED)
find_program(OPENPS2BIOS_OBJCOPY NAMES llvm-objcopy REQUIRED)
find_program(OPENPS2BIOS_MAKE NAMES gmake make REQUIRED)

set(openps2bios_patch "${CMAKE_CURRENT_LIST_DIR}/patches/openps2bios-v07-disc-boot.patch")
set(openps2bios_output "${CMAKE_BINARY_DIR}/_deps/openps2bios-build")
ExternalProject_Add(openps2bios_rom
    PREFIX "${CMAKE_BINARY_DIR}/_deps/openps2bios"
    SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/openps2bios-src"
    GIT_REPOSITORY https://github.com/TamaribaEmu/OpenPS2BIOS.git
    GIT_TAG 893b9fa96678f89c8e56cde28ffda00330cf35b1
    UPDATE_DISCONNECTED TRUE
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE TRUE
    BUILD_COMMAND "${OPENPS2BIOS_MAKE}"
        "CC=${OPENPS2BIOS_CLANG}" "LD=${OPENPS2BIOS_LLD}"
        "OBJCOPY=${OPENPS2BIOS_OBJCOPY}" "PYTHON=${Python3_EXECUTABLE}"
        "BUILD=${openps2bios_output}"
    BUILD_BYPRODUCTS "${openps2bios_output}/OpenPS2BIOS.bin"
    INSTALL_COMMAND ""
    EXCLUDE_FROM_ALL TRUE
)

# Reapply from the pinned source in this managed dependency checkout.
# Depending on the patch file also rebuilds the ROM when the patch changes.
ExternalProject_Add_Step(openps2bios_rom apply_disc_boot_patch
    COMMAND "${GIT_EXECUTABLE}" restore --source=HEAD -- .
    COMMAND "${GIT_EXECUTABLE}" apply "${openps2bios_patch}"
    WORKING_DIRECTORY "<SOURCE_DIR>"
    DEPENDEES patch
    DEPENDERS configure
    DEPENDS "${openps2bios_patch}"
)
