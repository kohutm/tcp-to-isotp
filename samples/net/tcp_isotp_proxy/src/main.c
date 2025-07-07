/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>

#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_TCP_CMD_HANDLER)
#include "tcp_cmd_handler.h"
#endif
#if defined(CONFIG_TCP_LINK)
#include "tcp_link.h"
#endif
#if defined(CONFIG_TX_MSG_QUEUE)
#include "tx_queue.h"
#endif
#if defined(CONFIG_CAN_ISOTP_PROXY)
#include "isotp_proxy.h"
#endif

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

// Staff for Alive status LED
#define HEART_BEAT_INTERVAL   1000
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static int init_led(void)
{
	if (!gpio_is_ready_dt(&led)) {
		return -ENODEV;
	}

	return gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
}

int main(void) {
	LOG_INF("Ethernet - ISO-TP on STM32 Sample Application! \n%s\n", CONFIG_BOARD_TARGET);

	if (init_led() < 0) {
		LOG_ERR("Failed to initialize LED\n");
		return -1;
	}

#if defined(CONFIG_TX_MSG_QUEUE)
	tx_msg_queue_system_init();
#endif

#if defined(CONFIG_TCP_LINK)
	tcp_link_init();
#endif

#if defined(CONFIG_CAN_ISOTP_PROXY)
	isotp_proxy_init();
#endif

	while (1) {
		gpio_pin_toggle_dt(&led);
		k_msleep(HEART_BEAT_INTERVAL);
	}

	return 0;
}
