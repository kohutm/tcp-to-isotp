/*
 * Copyright (c) 2025 N-Ix
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include "tcp_link.h"

LOG_MODULE_REGISTER(tcp_link, CONFIG_TCP_LINK_LOG_LEVEL);

K_THREAD_STACK_DEFINE(tcp_link_rx_thread_stack, CONFIG_TCP_LINK_RX_THREAD_STACK_SIZE);

#if defined(CONFIG_TX_MSG_QUEUE)
#include "tx_queue.h"
#endif

#if defined(CONFIG_CAN_ISOTP_PROXY)
#include "isotp_proxy.h"
#endif

#if defined(CONFIG_TCP_CMD_HANDLER)
#include "tcp_cmd_handler.h"
#define TCP_LINK_RX_BUFFER_SIZE TCP_CMD_HANDLER_BUFFER_SIZE
#else
#define TCP_LINK_RX_BUFFER_SIZE (CONFIG_TCP_LINK_RX_BUFFER_SIZE)
#endif

#if defined(CONFIG_TCP_CMD_HANDLER)
/* Define the type for the RX handler callback if TCP command handler is enabled */
typedef tcp_link_err_t (*rx_handler)(const uint8_t *data, size_t cmd_len);

static tcp_link_err_t s_tcp_link_rx_handler(const uint8_t *data, size_t cmd_len);
#else
/* Define the type for the RX handler callback for general use case */
typedef tcp_link_err_t (*rx_handler)(const uint8_t *data, size_t cmd_len);

__weak tcp_link_err_t s_tcp_link_rx_handler(const uint8_t *data, size_t cmd_len)
{
	/* Default RX handler implementation should be overridden by user */
	if (!data || cmd_len == 0) {
		LOG_ERR("Invalid data or command length");
		return TCP_LINK_ERR_PARAM;
	}
	LOG_INF("Received TCP data: %.*s", (int)cmd_len, data);
	return TCP_LINK_OK;
}
#endif

/**
 * @brief Enumeration for TCP RX states.
 */
enum tcp_rx_state {
	TCP_RX_STATE_IDLE = 0,       /* TCP RX is idle */
	TCP_RX_STATE_WAITING,        /* Waiting for data to be received */
	TCP_RX_STATE_DATA_RECEIVED,  /* Data has been received */
	TCP_RX_STATE_CB_EXECUTING,   /* Executing the RX callback */
	TCP_RX_STATE_COMPLETE,       /* RX process is complete */
	TCP_RX_STATE_ERROR           /* An error occurred during the RX process */
};

/**
 * @brief Enumeration for TCP TX states.
 */
enum tcp_tx_state {
	TCP_TX_STATE_IDLE = 0,  /* TCP TX is idle */
	TCP_TX_STATE_READY,     /* TCP TX is ready to send data */
	TCP_TX_STATE_SENDING,   /* TCP TX is currently sending data */
	TCP_TX_STATE_ERROR      /* An error occurred during the TX process */
};

/**
 * @brief Structure for TCP link module representation.
 */
typedef struct {
	int server_fd;                   /* TCP server file descriptor */
	struct sockaddr_in local_addr;   /* Local address for the TCP server */
	struct k_thread recv_thread;     /* Thread for receiving TCP data */
	uint8_t recv_buf[TCP_LINK_RX_BUFFER_SIZE];  /* Buffer for receiving TCP data */
	enum tcp_rx_state rx_state;      /* Current state of the TCP RX process */
	rx_handler rx_cb;                /* Callback for handling received data */
	tcp_link_err_t rx_cb_last_err;   /* Last error from the RX callback */

	int client_fd;                   /* TCP client file descriptor */
	struct sockaddr_in client_addr;  /* Client address for the TCP connection */
#if defined(CONFIG_TX_MSG_QUEUE)
	struct tx_queue tcp_tx_queue;    /* TX message queue for sending data */
#endif
	struct k_mutex tx_lock;
	enum tcp_tx_state tx_state;      /* Current state of the TCP TX process */

	bool is_initialized;             /* Flag indicating if the TCP link is initialized */
} tcp_link_t;

static tcp_link_t tcp_link = {0};

#if defined(CONFIG_TX_MSG_QUEUE)
static struct tcp_tx_q_ctx tcp_tx_q_ctx = {
	.queue_name = CONFIG_TCP_LINK_TX_QUEUE_NAME,
#if defined(CONFIG_TCP_CMD_HANDLER)
	.sender_name = "tcp_cmd_handler",
#else
	.sender_name = "tcp_link_public",
#endif
	.src_id = CONFIG_TCP_LINK_TX_SRC_ID,
	.dest_id = CONFIG_TCP_LINK_TX_DST_ID,
	.last_msg_id = 0,
	.msg_total_len = 0,
	.is_last_msg = false,
	.client_fd = &tcp_link.client_fd
};
#endif

/**
 * @brief Send data over the TCP link.
 * @param data Pointer to the data to send.
 * @param len Length of the data to send.
 * @return TCP_LINK_OK on success, or an error code on failure.
 */
static tcp_link_err_t s_tcp_link_send(const uint8_t *data, size_t len);

/**
 * @brief Receive thread for handling incoming TCP data.
 * @param arg1 Unused argument.
 * @param arg2 Unused argument.
 * @param arg3 Unused argument.
 */
static void s_tcp_link_rx_thread(void *arg1, void *arg2, void *arg3);

#if defined(CONFIG_TCP_CMD_HANDLER) && defined(CONFIG_TX_MSG_QUEUE)
/**
 * @brief TCP link RX handler.
 * Handles received TCP data and dispatches it to the TCP command handler.
 * @param data Pointer to the received data.
 * @param cmd_len Length of the received command data.
 * @return TCP_LINK_OK on success, or an error code on failure.
 */
static tcp_link_err_t s_tcp_link_rx_handler(const uint8_t *data, size_t cmd_len);

/**
 * @brief TCP command handler for echoing command names.
 * This function echoes the command name back to the sender.
 * @param cmd_name Name of the command to echo.
 * @return TCP_CMD_ERR_OK on success, or an error code on failure.
 */
static tcp_cmd_err_t s_tcp_link_cmd_handler_echo_cmd_name(const char *cmd_name);

/**
 * @brief TCP command handler for printing command names.
 * This function prints the command name to the log.
 * @param cmd_name Name of the command to print.
 * @return TCP_CMD_ERR_OK on success, or an error code on failure.
 */
static tcp_cmd_err_t s_tcp_link_cmd_handler_print_cmd_name(const char *cmd_name);

/**
 * @brief TCP command handler for echoing command names with values.
 * This function echoes the command name and value back to the sender.
 * @param cmd_name Name of the command to echo.
 * @param value Value associated with the command.
 * @return TCP_CMD_ERR_OK on success, or an error code on failure.
 */
static tcp_cmd_err_t s_tcp_link_cmd_handler_echo_name_value(const char *cmd_name, int value);

/**
 * @brief TCP command handler for printing command names with values.
 * This function prints the command name and value to the log.
 * @param cmd_name Name of the command to print.
 * @param value Value associated with the command.
 * @return TCP_CMD_ERR_OK on success, or an error code on failure.
 */
static tcp_cmd_err_t s_tcp_link_cmd_handler_print_name_value(const char *cmd_name, int value);

/**
 * @brief TCP command handler for echoing data.
 * This function echoes the received data back to the sender.
 * @param cmd_name Name of the command associated with the data.
 * @param data Pointer to the data to echo.
 * @param len Length of the data to echo.
 * @return TCP_CMD_ERR_OK on success, or an error code on failure.
 */
static tcp_cmd_err_t s_tcp_link_cmd_handler_data_echo(const char *cmd_name,
						      const char *data, size_t len);

/**
 * @brief TCP command handler for printing data.
 * This function prints the received data in hexadecimal format to the log.
 * @param cmd_name Name of the command associated with the data.
 * @param data Pointer to the data to print.
 * @param len Length of the data to print.
 * @return TCP_CMD_ERR_OK on success, or an error code on failure.
 */
static tcp_cmd_err_t s_tcp_link_cmd_handler_data_print(const char *cmd_name,
						       const char *data, size_t len);

/**
 * @brief TCP command handler for sending data to ISO-TP.
 * This function sends the received data to the ISO-TP TX queue.
 * @param cmd_name Name of the command associated with the data.
 * @param data Pointer to the data to send.
 * @param len Length of the data to send.
 * @return TCP_CMD_ERR_OK on success, or an error code on failure.
 */
static tcp_cmd_err_t s_tcp_link_cmd_handler_data_to_isotp(const char *cmd_name,
							  const char *data, size_t len);

/**
 * @brief Register TCP command handlers for the TCP link.
 */
static void s_tcp_link_register_tcp_cmd_handlers(void);

static tcp_link_err_t s_tcp_link_rx_handler(const uint8_t *data, size_t cmd_len)
{
	if (!data || cmd_len == 0) {
		LOG_ERR("Invalid data or command length");
		return TCP_LINK_ERR_PARAM;
	}
	/* Delete null terminator to avoid issues with ESC sequence */
	cmd_len--; /* Decrement cmd_len to account for the ESC sequence */
	/* Add ESC sequence to the end of the received data */
	memcpy(tcp_link.recv_buf + cmd_len, TCP_CMD_HANDLER_ESC_SEQ, TCP_CMD_HANDLER_ESC_SEQ_LEN);
	cmd_len += TCP_CMD_HANDLER_ESC_SEQ_LEN;
	/* Dispatch the command to the TCP command handler */
	tcp_cmd_err_t err = tcp_cmd_dispatch(data, cmd_len);

	if (err != TCP_CMD_ERR_OK) {
		LOG_ERR("tcp_cmd_dispatch failed: %d", err);
		return TCP_LINK_ERR_PARAM;
	}

	return TCP_LINK_OK;
}

static tcp_cmd_err_t s_tcp_link_cmd_handler_echo_cmd_name(const char *cmd_name)
{
	tx_msg_queue_err_t err = TX_MSG_QUEUE_ERR_OK;
	char buf[CONFIG_TCP_CMD_HANDLER_NAME_MAX_LEN + 1]; /* +1 for null terminator */

	memcpy(buf, cmd_name, strlen(cmd_name));
	buf[strlen(cmd_name)] = '\0'; /* Ensure null termination */
	err = tx_queue_msg_send(&tcp_link.tcp_tx_queue, buf, strlen(buf));
	if (err != TX_MSG_QUEUE_ERR_OK) {
		LOG_ERR("Failed to send command name '%s' to TX queue: %d", cmd_name, err);
		return TCP_CMD_ERR_HANDLER;
	}

	return TCP_CMD_ERR_OK;
}

static tcp_cmd_err_t s_tcp_link_cmd_handler_print_cmd_name(const char *cmd_name)
{
	LOG_INF("Command received: %s", cmd_name);
	return TCP_CMD_ERR_OK;
}

static tcp_cmd_err_t s_tcp_link_cmd_handler_echo_name_value(const char *cmd_name, int value)
{
	tx_msg_queue_err_t err = TX_MSG_QUEUE_ERR_OK;
	/* +2 for space and null terminator */
	char buf[CONFIG_TCP_CMD_HANDLER_NAME_MAX_LEN + CONFIG_TCP_CMD_HANDLER_VAL_MAX_LEN + 2];

	snprintf(buf, sizeof(buf), "%s %d", cmd_name, value);
	err = tx_queue_msg_send(&tcp_link.tcp_tx_queue, buf, strlen(buf));
	if (err != TX_MSG_QUEUE_ERR_OK) {
		LOG_ERR("Failed to send command name '%s' with value '%d' to TX queue: %s",
			cmd_name, value, tcp_link.tcp_tx_queue.name);
		return TCP_CMD_ERR_HANDLER;
	}
	return TCP_CMD_ERR_OK;
}

static tcp_cmd_err_t s_tcp_link_cmd_handler_print_name_value(const char *cmd_name, int value)
{
	LOG_INF("Command received: %s with value: %d", cmd_name, value);
	return TCP_CMD_ERR_OK;
}

static tcp_cmd_err_t s_tcp_link_cmd_handler_data_echo(const char *cmd_name,
						      const char *data, size_t len)
{
	tx_msg_queue_err_t err = TX_MSG_QUEUE_ERR_OK;

	err = tx_queue_msg_send(&tcp_link.tcp_tx_queue, data, len);
	if (err != TX_MSG_QUEUE_ERR_OK) {
		LOG_ERR("Failed to send data '%.*s' to TX queue: %s",
			(int)len, data, tcp_link.tcp_tx_queue.name);

		return TCP_CMD_ERR_HANDLER;
	}

	return TCP_CMD_ERR_OK;
}

static tcp_cmd_err_t s_tcp_link_cmd_handler_data_print(const char *cmd_name,
						       const char *data, size_t len)
{
	/* 2 chars per byte, plus null terminator */
	char buf[(CONFIG_TCP_CMD_HANDLER_DATA_MAX_LEN * 2) + 1];
	size_t i, pos = 0;

	LOG_INF("Command received: %s with data length: %d", cmd_name, (int)len);

	if (len > CONFIG_TCP_CMD_HANDLER_DATA_MAX_LEN) {
		len = CONFIG_TCP_CMD_HANDLER_DATA_MAX_LEN; /* prevent buffer overflow */
	}

	for (i = 0; i < len; i++) {
		if ((pos + 2) >= sizeof(buf)) {
			break;
		}
		pos += snprintk(&buf[pos], sizeof(buf) - pos, "%02X", (unsigned char)data[i]);
	}

	buf[pos] = '\0'; /* гарантовано завершуємо */
	LOG_INF("Data (hex): %s", buf);

	return TCP_CMD_ERR_OK;
}

static tcp_cmd_err_t s_tcp_link_cmd_handler_data_to_isotp(const char *cmd_name,
							  const char *data, size_t len)
{
	struct tx_queue *isotp_tx_queue;
	tx_msg_queue_err_t tx_queue_err;

	isotp_tx_queue = tx_queue_get_by_name(CONFIG_CAN_ISOTP_PROXY_TX_QUEUE_NAME);

	if (!isotp_tx_queue) {
		LOG_ERR("Failed to get ISO-TP TX queue");
		return TCP_CMD_ERR_HANDLER;
	}

	tx_queue_err = tx_queue_msg_send(isotp_tx_queue, data, len);

	if (tx_queue_err != TX_MSG_QUEUE_ERR_OK) {
		LOG_ERR("Failed to send data '%.*s' to ISO-TP TX queue: %s",
			(int)len, data, isotp_tx_queue->name);

		return TCP_CMD_ERR_HANDLER;
	}

	return TCP_CMD_ERR_OK;
}

static void s_tcp_link_register_tcp_cmd_handlers(void)
{
	tcp_cmd_err_t err;

	/* Register command handlers for name only commands */
	err = tcp_cmd_handler_register_cb("name-echo", TCP_CMD_NAME_ONLY,
					  s_tcp_link_cmd_handler_echo_cmd_name);
	if (err != TCP_CMD_ERR_OK) {
		LOG_ERR("Failed to register name-echo handler: %d", err);
	}
	err = tcp_cmd_handler_register_cb("name-print", TCP_CMD_NAME_ONLY,
					  s_tcp_link_cmd_handler_print_cmd_name);
	if (err != TCP_CMD_ERR_OK) {
		LOG_ERR("Failed to register name-print handler: %d", err);
	}

	/* Register command handlers for name and value commands */
	err = tcp_cmd_handler_register_cb("name-value-echo", TCP_CMD_NAME_VALUE,
					  s_tcp_link_cmd_handler_echo_name_value);
	if (err != TCP_CMD_ERR_OK) {
		LOG_ERR("Failed to register name-value-echo handler: %d", err);
	}
	err = tcp_cmd_handler_register_cb("name-value-print", TCP_CMD_NAME_VALUE,
					  s_tcp_link_cmd_handler_print_name_value);
	if (err != TCP_CMD_ERR_OK) {
		LOG_ERR("Failed to register name-value-print handler: %d", err);
	}

	/* Register command handlers for data commands */
	err = tcp_cmd_handler_register_cb("data-echo", TCP_CMD_DATA,
					  s_tcp_link_cmd_handler_data_echo);
	if (err != TCP_CMD_ERR_OK) {
		LOG_ERR("Failed to register data-echo handler: %d", err);
	}
	err = tcp_cmd_handler_register_cb("data-print", TCP_CMD_DATA,
					  s_tcp_link_cmd_handler_data_print);
	if (err != TCP_CMD_ERR_OK) {
		LOG_ERR("Failed to register data-print handler: %d", err);
	}
	err = tcp_cmd_handler_register_cb("data-to-isotp", TCP_CMD_DATA,
					  s_tcp_link_cmd_handler_data_to_isotp);
	if (err != TCP_CMD_ERR_OK) {
		LOG_ERR("Failed to register data-to-isotp handler: %d", err);
	}
}
#endif

#if defined(CONFIG_TX_MSG_QUEUE)
/**
 * @brief Callback function for sending messages from the TX queue.
 * This function is called when a message is ready to be sent from the TX queue.
 * It prepares the message with the appropriate header and sends it over the TCP link.
 * @param data Pointer to the data to send.
 * @param len Length of the data to send.
 * @param user_data Pointer to user data, which is the TX queue context.
 * @note This function checks the validity of the parameters, prepares the message header,
 *       and appends the appropriate end-of-message sequence (EOF packet or EOF message).
 *       It also handles errors related to message length and sending status.
 * @note The function assumes that the user_data is a pointer to a struct tcp_tx_q_ctx,
 *       which contains the necessary context for sending the message.
 * @note The function uses the s_tcp_link_send function to send the prepared message.
 * @note The function is designed to be used with the TX message queue system,
 *       specifically for sending messages over a TCP link.
 */
static void s_tcp_link_tx_queue_msg_send_cb(const uint8_t *data,
					    size_t len, void *user_data);

/**
 * @brief User data handler for TX queue messages.
 * This function processes the user data associated with a TX queue message.
 * It extracts the source and destination IDs, checks if the message is the last one,
 * and prepares the message header with the appropriate fields.
 * @param msg Pointer to the TX queue message.
 * @param user_data Pointer to user data, which is the TX queue context.
 * @note This function assumes that the user_data is a pointer to a struct tcp_tx_q_ctx,
 *       which contains the necessary context for processing the message.
 */
static void s_tcp_link_tx_queue_msg_usr_data_handler(struct tx_queue_msg *msg,
						     void *user_data);

static void s_tcp_link_tx_queue_msg_send_cb(const uint8_t *data,
					    size_t len, void *user_data)
{
	struct tcp_tx_q_ctx *ctx = (struct tcp_tx_q_ctx *)user_data;
	tcp_link_err_t send_status;

	if (!ctx || !data || len == 0) {
		LOG_ERR("Invalid parameters for TX queue send callback");
		return;
	}

	if (len > CONFIG_TCP_LINK_TX_MAX_PAYLOAD_SIZE) {
		LOG_ERR("Data length exceeds maximum payload size: %zu > %d",
			len, CONFIG_TCP_LINK_TX_MAX_PAYLOAD_SIZE);
		return;
	}

	size_t eof_msg_len = ctx->is_last_msg ? TCP_LINK_TX_EOF_PCKT_LEN : TCP_LINK_TX_EOF_MSG_LEN;

	if ((ctx->msg_total_len - sizeof(struct tcp_link_tx_hdr) - eof_msg_len) != len) {
		LOG_ERR("Message length mismatch: expected %zu, got %zu",
			ctx->msg_total_len - sizeof(struct tcp_link_tx_hdr), len);
		return;
	}

	send_status = s_tcp_link_send(ctx->tx_buf, ctx->msg_total_len);
	if (send_status != TCP_LINK_OK) {
		LOG_ERR("Failed to send TCP message: %d", send_status);
		return;
	}
}

static void s_tcp_link_tx_queue_msg_usr_data_handler(struct tx_queue_msg *msg,
						     void *user_data)
{
	struct tcp_tx_q_ctx *queue_ctx = (struct tcp_tx_q_ctx *)user_data;
	struct tcp_link_tx_ctx *msg_ctx = (struct tcp_link_tx_ctx *)msg->usr_ctx;

	if (!queue_ctx) {
		LOG_ERR("Invalid user data in TX queue message handler");
		return;
	}

	queue_ctx->last_msg_id = msg->msg_id;

	if (!msg_ctx) {
		queue_ctx->src_id = CONFIG_TCP_LINK_TX_SRC_ID;
		queue_ctx->dest_id = CONFIG_TCP_LINK_TX_DST_ID;
		queue_ctx->is_last_msg = false; /* Default to false if no context provided */
	} else {
		queue_ctx->src_id = msg_ctx->src_id;
		queue_ctx->dest_id = msg_ctx->dest_id;
		queue_ctx->is_last_msg = msg_ctx->is_last_msg;
	}

	/* Prepare the full message with header */
	struct tcp_link_tx_hdr hdr = {
		.src_id = queue_ctx->src_id,
		.dest_id = queue_ctx->dest_id,
		.msg_id = queue_ctx->last_msg_id,
		.payload_len = msg->len
	};

	queue_ctx->msg_total_len = sizeof(hdr) + msg->len +
	(queue_ctx->is_last_msg ? TCP_LINK_TX_EOF_PCKT_LEN : TCP_LINK_TX_EOF_MSG_LEN);

	if (queue_ctx->msg_total_len > TCP_LINK_TX_BUFFER_SIZE) {
		LOG_ERR("Total message length exceeds buffer size: %zu > %d",
			queue_ctx->msg_total_len, TCP_LINK_TX_BUFFER_SIZE);
		queue_ctx->msg_total_len = sizeof(hdr);
		return;
	}

	memcpy(queue_ctx->tx_buf, &hdr, sizeof(hdr));
	memcpy(queue_ctx->tx_buf + sizeof(hdr), msg->data, msg->len);

	if (!queue_ctx->is_last_msg) {
		memcpy(queue_ctx->tx_buf + sizeof(hdr) + msg->len,
		       TCP_LINK_TX_EOF_MSG, TCP_LINK_TX_EOF_MSG_LEN);
	} else {
		/* If this is the last message, append the ESC sequence */
		memcpy(queue_ctx->tx_buf + sizeof(hdr) + msg->len,
		       TCP_LINK_TX_EOF_PCKT, TCP_LINK_TX_EOF_PCKT_LEN);
	}
}
#endif

static void s_tcp_link_rx_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1); ARG_UNUSED(arg2); ARG_UNUSED(arg3);

	while (1) {
		socklen_t addrlen = sizeof(tcp_link.client_addr);

		tcp_link.client_fd = accept(tcp_link.server_fd,
					    (struct sockaddr *)&tcp_link.client_addr, &addrlen);

		if (tcp_link.client_fd < 0) {
			tcp_link.tx_state = TCP_TX_STATE_ERROR;
			LOG_ERR("Accept failed");
			continue;
		} else {
			tcp_link.tx_state = TCP_TX_STATE_READY;
			LOG_INF("Client connected");
		}

		while (1) {
			tcp_link.rx_state = TCP_RX_STATE_WAITING;
#if defined(CONFIG_TCP_CMD_HANDLER)
			int len = recv(tcp_link.client_fd, tcp_link.recv_buf,
				sizeof(tcp_link.recv_buf) - TCP_CMD_HANDLER_ESC_SEQ_LEN + 1, 0);
#else
			int len = recv(tcp_link.client_fd, tcp_link.recv_buf,
				       sizeof(tcp_link.recv_buf), 0);
#endif
			if (len <= 0) {
				LOG_WRN("Client disconnected or recv error");
				close(tcp_link.client_fd);
				tcp_link.client_fd = -1;
				tcp_link.rx_state = TCP_RX_STATE_IDLE;
				break;
			}

			tcp_link.rx_state = TCP_RX_STATE_DATA_RECEIVED;

			if (tcp_link.rx_cb) {
				tcp_link.rx_state = TCP_RX_STATE_CB_EXECUTING;
				tcp_link.rx_cb_last_err = tcp_link.rx_cb(tcp_link.recv_buf, len);
				tcp_link.rx_state = TCP_RX_STATE_COMPLETE;
			} else {
				tcp_link.rx_state = TCP_RX_STATE_ERROR;
				LOG_ERR("No RX handler registered");
			}
		}
	}
}

static tcp_link_err_t s_tcp_link_send(const uint8_t *data, size_t len)
{
	if (!data || len == 0) {
		return TCP_LINK_ERR_PARAM;
	}

	if (!tcp_link.is_initialized) {
		return TCP_LINK_ERR_INIT;
	}

#if defined(CONFIG_TCP_LINK_TX_USE_SEND_TIMEOUT)
	if (k_mutex_lock(&tcp_link.tx_lock, K_MSEC(CONFIG_TCP_LINK_TX_SEND_TIMEOUT)) != 0) {
		return TCP_LINK_ERR_TX_TIMEOUT;
	}
#else
	if (k_mutex_lock(&tcp_link.tx_lock, K_NO_WAIT) != 0) {
		return TCP_LINK_ERR_TX_BUSY;
	}
#endif
	if (tcp_link.tx_state != TCP_TX_STATE_READY) {
		k_mutex_unlock(&tcp_link.tx_lock);
		return TCP_LINK_ERR_NOT_READY;
	}

	tcp_link.tx_state = TCP_TX_STATE_SENDING;

	ssize_t sent = send(tcp_link.client_fd, data, len, 0);

	if (sent < 0 || (size_t)sent != len) {
		LOG_ERR("TCP send failed (%zd/%zu)", sent, len);
		tcp_link.tx_state = TCP_TX_STATE_ERROR;
		k_mutex_unlock(&tcp_link.tx_lock);
		return TCP_LINK_ERR_SEND;
	}

	k_mutex_unlock(&tcp_link.tx_lock);

	tcp_link.tx_state = TCP_TX_STATE_READY;

	return TCP_LINK_OK;
}

/** Public API function to initialize the TCP link */
tcp_link_err_t tcp_link_init(void)
{
	tcp_link_err_t err = TCP_LINK_OK;
	int status;

	if (tcp_link.is_initialized) {
		err = TCP_LINK_ERR_INIT;
		LOG_WRN("TCP Link is already initialized");
		return err;
	}

	tcp_link.server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (tcp_link.server_fd < 0) {
		err = TCP_LINK_ERR_SOCKET;
		LOG_ERR("Failed to create TCP socket: %d", err);
		return err;
	}

	/* Initialize the TCP link structure */
	tcp_link.local_addr.sin_family = AF_INET;
	tcp_link.local_addr.sin_port = htons(CONFIG_TCP_LINK_PORT);
	tcp_link.local_addr.sin_addr.s_addr = INADDR_ANY;
	tcp_link.rx_cb = s_tcp_link_rx_handler;

	status = bind(tcp_link.server_fd, (struct sockaddr *)&tcp_link.local_addr,
		      sizeof(tcp_link.local_addr));
	if (status < 0) {
		err = TCP_LINK_ERR_SOCKET;
		LOG_ERR("Failed to bind TCP socket: %d", status);
		close(tcp_link.server_fd);
		return err;
	}

	status = listen(tcp_link.server_fd, 1);
	if (status < 0) {
		err = TCP_LINK_ERR_LISTEN;
		LOG_ERR("Failed to listen on TCP socket: %d", status);
		close(tcp_link.server_fd);
		return err;
	}

	tcp_link.rx_state = TCP_RX_STATE_IDLE;
	tcp_link.tx_state = TCP_TX_STATE_IDLE;
	tcp_link.client_fd = -1; /* No client connected yet */

	k_mutex_init(&tcp_link.tx_lock);

	/* Create the RX thread */
	k_thread_create(&tcp_link.recv_thread, tcp_link_rx_thread_stack,
			K_THREAD_STACK_SIZEOF(tcp_link_rx_thread_stack),
					s_tcp_link_rx_thread, NULL, NULL, NULL,
					CONFIG_TCP_LINK_RX_THREAD_PRIO, 0, K_NO_WAIT);

	k_thread_name_set(&tcp_link.recv_thread, CONFIG_TCP_LINK_RX_THREAD_NAME);

#if defined(CONFIG_TCP_CMD_HANDLER) && defined(CONFIG_TX_MSG_QUEUE)
	/* Initialize the TCP command handler if enabled */
	tcp_cmd_handler_init();
	/* Register command handlers */
	s_tcp_link_register_tcp_cmd_handlers();
#endif

#if defined(CONFIG_TX_MSG_QUEUE)
	/* Initialize the TX queue if enabled */
	struct tx_queue_register_params tx_params = {
		.queue = &tcp_link.tcp_tx_queue,
		.name = CONFIG_TCP_LINK_TX_QUEUE_NAME,
		.user_data = &tcp_tx_q_ctx,
		.send_cb = s_tcp_link_tx_queue_msg_send_cb,
		.usr_data_handler = s_tcp_link_tx_queue_msg_usr_data_handler
	};
	tx_queue_register(&tx_params);
#endif

	tcp_link.is_initialized = true;
	return err;
}
