# TCP Command Handler
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tcp_cmd_handler.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
