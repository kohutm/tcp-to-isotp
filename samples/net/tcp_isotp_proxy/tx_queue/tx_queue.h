/**
 * @file tx_queue.h
 * @brief Header file for TX message queue functionality.
 * This file contains the declarations for the TX message queue API,
 * including error codes, initialization function, and other related types.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <stddef.h>

#define TX_MSG_QUEUE_MAX_NAME_LEN (CONFIG_TX_MSG_QUEUE_MAX_NAME_LEN + 1)

/** TX message queue error codes */
typedef enum {
    TX_MSG_QUEUE_ERR_OK = 0,          // No error
    TX_MSG_QUEUE_ERR_INIT,            // Initialization error
    TX_MSG_QUEUE_ERR_SEND,            // Send error
    TX_MSG_QUEUE_ERR_NO_SPACE,        // No space in queue
    TX_MSG_QUEUE_ERR_INVALID_PARAM,   // Invalid parameter error
    TX_MSG_QUEUE_ERR_UNREGISTER       // Unregister error
} tx_msg_queue_err_t;

struct tx_queue_msg;

/** Callback for sending messages */
typedef void (*tx_queue_msg_send_cb_t)(const uint8_t *data, size_t len, void *user_data);
/** Function for user data handling */
typedef void (*tx_queue_msg_usr_data_handler_t)(struct tx_queue_msg *msg, void *user_data);

/** Structure for a message in the TX queue */
struct tx_queue_msg {
    void *fifo_reserved;  // Reserved for FIFO management
    void *usr_ctx;        // User context for the message
    size_t len;           // Length of the data
    uint32_t msg_id;      // ID for tracking the message
    uint8_t data[CONFIG_TX_MSG_QUEUE_PAYLOAD_MAX_SIZE]; // Data payload
};

/** Structure for the TX queue */
struct tx_queue {
    char name[TX_MSG_QUEUE_MAX_NAME_LEN];  // Name of the TX queue
    struct k_fifo fifo;  // FIFO for message queueing
    struct tx_queue_msg msg_pool[CONFIG_TX_MSG_QUEUE_DEPTH];  // Pool of messages
    struct k_sem msg_sem;  // Semaphore for managing message availability
    tx_queue_msg_usr_data_handler_t usr_data_handler; // Callback for user data handling
    tx_queue_msg_send_cb_t send_cb; // Callback for sending data
    void *user_data;  // User data for callbacks
    sys_snode_t node;  // Node for global queue list
    uint8_t used_flags[CONFIG_TX_MSG_QUEUE_DEPTH]; // Used flags for message pool
};

/** Structure for the TX queue registration parameters */
struct tx_queue_register_params {
    struct tx_queue *queue;  // Pointer to the TX queue
    char *name;  // Name of the TX queue
    void *user_data;  // User data for callbacks
    tx_queue_msg_usr_data_handler_t usr_data_handler;  // Callback for user data handling
    tx_queue_msg_send_cb_t send_cb;  // Callback for sending data
};

/** Initialize the TX message queue system */
void tx_msg_queue_system_init(void);

/**
 * @brief Register a TX queue
 * @param params Pointer to the registration parameters
 * @return Error code indicating the result of the operation
 */
tx_msg_queue_err_t tx_queue_register(const struct tx_queue_register_params *params);

/**
 * @brief Unregister a TX queue
 * @param queue Pointer to the TX queue to unregister
 * @return Error code indicating the result of the operation
 */
tx_msg_queue_err_t tx_queue_unregister(struct tx_queue *queue);

/**
 * @brief Get a TX queue by its name
 * @param name Name of the TX queue to search for
 * @return Pointer to the TX queue if found, NULL otherwise
 */
struct tx_queue *tx_queue_get_by_name(const char *name);

/**
 * @brief Send a message to the TX queue with an optional message ID
 * @param queue Pointer to the TX queue
 * @param data Pointer to the data to send
 * @param len Length of the data
 * @param msg_id Optional message ID for tracking
 * @param usr_ctx User context for the message
 * @return Error code indicating the result of the operation
 */
tx_msg_queue_err_t tx_queue_msg_send_ex(struct tx_queue *queue, 
    const void *data, size_t len, uint32_t msg_id, void *usr_ctx);

/**
 * @brief Send a message to the TX queue
 * @param queue Pointer to the TX queue
 * @param data Pointer to the data to send
 * @param len Length of the data
 * @return Error code indicating the result of the operation
 */
tx_msg_queue_err_t tx_queue_msg_send(struct tx_queue *queue, 
    const void *data, size_t len);

#ifdef __cplusplus
}
#endif
