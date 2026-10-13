# SPDX-License-Identifier: MPL-2.0
cmake_minimum_required(VERSION 3.14.0)
project(interrupt.library LANGUAGES C VERSION 1.13)

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(CPM-init)

get_verstring(VERSTRING)
string(REPLACE "${PROJECT_NAME}" "gic400.library" COMPATIBILITY_VERSTRING "${VERSTRING}")

if(NOT TARGET devicetree)
    CPMGetPackage(devicetree.resource)
endif()

# Set default install prefix to ./install subdirectory
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install" CACHE PATH "Install path" FORCE)
endif()

sfdc(sfd/interrupt.sfd)
target_include_directories(interrupt INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Native controller implementation belongs to this library. Keep discovery
# and register access testable without a private emulator service.
add_library(interrupt_controller STATIC
	src/devicetree_address.c
	src/devicetree_interrupt.c
    src/controller.c
    src/interrupt.c
    src/domain.c
    src/domain_tree.c
    src/resolve.c
    src/pending.c
    src/arrival.c
    src/dispatch.c
    src/vector.c
    src/message.c
    src/local_apic.c
    src/io_apic.c
)

target_link_libraries(interrupt_controller PUBLIC devicetree interrupt)
target_include_directories(interrupt_controller PUBLIC include include/private)
target_compile_options(interrupt_controller PRIVATE -O2 -m68040 -fomit-frame-pointer
    -ffreestanding -fno-builtin -fno-common -Wall -Wextra -Werror)

add_executable(interrupt.library
    src/main.c
    src/end.c
)

target_compile_definitions(interrupt.library PRIVATE
    VERSION_STRING="${VERSTRING}"
    COMPATIBILITY_VERSION_STRING="${COMPATIBILITY_VERSTRING}"
    LIBRARY_VERSION=${PROJECT_VERSION_MAJOR}
    LIBRARY_REVISION=${PROJECT_VERSION_MINOR}
    LIBRARY_NAME="${PROJECT_NAME}"
)
set_property(TARGET interrupt.library APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ldscript.lds")
add_dependencies(interrupt.library devicetree interrupt)
target_link_libraries(interrupt.library interrupt devicetree interrupt_controller)
target_link_options(interrupt.library PRIVATE -nostdlib -nostartfiles -Wall -Wextra -s
    "-Wl,-T${CMAKE_CURRENT_SOURCE_DIR}/ldscript.lds"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/interrupt.library.map")
target_compile_options(interrupt.library PRIVATE -Os -m68040 -fomit-frame-pointer
    -ffreestanding -fno-builtin -fno-common -Wall -Wextra -Werror)

check_rom(interrupt.library interrupt.library)
bin_to_header(interrupt.library)

# Set up information for Aminet
add_project_info(
    "interrupt.library for AmigaOS 3.x"
    "marlon@amigadev.com (Marlon Beijer)"
    "driver/other"
    "m68k-amigaos"
)

add_aminet_readme_generator(interrupt.library "interrupt.library.readme")

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/sfd/interrupt.sfd DESTINATION Developer/sfd)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/libraries DESTINATION Developer/include)
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/ DESTINATION Developer/include)
install(FILES LICENSE DESTINATION Developer)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/interrupt.library DESTINATION Libs)
