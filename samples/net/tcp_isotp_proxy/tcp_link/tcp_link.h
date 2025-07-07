/**
 * @file tcp_link.h
 * @brief Header file for TCP Link functionality.
 * This file contains the declarations for the TCP Link API,
 * including error codes, initialization function, and other related types.
 * The TCP Link module provides a way to send and receive messages over TCP connections.
 * It is designed to be used in a Zephyr-based application.
 * The TCP Link module supports sending messages with headers, payloads, and end-of-message sequences
 * to ensure proper message framing.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <zephyr/kernel.h>
#include <stddef.h>


/** TCP Link TX definitions */
#define TCP_LINK_TX_EOF_MSG "EOF_MSG\n\r"
#define TCP_LINK_TX_EOF_PCKT "EOF_PKT\n\r"
#define TCP_LINK_TX_EOF_MSG_LEN (sizeof(TCP_LINK_TX_EOF_MSG) - 1)
#define TCP_LINK_TX_EOF_PCKT_LEN (sizeof(TCP_LINK_TX_EOF_PCKT) - 1)
#define TCP_LINK_TX_EOF_MAX_LEN \
    ((TCP_LINK_TX_EOF_MSG_LEN > TCP_LINK_TX_EOF_PCKT_LEN) ? \
    TCP_LINK_TX_EOF_MSG_LEN : TCP_LINK_TX_EOF_PCKT_LEN)

#define TCP_LINK_TX_BUFFER_SIZE (sizeof(struct tcp_link_tx_hdr) + CONFIG_TCP_LINK_TX_MAX_PAYLOAD_SIZE \
                                + TCP_LINK_TX_EOF_MAX_LEN)

/** TCP Link error codes */
typedef enum {
    TCP_LINK_OK = 0,              // Success
    TCP_LINK_ERR_INIT,            // Initialization error
    TCP_LINK_ERR_LISTEN,          // Listen error
    TCP_LINK_ERR_SEND,            // Send error
    TCP_LINK_ERR_TX_BUSY,         // Transmission bus is busy
    TCP_LINK_ERR_TX_TIMEOUT,      // Transmission timeout
    TCP_LINK_ERR_PARAM,           // Invalid parameters
    TCP_LINK_ERR_SOCKET,          // TCP socket error
    TCP_LINK_ERR_NOT_READY        // TCP connection not ready
} tcp_link_err_t;

/** TCP Link TX header structure */
struct tcp_link_tx_hdr {
    uint32_t src_id;       // Identifier of the source (module, queue, etc.)
    uint32_t dest_id;      // Identifier of the destination (module, queue, etc.)
    uint16_t msg_id;       // Identifier of the message (optional for tracking)
    uint16_t payload_len;  // Length of the payload
} __packed;

/** TCP Link TX context structure */
struct tcp_link_tx_ctx {
    uint32_t msg_id;             // Message ID
    uint32_t src_id;             // Identifier of the source
    uint32_t dest_id;            // Identifier of the destination
    bool is_last_msg;            // Flag to indicate if this is the last message
};

#if defined(CONFIG_TX_MSG_QUEUE)
/** Context for TCP TX queue */
struct tcp_tx_q_ctx{
	const char *queue_name;   // Name of the TX queue
	const char *sender_name;  // Name of the sender
    uint32_t is_last_msg;     // Flag to indicate if this is the last message in the queue
    uint32_t src_id;          // Source ID for the message
    uint32_t dest_id;         // Destination ID for the message
    uint32_t last_msg_id;     // Last message ID sent
    size_t msg_total_len;     // Total length of the message
	int *client_fd;           // TCP client file descriptor
    uint8_t tx_buf[TCP_LINK_TX_BUFFER_SIZE]; // Buffer for sending data
};
#endif

/**
 * @brief Initialize the TCP link module.
 * @return TCP_LINK_OK on success, or an error code on failure
 */
tcp_link_err_t tcp_link_init(void);

#ifdef __cplusplus
}
#endif
