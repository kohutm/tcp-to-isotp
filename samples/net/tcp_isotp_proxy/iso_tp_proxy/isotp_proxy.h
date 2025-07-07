/*
 * Copyright (c) 2025 N-Ix
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file isotp_proxy.h
 * @brief Header file for ISO-TP Proxy functionality.
 * This file contains the declarations for the ISO-TP Proxy API,
 * including error codes, initialization function, and other related types.
 * This proxy allows communication over ISO-TP protocol using a CAN device.
 * It provides an interface for sending and receiving ISO-TP messages
 * and integrates with a TCP link for message transmission.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/**
 * @brief ISO-TP Proxy error codes.
 */
typedef enum {
	ISOTP_PROXY_ERR_OK = 0,         /* No error */
	ISOTP_PROXY_ERR_INIT,           /* Initialization error */
	ISOTP_PROXY_ERR_SEND,           /* Send error */
	ISOTP_PROXY_ERR_RECEIVE,        /* Receive error */
	ISOTP_PROXY_ERR_INVALID_PARAM,  /* Invalid parameter error */
	ISOTP_PROXY_ERR_TIMEOUT         /* Timeout error */
} isotp_proxy_err_t;

/**
 * @brief Initialize the ISO-TP proxy.
 * This function sets up the ISO-TP proxy, initializes the CAN device,
 * and starts the RX thread for handling incoming ISO-TP messages.
 * @return ISOTP_PROXY_ERR_OK on success, or an error code on failure.
 */
isotp_proxy_err_t isotp_proxy_init(void);

#ifdef __cplusplus
}
#endif
