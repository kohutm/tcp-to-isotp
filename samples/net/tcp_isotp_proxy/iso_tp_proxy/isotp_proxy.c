/*
 * Copyright (c) 2025 N-Ix
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/canbus/isotp.h>
#include "isotp_proxy.h"
#include "tx_queue.h"
#include "tcp_link.h"

LOG_MODULE_REGISTER(isotp_proxy, CONFIG_CAN_ISOTP_PROXY_LOG_LEVEL);

static K_THREAD_STACK_DEFINE(isotp_rx_thread_stack, CONFIG_CAN_ISOTP_PROXY_RX_STACK_SIZE);

/**
 * @brief ISO-TP send callback type.
 * This callback is called when an ISO-TP message is sent.
 * @param error Error code, ISOTP_N_OK on success.
 * @param user_data User data passed to the send function.
 * This can be used to pass context or additional information.
 * It can be NULL if not needed.
 */
typedef void (*isotp_send_cb_t)(int error, void *user_data);

/**
 * @brief ISO-TP receive callback type.
 * This callback is called when an ISO-TP message is received.
 * @param buf Pointer to the received message buffer.
 * @param tx_ctx Pointer to the TCP link transmission context.
 * @return ISOTP_PROXY_ERR_OK on success, or an error code on failure.
 */
typedef isotp_proxy_err_t (*isotp_recv_cb_t)(const struct net_buf *buf,
				struct tcp_link_tx_ctx *tx_ctx);

/**
 * @brief ISO-TP Proxy structure.
 */
typedef struct {
	const char *rx_thread_name;           /* Name of the RX thread */
	struct k_thread thread_data;          /* Thread data structure */
	struct k_mutex send_mutex;            /* Mutex for sending messages */

	const struct device *can_device;      /* Pointer to CAN device */
	const can_mode_t can_mode;            /* CAN mode (e.g., loopback, FD) */
	struct isotp_recv_ctx recv_ctx;       /* ISO-TP receive context */
	struct isotp_send_ctx send_ctx;       /* ISO-TP send context */
	const struct isotp_msg_id rx_msg_id;  /* Message ID for receiving */
	const struct isotp_msg_id tx_msg_id;  /* Message ID for sending */
	const struct isotp_fc_opts fc_opts;   /* Flow control options */

	struct tx_queue isotp_tx_queue;       /* Object ISO-TP TX queue */
	uint8_t tx_buf[CONFIG_CAN_ISOTP_PROXY_MAX_PAYLOAD_SIZE];  /* Buffer for sending data */
	size_t tx_buf_len;                    /* Length of the data in tx_buf */
	isotp_send_cb_t send_callback;        /* Callback for send completion */
	isotp_recv_cb_t recv_callback;        /* Callback for receive completion */

	struct tcp_link_tx_ctx tcp_link_tx_ctx;  /* TCP link transmission context */
	struct tx_queue *tcp_link_tx_queue;   /* Pointer to the TCP link TX queue */

	bool is_initialized;                  /* Flag to indicate if the proxy is initialized */
} isotp_proxy_t;

static isotp_proxy_t isotp_proxy = {
	.rx_thread_name = CONFIG_CAN_ISOTP_PROXY_RX_THREAD_NAME,
	.can_device = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus)),
	.can_mode = (IS_ENABLED(CONFIG_CAN_ISOTP_PROXY_LOOPBACK) ? CAN_MODE_LOOPBACK : 0) |
		(IS_ENABLED(CONFIG_CAN_ISOTP_PROXY_CAN_FD) ? CAN_MODE_FD : 0),
	.rx_msg_id = {
		.std_id = CONFIG_CAN_ISOTP_PROXY_RX_ID,
#ifdef CONFIG_CAN_ISOTP_PROXY_CAN_FD
		.flags = ISOTP_MSG_FDF | ISOTP_MSG_BRS,
#endif
	},
	.tx_msg_id = {
		.std_id = CONFIG_CAN_ISOTP_PROXY_TX_ID,
#ifdef CONFIG_CAN_ISOTP_PROXY_CAN_FD
		.dl = 64,
		.flags = ISOTP_MSG_FDF | ISOTP_MSG_BRS,
#endif
	},
	.fc_opts = {
		.bs = CONFIG_CAN_ISOTP_PROXY_FC_BC,
		.stmin = CONFIG_CAN_ISOTP_PROXY_FC_STMIN,
	},
	.tcp_link_tx_queue = NULL,
	.tx_buf_len = 0,
	.send_callback = NULL,
	.recv_callback = NULL,
	.is_initialized = false
};

/**
 * @brief ISO-TP Proxy send function.
 * This function sends data over the ISO-TP protocol.
 * It locks the send mutex, copies the data to the internal buffer,
 * and calls the ISO-TP send function.
 * @param data Pointer to the data to send.
 * @param len Length of the data to send.
 * @return ISOTP_PROXY_ERR_OK on success, or an error code on failure.
 * @note The function will block until the send operation is complete.
 *       If the send operation fails, it will unlock the mutex and return an error.
 */
static isotp_proxy_err_t s_isotp_proxy_send(const uint8_t *data, size_t len);

/**
 * @brief ISO-TP send callback.
 * This function is called when an ISO-TP message is sent.
 * It logs the result of the send operation and unlocks the send mutex.
 * @param error Error code, ISOTP_N_OK on success.
 * @param user_data User data passed to the send function, unused here.
 */
static void s_isotp_send_cb(int error, void *user_data);

/**
 * @brief ISO-TP receive callback.
 * This function is called when an ISO-TP message is received.
 * It sends the received data to the TCP link TX queue.
 * @param buf Pointer to the received message buffer.
 * @param tx_ctx Pointer to the TCP link transmission context.
 * @return ISOTP_PROXY_ERR_OK on success, or an error code on failure.
 */
static isotp_proxy_err_t s_isotp_recv_cb(const struct net_buf *buf,
					 struct tcp_link_tx_ctx *tx_ctx);

/**
 * @brief ISO-TP TX queue initialization.
 * This function initializes the ISO-TP TX queue and registers the send callback.
 * It also sets up the TCP link transmission context.
 * @return ISOTP_PROXY_ERR_OK on success, or an error code on failure.
 */
static isotp_proxy_err_t s_isotp_tx_queue_init(void);

/**
 * @brief ISO-TP TX queue callback.
 * This function is called when data is ready to be sent from the ISO-TP TX queue.
 * It sends the data using the ISO-TP send function.
 * @param data Pointer to the data to send.
 * @param len Length of the data to send.
 * @param ctx User context, unused here.
 */
static void s_isotp_tx_queue_cb(const uint8_t *data, size_t len, void *ctx);

/**
 * @brief ISO-TP RX thread function.
 * This function runs in a separate thread and handles incoming ISO-TP messages.
 * It binds the ISO-TP receiver and waits for messages to process.
 * @param arg1 Unused argument.
 * @param arg2 Unused argument.
 * @param arg3 Unused argument.
 */
static void s_isotp_rx_thread(void *arg1, void *arg2, void *arg3);

/**
 * @brief Static mutex unlock function.
 * This function releases the mutex if it is locked.
 * It is used to ensure that the mutex is not left locked in case of an error.
 * @param mutex Pointer to the mutex to release.
 */
static void s_isotp_proxy_mutex_unlock_safe(struct k_mutex *mutex);

/** Static ISO-TP functions implementation */
static void s_isotp_proxy_mutex_unlock_safe(struct k_mutex *mutex)
{
	if (k_mutex_lock(mutex, K_NO_WAIT) != 0) {
		k_mutex_unlock(mutex);
	}
}

static isotp_proxy_err_t s_isotp_tx_queue_init(void)
{
	tx_msg_queue_err_t err;

	struct tx_queue_register_params tx_params = {
		.queue = &isotp_proxy.isotp_tx_queue,          /* Pointer to the TX queue */
		.name = CONFIG_CAN_ISOTP_PROXY_TX_QUEUE_NAME,  /* Name of the TX queue */
		.user_data = NULL,                             /* No user data for callbacks */
		.send_cb = s_isotp_tx_queue_cb,                /* Callback for sending data */
		.usr_data_handler = NULL                       /* No user data handler */
	};

	err = tx_queue_register(&tx_params);
	if (err != TX_MSG_QUEUE_ERR_OK) {
		LOG_ERR("Failed to register ISO-TP TX queue: %d", err);
		return ISOTP_PROXY_ERR_INIT;
	}

	isotp_proxy.tcp_link_tx_ctx.msg_id = 0;
	isotp_proxy.tcp_link_tx_ctx.src_id = CONFIG_CAN_ISOTP_PROXY_TCP_LINK_TX_SRC_ID;
	isotp_proxy.tcp_link_tx_ctx.dest_id = CONFIG_CAN_ISOTP_PROXY_TCP_LINK_TX_DST_ID;
	isotp_proxy.tcp_link_tx_ctx.is_last_msg = false;

	isotp_proxy.tcp_link_tx_queue = tx_queue_get_by_name(CONFIG_TCP_LINK_TX_QUEUE_NAME);
	if (!isotp_proxy.tcp_link_tx_queue) {
		LOG_ERR("Failed to get TCP link TX queue");
		return ISOTP_PROXY_ERR_INIT;
	}

	return ISOTP_PROXY_ERR_OK;
}

static void s_isotp_send_cb(int error, void *user_data)
{
	ARG_UNUSED(user_data);

	if (error != ISOTP_N_OK) {
		LOG_ERR("ISO-TP send failed with error: %d", error);
	} else {
		LOG_DBG("ISO-TP message sent successfully");
	}

	s_isotp_proxy_mutex_unlock_safe(&isotp_proxy.send_mutex);
}

static isotp_proxy_err_t s_isotp_proxy_send(const uint8_t *data, size_t len)
{
	if (!isotp_proxy.is_initialized) {
		LOG_ERR("ISO-TP proxy is not initialized");
		return ISOTP_PROXY_ERR_INIT;
	}

	if (!data || len == 0 || len > CONFIG_CAN_ISOTP_PROXY_MAX_PAYLOAD_SIZE) {
		return ISOTP_PROXY_ERR_INVALID_PARAM;
	}

#if defined(CONFIG_CAN_ISOTP_PROXY_ISOTP_TX_USE_SEND_TIMEOUT)
	if (k_mutex_lock(&isotp_proxy.send_mutex,
			 K_MSEC(CONFIG_CAN_ISOTP_PROXY_ISOTP_TX_TIMEOUT)) != 0) {
		return ISOTP_PROXY_ERR_TIMEOUT;
	}
#else
	if (k_mutex_lock(&isotp_proxy.send_mutex, K_NO_WAIT) != 0) {
		return ISOTP_PROXY_ERR_SEND;
	}
#endif

	memcpy(isotp_proxy.tx_buf, data, len);
	isotp_proxy.tx_buf_len = len;

	int ret = isotp_send(&isotp_proxy.send_ctx, isotp_proxy.can_device,
				isotp_proxy.tx_buf, isotp_proxy.tx_buf_len,
				&isotp_proxy.tx_msg_id, &isotp_proxy.rx_msg_id,
				isotp_proxy.send_callback, NULL);

	if (ret != ISOTP_N_OK) {
		s_isotp_proxy_mutex_unlock_safe(&isotp_proxy.send_mutex);
		LOG_ERR("ISO-TP send failed with error: %d", ret);
		return ISOTP_PROXY_ERR_SEND;
	}

	return ISOTP_PROXY_ERR_OK;
}

static isotp_proxy_err_t s_isotp_recv_cb(const struct net_buf *buf,
					 struct tcp_link_tx_ctx *tx_ctx)
{
	if (!buf || !buf->data || buf->len == 0 || !tx_ctx) {
		LOG_ERR("Invalid parameters in ISO-TP receive callback");
		return ISOTP_PROXY_ERR_INVALID_PARAM;
	}

	tx_msg_queue_err_t err;

	err = tx_queue_msg_send_ex(isotp_proxy.tcp_link_tx_queue,
				   buf->data, buf->len, tx_ctx->msg_id, tx_ctx);
	if (err != TX_MSG_QUEUE_ERR_OK) {
		LOG_ERR("Failed to send message to TCP link TX queue: %d", err);
		return ISOTP_PROXY_ERR_SEND;
	}

	return ISOTP_PROXY_ERR_OK;
}

static void s_isotp_rx_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	int ret;
	int received_len;
	struct net_buf *buf;
	isotp_proxy_err_t recv_result;
	struct tcp_link_tx_ctx *tcp_link_tx_ctx = &isotp_proxy.tcp_link_tx_ctx;

	ret = isotp_bind(&isotp_proxy.recv_ctx, isotp_proxy.can_device,
			 &isotp_proxy.tx_msg_id, &isotp_proxy.rx_msg_id,
			 &isotp_proxy.fc_opts, K_FOREVER);
	if (ret != ISOTP_N_OK) {
		LOG_ERR("Failed to bind ISO-TP receiver [%d]", ret);
		return;
	}

	while (1) {
		received_len = isotp_recv_net(&isotp_proxy.recv_ctx, &buf,
					      K_MSEC(CONFIG_CAN_ISOTP_PROXY_RECV_TIMEOUT_MS));
		if (received_len == ISOTP_RECV_TIMEOUT) {
			continue; /* Timeout, continue to wait for data */
		} else if (received_len < 0) {
			LOG_ERR("ISO-TP receive error [%d]", received_len);
			continue;
		}
		uint32_t msg_id_idx = 0;

		while (buf) {
			if (isotp_proxy.recv_callback && isotp_proxy.is_initialized) {
				tcp_link_tx_ctx->msg_id = msg_id_idx++;
				tcp_link_tx_ctx->is_last_msg = (!buf->frags);

				recv_result = isotp_proxy.recv_callback(buf, tcp_link_tx_ctx);
				if (recv_result != ISOTP_PROXY_ERR_OK) {
					LOG_ERR("ISO-TP receive callback error: %d", recv_result);
				}
			} else {
				LOG_DBG("No receive callback set or proxy not initialized");
			}
			buf = net_buf_frag_del(NULL, buf);
		}
	}
}

static void s_isotp_tx_queue_cb(const uint8_t *data, size_t len, void *ctx)
{
	ARG_UNUSED(ctx);

	isotp_proxy_err_t send_result = s_isotp_proxy_send(data, len);

	if (send_result != ISOTP_PROXY_ERR_OK) {
		LOG_ERR("Failed to send data via ISO-TP: %d", send_result);
	}
}

/** Public API ISO-TP proxy function. */
isotp_proxy_err_t isotp_proxy_init(void)
{
	if (isotp_proxy.is_initialized) {
		LOG_WRN("ISO-TP proxy is already initialized");
		return ISOTP_PROXY_ERR_OK;
	}

	if (!device_is_ready(isotp_proxy.can_device)) {
		LOG_ERR("CAN device not ready");
		return ISOTP_PROXY_ERR_INIT;
	}

	int ret = can_set_mode(isotp_proxy.can_device, isotp_proxy.can_mode);

	if (ret != 0) {
		LOG_ERR("Failed to set CAN mode [%d]", ret);
		return ISOTP_PROXY_ERR_INIT;
	}

	ret = can_start(isotp_proxy.can_device);
	if (ret != 0) {
		LOG_ERR("Failed to start CAN [%d]", ret);
		return ISOTP_PROXY_ERR_INIT;
	}

	k_mutex_init(&isotp_proxy.send_mutex);

	isotp_proxy.recv_callback = s_isotp_recv_cb;
	isotp_proxy.send_callback = s_isotp_send_cb;

	k_thread_create(&isotp_proxy.thread_data, isotp_rx_thread_stack,
			CONFIG_CAN_ISOTP_PROXY_RX_STACK_SIZE,
			s_isotp_rx_thread, NULL, NULL, NULL,
			CONFIG_CAN_ISOTP_PROXY_RX_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&isotp_proxy.thread_data, isotp_proxy.rx_thread_name);

	ret = s_isotp_tx_queue_init();
	if (ret != ISOTP_PROXY_ERR_OK) {
		LOG_ERR("Failed to initialize ISO-TP TX queue: %d", ret);
		return ISOTP_PROXY_ERR_INIT;
	}

	isotp_proxy.is_initialized = true;

	LOG_INF("ISO-TP proxy initialized");

	return ISOTP_PROXY_ERR_OK;
}
