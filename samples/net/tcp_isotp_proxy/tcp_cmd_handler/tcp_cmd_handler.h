/*
 * Copyright (c) 2025 N-Ix
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * TCP Command Handler
 * This header file defines the TCP command handler interface for handling commands over
 * TCP connections. It includes error codes, command types, and function prototypes for
 * initializing, registering, unregistering, and executing command handlers.
 * The command handler supports commands with names, names with values, and commands with data.
 * It also provides a mechanism for dispatching commands and handling errors.
 * The command handler is designed to be used in a Zephyr-based application.
 * It is intended for use in applications that require command handling over TCP connections,
 * such as networked devices or IoT applications.
 * The commands that can be handled include simple commands with names,
 * commands with names and integer values, and commands with data payloads.
 * The accepted command formats are:
 * - Name only: "cmd_name"
 * - Name and value: "cmd_name value"
 * - Data: "cmd_name len data" (Note: between "len" and "data" there must be just one space)
 * The command handler provides a flexible and extensible way to handle commands,
 * allowing for easy registration and execution of command handlers.
 * It also includes error handling to ensure that commands are processed correctly and that
 * errors are reported. The command handler is designed to be thread-safe and can be used
 * in a multi-threaded environment.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <zephyr/kernel.h>

/**
 * TCP Command Handler Error Codes
 */
typedef enum {
	TCP_CMD_ERR_OK = 0,            /* No error */
	TCP_CMD_ERR_NAME_EXISTS,       /* Command name already exists */
	TCP_CMD_ERR_NAME_NOT_FOUND,    /* Command name not found */
	TCP_CMD_ERR_TYPE_NOT_FOUND,    /* Command type not found */
	TCP_CMD_ERR_NO_SPACE,          /* No space available */
	TCP_CMD_ERR_HANDLER,           /* Handler error */
	TCP_CMD_ERR_NAME,              /* Name error */
	TCP_CMD_ERR_VALUE,             /* Value error */
	TCP_CMD_ERR_DATA,              /* Data error */
	TCP_CMD_ERR_DATA_LEN,          /* Data length error */
	TCP_CMD_ERR_INIT,              /* Initialization error */
	TCP_CMD_ERR_FORMAT,            /* Format error */
	TCP_CMD_ERR_EXECUTION          /* Execution error */
} tcp_cmd_err_t;

/**
 * TCP Command Handler Types
 */
typedef enum {
	TCP_CMD_NOT_ASSIGNED = 0,      /* Command handler not assigned */
	TCP_CMD_NAME_ONLY,             /* Command with name only */
	TCP_CMD_NAME_VALUE,            /* Command with name and value */
	TCP_CMD_DATA                   /* Command with data */
} tcp_cmd_type_t;

/**
 * TCP Command Handler Escape Sequence
 * This sequence is used to identify the end of a TCP command.
 */
#define TCP_CMD_HANDLER_ESC_SEQ      "\xAA\x55\x33\xCC"
#define TCP_CMD_HANDLER_ESC_SEQ_LEN  (sizeof(TCP_CMD_HANDLER_ESC_SEQ) - 1)

/**
 * TCP Command Handler Buffer Size
 * This defines the total buffer size required for the TCP command handler.
 */
#define TCP_CMD_HANDLER_BUFFER_SIZE  (CONFIG_TCP_CMD_HANDLER_DATA_MAX_LEN + \
					 CONFIG_TCP_CMD_HANDLER_NAME_MAX_LEN + \
				     CONFIG_TCP_CMD_HANDLER_VAL_MAX_LEN + \
					 CONFIG_TCP_CMD_HANDLER_SPACES_MAX_LEN + \
				     TCP_CMD_HANDLER_ESC_SEQ_LEN)

/*
 * Initializes the TCP Command Handler.
 * This function must be called before any other command handler functions.
 * @return TCP_CMD_ERR_OK on success, or an error code on failure.
 */
tcp_cmd_err_t tcp_cmd_handler_init(void);

/**
 * Deinitializes the TCP Command Handler.
 * This function clears all registered handlers and frees resources.
 * @return Returns TCP_CMD_ERR_OK on success, or an error code on failure.
 */
tcp_cmd_err_t tcp_cmd_handler_deinit(void);

/**
 * Dispatches a command to the TCP Command Handler.
 * The command must be in the format: <command_name> [<value>] [<data>]
 * @param cmd The command string to dispatch.
 * @param cmd_len The length of the command string.
 * The command must include the ESC sequence at the end.
 * The command can be of the following formats:
 * - Name only: "cmd_name" (e.g., "cmd_start")
 * - Name and value: "cmd_name value" (e.g., "cmd_set 42")
 * - Data: "cmd_name len data" (e.g., "cmd_send_data 11 Hello World")
 * @return Returns TCP_CMD_ERR_OK on success, or an error code on failure.
 */
tcp_cmd_err_t tcp_cmd_dispatch(const uint8_t *cmd, size_t cmd_len);

/**
 * Registers a callback for a command handler.
 * The command name must be unique and not already registered.
 * @param cmd_name The name of the command to register.
 * @param type The type of command handler (name only, name and value, or data).
 * @param handler The callback function to handle the command.
 * The handler function must match the type specified:
 * - For TCP_CMD_NAME_ONLY: tcp_cmd_err_t (*handler)(const char *cmd_name)
 * - For TCP_CMD_NAME_VALUE: tcp_cmd_err_t (*handler)(const char *cmd_name, int value)
 * - For TCP_CMD_DATA: tcp_cmd_err_t (*handler)(const char *cmd_name, const char *data, size_t len)
 * @return TCP_CMD_ERR_OK on success, or an error code on failure.
 */
tcp_cmd_err_t tcp_cmd_handler_register_cb(const char *cmd_name,
					  tcp_cmd_type_t type, void *handler);

/**
 * Unregisters a callback for a command handler.
 * The command name must be registered before unregistering.
 * @param cmd_name The name of the command to unregister.
 * @param type The type of command handler to unregister (name only, name and value, or data).
 * @return TCP_CMD_ERR_OK on success, or an error code on failure.
 */
tcp_cmd_err_t tcp_cmd_handler_unregister_cb(const char *cmd_name,
					    tcp_cmd_type_t type);

/**
 * Executes a command handler for a command with name only.
 * @param cmd_name The name of the command to execute.
 * The command name must be registered and the handler must be assigned.
 * @return TCP_CMD_ERR_OK on success, or an error code on failure.
 */
tcp_cmd_err_t tcp_cmd_handler_exec_name_cb(const char *cmd_name);

/**
 * Executes a command handler for a command with name and value.
 * @param cmd_name The name of the command to execute.
 * @param value The value associated with the command.
 * The command name must be registered and the handler must be assigned.
 * @return TCP_CMD_ERR_OK on success, or an error code on failure.
 */
tcp_cmd_err_t tcp_cmd_handler_exec_value_cb(const char *cmd_name, int value);

/**
 * Executes a command handler for a command with data.
 * @param cmd_name The name of the command to execute.
 * @param data The data associated with the command.
 * @param len The length of the data.
 * The command name must be registered and the handler must be assigned.
 * The data must be valid and within the maximum length.
 * @return TCP_CMD_ERR_OK on success, or an error code on failure.
 */
tcp_cmd_err_t tcp_cmd_handler_exec_data_cb(const char *cmd_name,
					   const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
