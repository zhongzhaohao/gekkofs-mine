set(CMAKE_CXX_STANDARD 17)
add_executable(gkfs_register)

target_sources(gkfs_register
  PRIVATE register.cpp
          register_util.cpp
)

target_link_libraries(gkfs_register
  PRIVATE gkfs_user_lib
          CLI11::CLI11
)

install(TARGETS gkfs_register RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})