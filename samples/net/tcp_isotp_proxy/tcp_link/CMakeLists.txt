# CMakeLists.txt for TCP Link Module
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tcp_link.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
