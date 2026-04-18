###############################################################################
# Copyright 2018-2024, Barcelona Supercomputing Center (BSC), Spain            #
# Copyright 2015-2024, Johannes Gutenberg Universitaet Mainz, Germany          #
#                                                                              #
# This software was partially supported by the                                 #
# EC H2020 funded project NEXTGenIO (Project ID: 671951, www.nextgenio.eu).    #
#                                                                              #
# This software was partially supported by the                                 #
# ADA-FS project under the SPPEXA project funded by the DFG.                   #
#                                                                              #
# This file is part of GekkoFS' POSIX interface.                               #
#                                                                              #
# GekkoFS' POSIX interface is free software: you can redistribute it and/or    #
# modify it under the terms of the GNU Lesser General Public License as        #
# published by the Free Software Foundation, either version 3 of the License,  #
# or (at your option) any later version.                                       #
#                                                                              #
# GekkoFS' POSIX interface is distributed in the hope that it will be useful,  #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU Lesser General Public License for more details.                          #
#                                                                              #
# You should have received a copy of the GNU Lesser General Public License     #
# along with GekkoFS' POSIX interface.  If not, see                            #
# <https://www.gnu.org/licenses/>.                                             #
#                                                                              #
# SPDX-License-Identifier: LGPL-3.0-or-later                                   #
################################################################################

# ##############################################################################
# This builds the `libgkfs_intercept.so` library: the primary GekkoFS client
# based on syscall interception.
# ##############################################################################
add_library(gkfs_intercept SHARED)
add_library(gkfs_user_lib SHARED)

target_sources(
  gkfs_intercept
  PRIVATE gkfs_functions.cpp
          hooks.cpp
          intercept.cpp
          logging.cpp
          malleability_logger.cpp
          open_file_map.cpp
          open_dir.cpp
          path.cpp
          preload.cpp
          preload_context.cpp
          preload_util.cpp
          rpc/rpc_types.cpp
          rpc/forward_data.cpp
          rpc/forward_management.cpp
          rpc/forward_metadata.cpp
          syscalls/detail/syscall_info.c
)


target_sources(
    gkfs_user_lib
    PRIVATE gkfs_functions.cpp
    intercept.cpp
    hooks.cpp
    logging.cpp
    malleability_logger.cpp
    open_file_map.cpp
    open_dir.cpp
    path.cpp
    preload.cpp
    preload_context.cpp
    preload_util.cpp
    rpc/rpc_types.cpp
    rpc/forward_data.cpp
    rpc/forward_management.cpp
    rpc/forward_metadata.cpp
    rpc/forward_stage.cpp
    syscalls/detail/syscall_info.c 
)

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    target_sources(gkfs_user_lib PRIVATE syscalls/util_x86.S)
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    target_sources(gkfs_user_lib PRIVATE syscalls/util_arm64.S)
else()
    message(FATAL_ERROR "Unsupported architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

target_compile_definitions(gkfs_user_lib PUBLIC BYPASS_SYSCALL)
target_link_options(gkfs_user_lib PRIVATE -z noexecstack)

target_link_libraries(
  gkfs_intercept
  PRIVATE metadata distributor env_util arithmetic path_util rpc_utils
          malleability_logger
  PUBLIC Syscall_intercept::Syscall_intercept
         dl
         Mercury::Mercury
         hermes
         fmt::fmt
         Threads::Threads
)

install(
  TARGETS gkfs_intercept
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gkfs
)

target_link_libraries(
    gkfs_user_lib
    PRIVATE metadata distributor env_util arithmetic path_util rpc_utils
            malleability_logger
    PUBLIC dl
    Mercury::Mercury
    hermes
    fmt::fmt
    Threads::Threads
    Syscall_intercept::Syscall_intercept
)

set_target_properties(gkfs_user_lib
    PROPERTIES
    PUBLIC_HEADER "../../include/client/void_syscall_intercept.hpp"
    PUBLIC_HEADER "../../include/client/user_functions.hpp"
)

install(
    TARGETS gkfs_user_lib
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gkfs
)

# ##############################################################################
# This builds the `libgkfwd_intercept.so` library: the client for GekkoFS'
# forwarding mode based on syscall interception.
# ##############################################################################

if(GKFS_ENABLE_FORWARDING)
  add_library(gkfwd_intercept SHARED)

  target_sources(
    gkfwd_intercept
    PRIVATE gkfs_functions.cpp
            hooks.cpp
            intercept.cpp
            logging.cpp
            malleability_logger.cpp
            open_file_map.cpp
            open_dir.cpp
            path.cpp
            preload.cpp
            preload_context.cpp
            preload_util.cpp
            rpc/rpc_types.cpp
            rpc/forward_data.cpp
            rpc/forward_management.cpp
            rpc/forward_metadata.cpp
            syscalls/detail/syscall_info.c
  )
  target_compile_definitions(gkfwd_intercept PUBLIC GKFS_ENABLE_FORWARDING)

  if(GKFS_ENABLE_AGIOS)
    target_compile_definitions(gkfwd_intercept PUBLIC GKFS_ENABLE_AGIOS)
  endif()

  target_link_libraries(
    gkfwd_intercept
    PRIVATE metadata distributor env_util arithmetic path_util rpc_utils
            malleability_logger
    PUBLIC Syscall_intercept::Syscall_intercept
           dl
           Mercury::Mercury
           hermes
           fmt::fmt
           Threads::Threads
  )

  install(
    TARGETS gkfwd_intercept
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gkfs
  )
endif()
