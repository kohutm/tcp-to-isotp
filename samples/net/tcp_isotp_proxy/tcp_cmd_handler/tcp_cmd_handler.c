/*
 * Copyright (c) 2025 N-Ix
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <stdlib.h>
#include <zephyr/logging/log.h>
#include "tcp_cmd_handler.h"
#include <string.h>
#include <ctype.h>

/* Logging configuration */
LOG_MODULE_REGISTER(tcp_cmd_handler, CONFIG_TCP_CMD_HANDLER_LOG_LEVEL);

/**
 * TCP Command Handler Callback Types by Command Type
 * These callbacks are used to handle different types of commands.
 * - cmd_handler_name_cb: For commands with name only.
 * - cmd_handler_value_cb: For commands with name and value.
 * - cmd_handler_data_cb: For commands with data.
 */
typedef tcp_cmd_err_t (*cmd_handler_name_cb)(const char *cmd);
typedef tcp_cmd_err_t (*cmd_handler_value_cb)(const char *cmd, int value);
typedef tcp_cmd_err_t (*cmd_handler_data_cb)(const char *cmd, const uint8_t *data, size_t len);

/**
 * TCP Command Handlers Structures
 */
typedef struct {
	char cmd_name[CONFIG_TCP_CMD_HANDLER_NAME_MAX_LEN + 1];  /* Command name */
	cmd_handler_name_cb handler_cb;       /* Callback for name-only commands */
	bool assigned;                        /* Flag to check if the handler is assigned */
} tcp_cmd_name_handler_t;

typedef struct {
	char cmd_name[CONFIG_TCP_CMD_HANDLER_NAME_MAX_LEN + 1];  /* Command name */
	cmd_handler_value_cb handler_cb;      /* Callback for name-value commands */
	bool assigned;                        /* Flag to check if the handler is assigned */
} tcp_cmd_value_handler_t;

typedef struct {
	char cmd_name[CONFIG_TCP_CMD_HANDLER_NAME_MAX_LEN + 1];  /* Command name */
	cmd_handler_data_cb handler_cb;       /* Callback for data commands */
	bool assigned;                        /* Flag to check if the handler is assigned */
} tcp_cmd_data_handler_t;

/**
 * TCP Command Handler Collection
 * This structure holds all command handlers for different types of commands.
 */
typedef struct {
	tcp_cmd_name_handler_t name_handler[CONFIG_TCP_CMD_HANDLER_NAME_QUANT];
	tcp_cmd_value_handler_t value_handler[CONFIG_TCP_CMD_HANDLER_VAL_QUANT];
	tcp_cmd_data_handler_t data_handler[CONFIG_TCP_CMD_HANDLER_DATA_QUANT];
} tcp_cmd_handlers_t;

/**
 * TCP Command Parser Structure
 * This structure is used to parse incoming commands and store their details.
 */
typedef struct tcp_cmd_parser {
	tcp_cmd_type_t cmd_type;                 /* Type of command being parsed */
	int32_t value_int;                       /* Integer value for commands with value */
	size_t raw_data_len;                     /* Length of the raw command data */
	size_t cmd_tot_len;                      /* Total length of the command */
	size_t cmd_idx;                          /* Current index in the command data */
	size_t cmd_spaces;                       /* Number of spaces in the command */
	size_t cmd_name_len;                     /* Length of the command name */
	size_t cmd_val_len;                      /* Length of the command value */
	size_t cmd_data_len;                     /* Length of the command data */
	/* Buffers for command parsing */
	uint8_t cmd_data[CONFIG_TCP_CMD_HANDLER_DATA_MAX_LEN];   /* Buffer for command data */
	char cmd_name[CONFIG_TCP_CMD_HANDLER_NAME_MAX_LEN + 1];  /* +1 for null terminator */
	char cmd_val[CONFIG_TCP_CMD_HANDLER_VAL_MAX_LEN + 1];    /* +1 for null terminator */
} tcp_cmd_parser_t;

/**
 * TCP Command Handler Structure
 * This structure holds the command handlers and the parser.
 */
typedef struct {
	tcp_cmd_handlers_t handlers;  /* Collection of command handlers */
	tcp_cmd_parser_t parser;      /* Command parser */
	bool is_initialized;          /* Flag to check if the handler is initialized */
} tcp_cmd_handler_t;

/* Global variable pointer for the TCP Command Handler */
static tcp_cmd_handler_t *tcp_cmd_handler;

/**
 * Converts a TCP command error code to a string.
 * @param err The error code to convert.
 * @return A string representation of the error code.
 */
static const char *s_cmd_err_to_string(tcp_cmd_err_t err)
{
	switch (err) {
	case TCP_CMD_ERR_OK:
		return "OK";
	case TCP_CMD_ERR_NAME_EXISTS:
		return "Name Exists";
	case TCP_CMD_ERR_NAME_NOT_FOUND:
		return "Name Not Found";
	case TCP_CMD_ERR_TYPE_NOT_FOUND:
		return "Type Not Found";
	case TCP_CMD_ERR_NO_SPACE:
		return "No Space";
	case TCP_CMD_ERR_HANDLER:
		return "Handler Error";
	case TCP_CMD_ERR_NAME:
		return "Name Error";
	case TCP_CMD_ERR_VALUE:
		return "Value Error";
	case TCP_CMD_ERR_DATA:
		return "Data Error";
	case TCP_CMD_ERR_DATA_LEN:
		return "Data Length Error";
	case TCP_CMD_ERR_INIT:
		return "Initialization Error";
	case TCP_CMD_ERR_FORMAT:
		return "Format Error";
	case TCP_CMD_ERR_EXECUTION:
		return "Execution Error";
	default:
		return "Unknown Error";
	}
}

/**
 * Checks if a character is valid for command names.
 * @param c The character to check.
 * @return True if the character is valid, false otherwise.
 */
static inline bool s_is_valid_cmd_char(char c)
{
	return isalnum(c) || (c == '_') || (c == '-');
}

/**
 * Checks if a character is valid for command values.
 * Valid characters are digits and the minus sign.
 * @param c The character to check.
 * @return True if the character is valid, false otherwise.
 */
static inline bool s_is_valid_value_char(char c)
{
	return isdigit(c) || (c == '-');
}

/**
 * Checks if a character is valid for command data.
 * Valid characters are alphanumeric, space, and some special characters.
 * @param c The character to check.
 * @return True if the character is valid, false otherwise.
 */
static inline bool s_is_space_overflow(const tcp_cmd_parser_t *parser)
{
	return parser->cmd_spaces >= CONFIG_TCP_CMD_HANDLER_SPACES_MAX_LEN;
}

/**
 * Checks if the command name, value, or data length exceeds the maximum allowed.
 * @param parser Pointer to the command parser.
 * @return True if overflow occurs, false otherwise.
 */
static inline bool s_is_cmd_name_overflow(const tcp_cmd_parser_t *parser)
{
	return parser->cmd_name_len >= CONFIG_TCP_CMD_HANDLER_NAME_MAX_LEN;
}

/* * Checks if the command value length exceeds the maximum allowed.
 * @param parser Pointer to the command parser.
 * @return True if overflow occurs, false otherwise.
 */
static inline bool s_is_cmd_val_overflow(const tcp_cmd_parser_t *parser)
{
	return parser->cmd_val_len >= CONFIG_TCP_CMD_HANDLER_VAL_MAX_LEN;
}

/**
 * Checks if the command data length exceeds the maximum allowed.
 * @param parser Pointer to the command parser.
 * @return True if overflow occurs, false otherwise.
 */
static inline bool s_is_cmd_data_overflow(const tcp_cmd_parser_t *parser)
{
	return parser->cmd_data_len >= CONFIG_TCP_CMD_HANDLER_DATA_MAX_LEN;
}

/**
 * Checks if the command parser is ready to parse a new command.
 * @param parser Pointer to the command parser.
 * @return True if the parser is ready, false otherwise.
 */
static inline bool s_detect_esc_seq(const char *cmd, tcp_cmd_parser_t *parser)
{
	/* Check if the command ends with the escape sequence */
	return parser->cmd_tot_len - parser->cmd_idx == TCP_CMD_HANDLER_ESC_SEQ_LEN &&
		   memcmp(cmd + parser->cmd_idx, TCP_CMD_HANDLER_ESC_SEQ,
			  TCP_CMD_HANDLER_ESC_SEQ_LEN) == 0;
}

/**
 * Checks if the TCP Command Handler is initialized.
 * @param handler Pointer to the TCP Command Handler structure.
 * @return TCP_CMD_ERR_OK if initialized, or an error code if not.
 */
static inline tcp_cmd_err_t s_tcp_cmd_handler_check_init(const tcp_cmd_handler_t *handler)
{
	if (!handler || !handler->is_initialized) {
		LOG_ERR("TCP Command Handler not initialized: %s",
			s_cmd_err_to_string(TCP_CMD_ERR_INIT));
		return TCP_CMD_ERR_INIT;
	}

	return TCP_CMD_ERR_OK;
}

/**
 * Checks if the command name is valid.
 * @param cmd_name The command name to check.
 * @return TCP_CMD_ERR_OK if valid, or an error code if invalid.
 */
static inline tcp_cmd_err_t s_tcp_cmd_handler_check_name(const char *cmd_name)
{
	if (!cmd_name || strlen(cmd_name) == 0 ||
	    strlen(cmd_name) > CONFIG_TCP_CMD_HANDLER_NAME_MAX_LEN) {
		LOG_ERR("Command name cannot be NULL or empty: %s",
			s_cmd_err_to_string(TCP_CMD_ERR_NAME));
		return TCP_CMD_ERR_NAME;
	}

	return TCP_CMD_ERR_OK;
}

/**
 * Checks if the command handler callback is valid.
 * @param callback The command handler callback to check.
 * @return TCP_CMD_ERR_OK if valid, or an error code if invalid.
 */
static inline tcp_cmd_err_t s_tcp_cmd_handler_check_cb(const void *callback)
{
	if (!callback) {
		LOG_ERR("Invalid handler cb: %s",
			s_cmd_err_to_string(TCP_CMD_ERR_HANDLER));
		return TCP_CMD_ERR_HANDLER;
	}

	return TCP_CMD_ERR_OK;
}

/**
 * TCP Command Parser Clear Function
 * This function clears the command parser state.
 * @param parser Pointer to the command parser to clear.
 */
static void s_tcp_cmd_parser_clear(tcp_cmd_parser_t *parser)
{
	parser->cmd_type = TCP_CMD_NOT_ASSIGNED;
	parser->cmd_data[0] = '\0';
	parser->cmd_name[0] = '\0';
	parser->cmd_val[0] = '\0';
	parser->value_int = 0;
	parser->cmd_idx = 0;
	parser->cmd_spaces = 0;
	parser->cmd_name_len = 0;
	parser->cmd_val_len = 0;
	parser->cmd_data_len = 0;
	parser->raw_data_len = 0;
	parser->cmd_tot_len = 0;
}

/**
 * Gets a free command handler for a specific command type.
 * @param type The type of command to get a handler for.
 * @return A pointer to the free command handler, or NULL if none is available.
 */
static void *s_get_free_cmd_handler(tcp_cmd_type_t type)
{
	switch (type) {
	case TCP_CMD_NAME_ONLY: {
		tcp_cmd_name_handler_t *name_handler = tcp_cmd_handler->handlers.name_handler;

		/* Find the first free name handler */
		for (size_t i = 0; i < CONFIG_TCP_CMD_HANDLER_NAME_QUANT; i++) {
			if (!name_handler[i].assigned) {
				return &name_handler[i];
			}
		}
		break;
	}
	case TCP_CMD_NAME_VALUE: {
		tcp_cmd_value_handler_t *value_handler = tcp_cmd_handler->handlers.value_handler;

		/* Find the first free value handler */
		for (size_t i = 0; i < CONFIG_TCP_CMD_HANDLER_VAL_QUANT; i++) {
			if (!value_handler[i].assigned) {
				return &value_handler[i];
			}
		}
		break;
	}
	case TCP_CMD_DATA: {
		tcp_cmd_data_handler_t *data_handler = tcp_cmd_handler->handlers.data_handler;

		/* Find the first free data handler */
		for (size_t i = 0; i < CONFIG_TCP_CMD_HANDLER_DATA_QUANT; i++) {
			if (!data_handler[i].assigned) {
				return &data_handler[i];
			}
		}
		break;
	}
	default:
		break;
	}

	return NULL; /* No free handler found */
}

/**
 * Checks if a command name is already assigned to a handler.
 * @param cmd_name The command name to check.
 * @param type The type of command to check.
 * @return TCP_CMD_ERR_OK if the name is free, or an error code if it exists.
 */
static tcp_cmd_err_t s_check_name_handler_is_free(const char *cmd_name, tcp_cmd_type_t type)
{
	switch (type) {
	case TCP_CMD_NAME_ONLY: {
		tcp_cmd_name_handler_t *name_handler = tcp_cmd_handler->handlers.name_handler;

		for (size_t i = 0; i < CONFIG_TCP_CMD_HANDLER_NAME_QUANT; i++) {
			if (name_handler[i].assigned &&
			    strcmp(name_handler[i].cmd_name, cmd_name) == 0) {
				return TCP_CMD_ERR_NAME_EXISTS;
			}
		}
		break;
	}
	case TCP_CMD_NAME_VALUE: {
		tcp_cmd_value_handler_t *value_handler = tcp_cmd_handler->handlers.value_handler;

		for (size_t i = 0; i < CONFIG_TCP_CMD_HANDLER_VAL_QUANT; i++) {
			if (value_handler[i].assigned &&
			    strcmp(value_handler[i].cmd_name, cmd_name) == 0) {
				return TCP_CMD_ERR_NAME_EXISTS;
			}
		}
		break;
	}
	case TCP_CMD_DATA: {
		tcp_cmd_data_handler_t *data_handler = tcp_cmd_handler->handlers.data_handler;

		for (size_t i = 0; i < CONFIG_TCP_CMD_HANDLER_DATA_QUANT; i++) {
			if (data_handler[i].assigned &&
			    strcmp(data_handler[i].cmd_name, cmd_name) == 0) {
				return TCP_CMD_ERR_NAME_EXISTS;
			}
		}
		break;
	}
	default:
		break;
	}

	return TCP_CMD_ERR_OK;
}

/**
 * Gets a command handler by name.
 * @param cmd_name The name of the command to find.
 * @param type The type of command to find.
 * @return A pointer to the command handler, or NULL if not found.
 */
static void *s_get_cmd_handler_by_name(const char *cmd_name, tcp_cmd_type_t type)
{
	switch (type) {
	case TCP_CMD_NAME_ONLY: {
		tcp_cmd_name_handler_t *name_handler = tcp_cmd_handler->handlers.name_handler;

		for (size_t i = 0; i < CONFIG_TCP_CMD_HANDLER_NAME_QUANT; i++) {
			if (name_handler[i].assigned &&
			    strcmp(name_handler[i].cmd_name, cmd_name) == 0) {
				return &name_handler[i];
			}
		}
		break;
	}
	case TCP_CMD_NAME_VALUE: {
		tcp_cmd_value_handler_t *value_handler = tcp_cmd_handler->handlers.value_handler;

		for (size_t i = 0; i < CONFIG_TCP_CMD_HANDLER_VAL_QUANT; i++) {
			if (value_handler[i].assigned &&
			    strcmp(value_handler[i].cmd_name, cmd_name) == 0) {
				return &value_handler[i];
			}
		}
		break;
	}
	case TCP_CMD_DATA: {
		tcp_cmd_data_handler_t *data_handler = tcp_cmd_handler->handlers.data_handler;

		for (size_t i = 0; i < CONFIG_TCP_CMD_HANDLER_DATA_QUANT; i++) {
			if (data_handler[i].assigned &&
			    strcmp(data_handler[i].cmd_name, cmd_name) == 0) {
				return &data_handler[i];
			}
		}
		break;
	}
	default:
		break;
	}

	return NULL; /* Handler not found */
}

/**
 * Clears a command handler based on its type.
 * @param handler Pointer to the command handler to clear.
 * @param type The type of command handler to clear.
 */
static void s_clear_cmd_handler(void *handler, tcp_cmd_type_t type)
{
	switch (type) {
	case TCP_CMD_NAME_ONLY: {
		tcp_cmd_name_handler_t *name_handler = (tcp_cmd_name_handler_t *)handler;

		name_handler->assigned = false;
		name_handler->handler_cb = NULL;
		name_handler->cmd_name[0] = '\0';
		break;
	}
	case TCP_CMD_NAME_VALUE: {
		tcp_cmd_value_handler_t *value_handler = (tcp_cmd_value_handler_t *)handler;

		value_handler->assigned = false;
		value_handler->handler_cb = NULL;
		value_handler->cmd_name[0] = '\0';
		break;
	}
	case TCP_CMD_DATA: {
		tcp_cmd_data_handler_t *data_handler = (tcp_cmd_data_handler_t *)handler;

		data_handler->assigned = false;
		data_handler->handler_cb = NULL;
		data_handler->cmd_name[0] = '\0';
		break;
	}
	default:
		break;
	}
}

/**
 * Skips spaces in the command string and updates the parser state.
 * @param cmd The command string to parse.
 * @param out Pointer to the command parser structure to update.
 * @return TCP_CMD_ERR_OK on success, or an error code if overflow occurs.
 */
static tcp_cmd_err_t s_parser_skip_spaces(const char *cmd, tcp_cmd_parser_t *out)
{
	while (out->cmd_idx < out->raw_data_len && isspace((unsigned char)cmd[out->cmd_idx])) {
		if (s_is_space_overflow(out)) {
			LOG_ERR("Command spaces overflow: %s",
				s_cmd_err_to_string(TCP_CMD_ERR_FORMAT));
			return TCP_CMD_ERR_FORMAT;
		}
		out->cmd_spaces++;
		out->cmd_idx++;
	}

	return TCP_CMD_ERR_OK;
}

/**
 * Parses the command name from the command string.
 * @param cmd The command string to parse.
 * @param out Pointer to the command parser structure to update.
 * @return TCP_CMD_ERR_OK on success, or an error code if parsing fails.
 */
static tcp_cmd_err_t s_tcp_cmd_parse_name(const char *cmd, tcp_cmd_parser_t *out)
{
	tcp_cmd_err_t err = TCP_CMD_ERR_OK;

	/* Skip leading spaces */
	err = s_parser_skip_spaces(cmd, out);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	/* find the command name */
	while (out->cmd_idx < out->raw_data_len && s_is_valid_cmd_char(cmd[out->cmd_idx])) {
		out->cmd_name[out->cmd_name_len++] = cmd[out->cmd_idx++];
		if (s_is_cmd_name_overflow(out)) {
			err = TCP_CMD_ERR_NAME;
			LOG_ERR("Command name overflow: %s", s_cmd_err_to_string(err));
			return err;
		}
	}

	out->cmd_name[out->cmd_name_len] = '\0';

	if (out->cmd_name_len < CONFIG_TCP_CMD_HANDLER_NAME_MIN_LEN) {
		err = TCP_CMD_ERR_NAME;
		LOG_ERR("Command name too short: %s", s_cmd_err_to_string(err));
		return err;
	}

	/* Skip spaces after command name */
	err = s_parser_skip_spaces(cmd, out);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	/* Check for escape sequence at the end */
	if (out->raw_data_len == out->cmd_idx) {
		if (s_detect_esc_seq(cmd, out)) {
			/* If the command ends with the escape sequence, it is a valid command */
			err = TCP_CMD_ERR_OK;
			out->cmd_type = TCP_CMD_NAME_ONLY;
			LOG_INF("Parsed command name: %s", out->cmd_name);
		} else {
			err = TCP_CMD_ERR_FORMAT;
			LOG_ERR("Command format error: %s", s_cmd_err_to_string(err));
		}
	}

	return err;
}

/**
 * Parses the command value from the command string.
 * The value can be a number or a string, depending on the command type.
 * @param cmd The command string to parse.
 * @param out Pointer to the command parser structure to update.
 * @return TCP_CMD_ERR_OK on success, or an error code if parsing fails.
 */
static tcp_cmd_err_t s_tcp_cmd_parse_value(const char *cmd, tcp_cmd_parser_t *out)
{
	tcp_cmd_err_t err = TCP_CMD_ERR_OK;

	/* find the command value */
	bool first_val_char = true;

	while (out->cmd_idx < out->raw_data_len) {
		if (first_val_char && s_is_valid_value_char(cmd[out->cmd_idx])) {
			first_val_char = false;
			out->cmd_val[out->cmd_val_len++] = cmd[out->cmd_idx++];
		} else if (isdigit(cmd[out->cmd_idx])) {
			out->cmd_val[out->cmd_val_len++] = cmd[out->cmd_idx++];
		} else {
			/* If we encounter a non-digit character, we stop parsing the value */
			break;
		}
		if (s_is_cmd_val_overflow(out)) {
			err = TCP_CMD_ERR_VALUE;
			LOG_ERR("Command value overflow: %s", s_cmd_err_to_string(err));
			return err;
		}
	}

	if (out->cmd_val_len >= CONFIG_TCP_CMD_HANDLER_VAL_MIN_LEN) {
		out->cmd_val[out->cmd_val_len] = '\0';
		/* Convert the command value to integer */
		out->value_int = atoi(out->cmd_val);
	} else {
		err = TCP_CMD_ERR_VALUE;
		LOG_ERR("Command value is required: %s", s_cmd_err_to_string(err));
		return err;
	}

	/* Check if there is space after the value */
	if (out->cmd_idx < out->raw_data_len && isspace((unsigned char)cmd[out->cmd_idx])) {
		if (s_is_space_overflow(out)) {
			err = TCP_CMD_ERR_FORMAT;
			LOG_ERR("Command spaces overflow: %s", s_cmd_err_to_string(err));
			return err;
		}
		out->cmd_idx++;
		out->cmd_spaces++;
	}

	if (out->cmd_idx == out->raw_data_len && s_detect_esc_seq(cmd, out)) {
		/* If the command ends with the escape sequence, it is a valid command */
		out->cmd_type = TCP_CMD_NAME_VALUE;
		LOG_INF("Parsed command name: %s, value: %d", out->cmd_name, out->value_int);
		return TCP_CMD_ERR_OK;
	}

	return err;
}

/**
 * Parses the command data from the command string.
 * The data is expected to be in a specific format after the command name and value.
 * @param cmd The command string to parse.
 * @param out Pointer to the command parser structure to update.
 * @return TCP_CMD_ERR_OK on success, or an error code if parsing fails.
 */
static tcp_cmd_err_t s_tcp_cmd_parse_data(const char *cmd, tcp_cmd_parser_t *out)
{
	tcp_cmd_err_t err = TCP_CMD_ERR_OK;

	/* Check if there is some data for the command data */
	if (out->raw_data_len <= out->cmd_idx || out->value_int <= 0 ||
	    out->raw_data_len - out->cmd_idx != out->value_int) {
		err = TCP_CMD_ERR_DATA_LEN;
		LOG_ERR("Command data length error: %s", s_cmd_err_to_string(err));
		return err;
	}

	/* Now we can parse the command data */
	size_t data_idx = 0;

	while (out->cmd_idx < out->raw_data_len) {
		if (s_is_cmd_data_overflow(out)) {
			err = TCP_CMD_ERR_DATA_LEN;
			LOG_ERR("Command data overflow: %s", s_cmd_err_to_string(err));
			return err;
		}
		/* Copy next byte to the command data buffer */
		out->cmd_data[data_idx++] = cmd[out->cmd_idx++];
	}

	if (data_idx == 0 || !s_detect_esc_seq(cmd, out)) {
		err = TCP_CMD_ERR_FORMAT;
		LOG_ERR("Command format error: %s", s_cmd_err_to_string(err));
		return err;
	}

	/* Check received Data length */
	if (data_idx != out->value_int) {
		err = TCP_CMD_ERR_DATA_LEN;
		LOG_ERR("Command data length error: %s", s_cmd_err_to_string(err));
	} else {
		out->cmd_type = TCP_CMD_DATA;
		out->cmd_data_len = data_idx;
		LOG_INF("Parsed command name: %s, data length: %zu",
			out->cmd_name, out->cmd_data_len);
	}

	return err;
}

/**
 * Parses a TCP command string into its components: name, value, and data.
 * @param cmd The command string to parse.
 * @param cmd_len The length of the command string.
 * @param out Pointer to the command parser structure to fill with parsed data.
 * @return TCP_CMD_ERR_OK on success, or an error code if parsing fails.
 */
static tcp_cmd_err_t s_tcp_cmd_parse(const char *cmd, size_t cmd_len, tcp_cmd_parser_t *out)
{
	tcp_cmd_err_t err = TCP_CMD_ERR_OK;

	out->cmd_tot_len = cmd_len;
	out->raw_data_len = cmd_len - TCP_CMD_HANDLER_ESC_SEQ_LEN;

	err = s_tcp_cmd_parse_name(cmd, out);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	if (out->cmd_type == TCP_CMD_NAME_ONLY) {
		/* If the command is only a name, we can return here */
		return TCP_CMD_ERR_OK;
	}

	/* If the command is not just a name, we need to parse the value */
	err = s_tcp_cmd_parse_value(cmd, out);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	/* If the command is name and value, we can return here */
	if (out->cmd_type == TCP_CMD_NAME_VALUE) {
		return TCP_CMD_ERR_OK;
	}

	/* If the command is not just a name and value, we need to parse the data */
	err = s_tcp_cmd_parse_data(cmd, out);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	if (out->cmd_type != TCP_CMD_DATA) {
		/* If the data command is not recognized, set an error */
		err = TCP_CMD_ERR_FORMAT;
		LOG_ERR("Command format error: %s", s_cmd_err_to_string(err));
	}

	return err;
}

/**
 * Public API Functions
 */
tcp_cmd_err_t tcp_cmd_handler_init(void)
{
	tcp_cmd_err_t err = TCP_CMD_ERR_OK;

	if (tcp_cmd_handler && tcp_cmd_handler->is_initialized) {
		LOG_INF("TCP Command Handler is already initialized");
		return err;
	}

	static tcp_cmd_handler_t handler_instance = {0};

	tcp_cmd_handler = &handler_instance;
	tcp_cmd_handler->is_initialized = true;
		LOG_INF("TCP Command Handler initialized");

	return err;
}

tcp_cmd_err_t tcp_cmd_handler_deinit(void)
{
	tcp_cmd_err_t err = TCP_CMD_ERR_OK;

	err = s_tcp_cmd_handler_check_init(tcp_cmd_handler);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	/* Clear all handlers */
	for (int i = 0; i < CONFIG_TCP_CMD_HANDLER_NAME_QUANT; i++) {
		tcp_cmd_handler->handlers.name_handler[i].assigned = false;
		tcp_cmd_handler->handlers.name_handler[i].handler_cb = NULL;
		tcp_cmd_handler->handlers.name_handler[i].cmd_name[0] = '\0';
	}

	for (int i = 0; i < CONFIG_TCP_CMD_HANDLER_VAL_QUANT; i++) {
		tcp_cmd_handler->handlers.value_handler[i].assigned = false;
		tcp_cmd_handler->handlers.value_handler[i].handler_cb = NULL;
		tcp_cmd_handler->handlers.value_handler[i].cmd_name[0] = '\0';
	}

	for (int i = 0; i < CONFIG_TCP_CMD_HANDLER_DATA_QUANT; i++) {
		tcp_cmd_handler->handlers.data_handler[i].assigned = false;
		tcp_cmd_handler->handlers.data_handler[i].handler_cb = NULL;
		tcp_cmd_handler->handlers.data_handler[i].cmd_name[0] = '\0';
	}

	/* Clear parser */
	s_tcp_cmd_parser_clear(&tcp_cmd_handler->parser);

	tcp_cmd_handler->is_initialized = false;
	tcp_cmd_handler = NULL;
	LOG_INF("TCP Command Handler deinitialized");

	return err;
}

tcp_cmd_err_t tcp_cmd_handler_register_cb(const char *cmd_name,
					  tcp_cmd_type_t type, void *handler_cb)
{
	tcp_cmd_err_t err = TCP_CMD_ERR_OK;

	err = s_tcp_cmd_handler_check_init(tcp_cmd_handler);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	err = s_tcp_cmd_handler_check_cb(handler_cb);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	err = s_tcp_cmd_handler_check_name(cmd_name);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	switch (type) {
	case TCP_CMD_NAME_ONLY:
		err = s_check_name_handler_is_free(cmd_name, TCP_CMD_NAME_ONLY);
		if (err != TCP_CMD_ERR_OK) {
			LOG_ERR("Command name already exists: %s", s_cmd_err_to_string(err));
			return err;
		}

		tcp_cmd_name_handler_t *name_handler = NULL;

		name_handler = (tcp_cmd_name_handler_t *)s_get_free_cmd_handler(TCP_CMD_NAME_ONLY);

		if (!name_handler) {
			err = TCP_CMD_ERR_NO_SPACE;
			LOG_ERR("No free name handler found: %s",
				s_cmd_err_to_string(err));
			return err;
		}

		name_handler->assigned = true;
		name_handler->handler_cb = (cmd_handler_name_cb)handler_cb;
		snprintf(name_handler->cmd_name, sizeof(name_handler->cmd_name),
			 "%s", cmd_name);
		LOG_INF("Registered name handler for command: %s", cmd_name);
		break;

	case TCP_CMD_NAME_VALUE:
		err = s_check_name_handler_is_free(cmd_name, TCP_CMD_NAME_VALUE);
		if (err != TCP_CMD_ERR_OK) {
			LOG_ERR("Command name already exists: %s", s_cmd_err_to_string(err));
			return err;
		}

		tcp_cmd_value_handler_t *value_handler = NULL;

		value_handler =
		(tcp_cmd_value_handler_t *)s_get_free_cmd_handler(TCP_CMD_NAME_VALUE);

		if (!value_handler) {
			err = TCP_CMD_ERR_NO_SPACE;
			LOG_ERR("No free value handler found: %s",
				s_cmd_err_to_string(err));
			return err;
		}
			value_handler->assigned = true;
			value_handler->handler_cb = (cmd_handler_value_cb)handler_cb;
			snprintf(value_handler->cmd_name, sizeof(value_handler->cmd_name),
				 "%s", cmd_name);
			LOG_INF("Registered value handler for command: %s", cmd_name);
		break;

	case TCP_CMD_DATA:
		err = s_check_name_handler_is_free(cmd_name, TCP_CMD_DATA);
		if (err != TCP_CMD_ERR_OK) {
			LOG_ERR("Command name already exists: %s", s_cmd_err_to_string(err));
			return err;
		}

		tcp_cmd_data_handler_t *data_handler = NULL;

		data_handler = (tcp_cmd_data_handler_t *)s_get_free_cmd_handler(TCP_CMD_DATA);

		if (!data_handler) {
			err = TCP_CMD_ERR_NO_SPACE;
			LOG_ERR("No free data handler found: %s",
				s_cmd_err_to_string(err));
			return err;
		}
			data_handler->assigned = true;
			data_handler->handler_cb = (cmd_handler_data_cb)handler_cb;
			snprintf(data_handler->cmd_name, sizeof(data_handler->cmd_name),
				 "%s", cmd_name);
			LOG_INF("Registered data handler for command: %s", cmd_name);
		break;

	default:
		err = TCP_CMD_ERR_TYPE_NOT_FOUND;
		LOG_ERR("Command type not found: %s", s_cmd_err_to_string(err));
		break;
	}

	return err;
}

tcp_cmd_err_t tcp_cmd_handler_unregister_cb(const char *cmd_name, tcp_cmd_type_t type)
{
	tcp_cmd_err_t err = TCP_CMD_ERR_OK;

	err = s_tcp_cmd_handler_check_init(tcp_cmd_handler);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	err = s_tcp_cmd_handler_check_name(cmd_name);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	switch (type) {
	case TCP_CMD_NAME_ONLY:
		tcp_cmd_name_handler_t *name_handler;

		name_handler =
		(tcp_cmd_name_handler_t *)s_get_cmd_handler_by_name(cmd_name, TCP_CMD_NAME_ONLY);

		if (name_handler) {
			s_clear_cmd_handler(name_handler, TCP_CMD_NAME_ONLY);
			LOG_INF("Unregistered name handler for command: %s", cmd_name);
			return err;
		}
		break;

	case TCP_CMD_NAME_VALUE:
		tcp_cmd_value_handler_t *value_handler;

		value_handler =
		(tcp_cmd_value_handler_t *)s_get_cmd_handler_by_name(cmd_name, TCP_CMD_NAME_VALUE);

		if (value_handler) {
			s_clear_cmd_handler(value_handler, TCP_CMD_NAME_VALUE);
			LOG_INF("Unregistered value handler for command: %s", cmd_name);
			return err;
		}
		break;

	case TCP_CMD_DATA:
		tcp_cmd_data_handler_t *data_handler;

		data_handler =
		(tcp_cmd_data_handler_t *)s_get_cmd_handler_by_name(cmd_name, TCP_CMD_DATA);

		if (data_handler) {
			s_clear_cmd_handler(data_handler, TCP_CMD_DATA);
			LOG_INF("Unregistered data handler for command: %s", cmd_name);
			return err;
		}
		break;

	default:
		err = TCP_CMD_ERR_TYPE_NOT_FOUND;
		LOG_ERR("Command type not found: %s", s_cmd_err_to_string(err));
		return err;
	}

	err = TCP_CMD_ERR_NAME_NOT_FOUND;
	LOG_ERR("Command name not found: %s", s_cmd_err_to_string(err));
	return err;
}

tcp_cmd_err_t tcp_cmd_handler_exec_name_cb(const char *cmd_name)
{
	tcp_cmd_err_t err = TCP_CMD_ERR_OK;

	err = s_tcp_cmd_handler_check_init(tcp_cmd_handler);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	err = s_tcp_cmd_handler_check_name(cmd_name);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	tcp_cmd_name_handler_t *handler;

	handler = (tcp_cmd_name_handler_t *)s_get_cmd_handler_by_name(cmd_name, TCP_CMD_NAME_ONLY);
	if (!handler || !handler->assigned || !handler->handler_cb) {
		err = TCP_CMD_ERR_NAME_NOT_FOUND;
		LOG_ERR("Name handler not found or not assigned: %s", s_cmd_err_to_string(err));
		return err;
	}

	return handler->handler_cb(cmd_name);
}

tcp_cmd_err_t tcp_cmd_handler_exec_value_cb(const char *cmd_name, int value)
{
	tcp_cmd_err_t err = TCP_CMD_ERR_OK;

	err = s_tcp_cmd_handler_check_init(tcp_cmd_handler);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	err = s_tcp_cmd_handler_check_name(cmd_name);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	tcp_cmd_value_handler_t *handler;

	handler =
	(tcp_cmd_value_handler_t *)s_get_cmd_handler_by_name(cmd_name, TCP_CMD_NAME_VALUE);

	if (!handler || !handler->assigned || !handler->handler_cb) {
		err = TCP_CMD_ERR_NAME_NOT_FOUND;
		LOG_ERR("Value handler not found or not assigned: %s", s_cmd_err_to_string(err));
		return err;
	}

	return handler->handler_cb(cmd_name, value);
}

tcp_cmd_err_t tcp_cmd_handler_exec_data_cb(const char *cmd_name, const uint8_t *data, size_t len)
{
	tcp_cmd_err_t err = TCP_CMD_ERR_OK;

	err = s_tcp_cmd_handler_check_init(tcp_cmd_handler);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	err = s_tcp_cmd_handler_check_name(cmd_name);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	if (!data || len == 0 || len > CONFIG_TCP_CMD_HANDLER_DATA_MAX_LEN) {
		err = TCP_CMD_ERR_DATA;
		LOG_ERR("Invalid data for command: %s", s_cmd_err_to_string(err));
		return err;
	}

	tcp_cmd_data_handler_t *handler;

	handler =
	(tcp_cmd_data_handler_t *)s_get_cmd_handler_by_name(cmd_name, TCP_CMD_DATA);

	if (!handler || !handler->assigned || !handler->handler_cb) {
		err = TCP_CMD_ERR_NAME_NOT_FOUND;
		LOG_ERR("Data handler not found or not assigned: %s", s_cmd_err_to_string(err));
		return err;
	}

	return handler->handler_cb(cmd_name, data, len);
}

tcp_cmd_err_t tcp_cmd_dispatch(const uint8_t *cmd, size_t cmd_len)
{
	tcp_cmd_err_t err = TCP_CMD_ERR_OK;

	err = s_tcp_cmd_handler_check_init(tcp_cmd_handler);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	if (!cmd || cmd_len <= TCP_CMD_HANDLER_ESC_SEQ_LEN ||
	    cmd_len > TCP_CMD_HANDLER_BUFFER_SIZE) {
		err = TCP_CMD_ERR_FORMAT;
		LOG_ERR("Invalid command format: %s", s_cmd_err_to_string(err));
		return err;
	}

	tcp_cmd_parser_t *parser = &tcp_cmd_handler->parser;

	s_tcp_cmd_parser_clear(parser);

	/* Parse the command */
	err = s_tcp_cmd_parse(cmd, cmd_len, parser);
	if (err != TCP_CMD_ERR_OK) {
		return err;
	}

	if (parser->cmd_type == TCP_CMD_NAME_ONLY) {
		err = tcp_cmd_handler_exec_name_cb(parser->cmd_name);
		if (err != TCP_CMD_ERR_OK) {
			LOG_ERR("Command execution failed: %s", s_cmd_err_to_string(err));
		}
	} else if (parser->cmd_type == TCP_CMD_NAME_VALUE) {
		err = tcp_cmd_handler_exec_value_cb(parser->cmd_name, parser->value_int);
		if (err != TCP_CMD_ERR_OK) {
			LOG_ERR("Command execution failed: %s", s_cmd_err_to_string(err));
		}
	} else if (parser->cmd_type == TCP_CMD_DATA) {
		err = tcp_cmd_handler_exec_data_cb(parser->cmd_name,
						   parser->cmd_data, parser->cmd_data_len);
		if (err != TCP_CMD_ERR_OK) {
			LOG_ERR("Command execution failed: %s", s_cmd_err_to_string(err));
		}
	}

	if (err != TCP_CMD_ERR_OK) {
		err = TCP_CMD_ERR_EXECUTION;
		LOG_ERR("Command execution failed: %s", s_cmd_err_to_string(err));
	}

	return err;
}
