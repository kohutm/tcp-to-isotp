#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "tx_queue.h"

LOG_MODULE_REGISTER(tx_queue, CONFIG_TX_MSG_QUEUE_LOG_LEVEL);

K_THREAD_STACK_DEFINE(tx_thread_stack, CONFIG_TX_MSG_QUEUE_THREAD_STACK_SIZE);

typedef struct {
    struct k_thread thread_data;  // Thread data for the TX queue processing thread
    struct k_sem new_data_sem;  // Semaphore to signal new data availability
    struct k_mutex global_queue_list_mutex;  // Mutex for protecting the global queue list
    sys_slist_t global_queue_list;  // Global list of registered TX queues
    atomic_t queue_count;  // Atomic counter for the number of registered queues
    bool queue_inited;  // Flag indicating if the TX queue system is initialized
} tx_queue_sys_t;

static tx_queue_sys_t tx_queue_sys = {
    .queue_count = ATOMIC_INIT(0),
    .queue_inited = false
};

/**
 * @brief Try to increment the queue count atomically.
 * This function checks if the current count is less than the limit,
 * and if so, increments it atomically.
 * @param counter Pointer to the atomic counter.
 * @param limit The maximum limit for the queue count.
 * @return true if the increment was successful, false if the limit was reached.
 */
static inline bool tx_queue_try_increment(atomic_t *counter, int limit) {
    int val;

    do {
        val = atomic_get(counter);
        if (val >= limit) {
            return false;
        }
    } while (!atomic_cas(counter, val, val + 1));

    return true;
}

/**
 * @brief Allocate a message from the TX queue's message pool.
 * This function searches for an unused message slot in the pool,
 * marks it as used, and returns a pointer to the allocated message.
 * @param queue Pointer to the TX queue.
 * @return Pointer to the allocated message, or NULL if no slots are available.
 */
static struct tx_queue_msg* s_alloc_tx_msg(struct tx_queue *queue) { 
    for (int i = 0; i < CONFIG_TX_MSG_QUEUE_DEPTH; ++i) {
        if (queue->used_flags[i] == 0) {
            queue->used_flags[i] = 1;
            return &queue->msg_pool[i];
        }
    }

    return NULL;
}

/**
 * @brief Free a message back to the TX queue's message pool.
 * This function marks the specified message slot as unused and gives the semaphore
 * to indicate that a message slot is available.
 * @param queue Pointer to the TX queue.
 * @param msg Pointer to the message to free.
 */
static void s_free_tx_msg(struct tx_queue *queue, struct tx_queue_msg *msg) {
    for (int i = 0; i < CONFIG_TX_MSG_QUEUE_DEPTH; ++i) {
        if (&queue->msg_pool[i] == msg) {
            queue->used_flags[i] = 0;
            k_sem_give(&queue->msg_sem);
            break;
        }
    }
}

/**
 * @brief Process messages in the TX queue.
 * This function runs in a dedicated thread and processes messages from all registered queues.
 * It waits for new data to be available and then processes each message by calling the user data handler
 * and the send callback if they are set.
 * @param arg1 Unused argument.
 * @param arg2 Unused argument.
 * @param arg3 Unused argument.
 */
static void tx_msg_queue_process(void *arg1, void *arg2, void *arg3) {
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    struct tx_queue *queue;
    struct tx_queue *tmp;
    struct tx_queue_msg *msg;

    while (1) {
        k_sem_take(&tx_queue_sys.new_data_sem, K_FOREVER);

        k_mutex_lock(&tx_queue_sys.global_queue_list_mutex, K_FOREVER);

        SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&tx_queue_sys.global_queue_list, queue, tmp, node) {
            while (true) {
                msg = k_fifo_get(&queue->fifo, K_NO_WAIT);
                if (!msg) break;
                // Call the user data handler if it is set
                if (queue->usr_data_handler) {
                    queue->usr_data_handler(msg, queue->user_data);
                }
                // Call the send callback if it is set
                if (queue->send_cb) {
                    queue->send_cb(msg->data, msg->len, queue->user_data);
                }

                // Free msg
                s_free_tx_msg(queue, msg);
            }
        }

        k_mutex_unlock(&tx_queue_sys.global_queue_list_mutex);
    }
}

/** TX QUEUE Public API functions */
void tx_msg_queue_system_init(void) {
    k_mutex_init(&tx_queue_sys.global_queue_list_mutex);
    sys_slist_init(&tx_queue_sys.global_queue_list);
    k_sem_init(&tx_queue_sys.new_data_sem, 0, K_SEM_MAX_LIMIT);

    k_thread_create(&tx_queue_sys.thread_data, tx_thread_stack,
                    CONFIG_TX_MSG_QUEUE_THREAD_STACK_SIZE,
                    tx_msg_queue_process, NULL, NULL, NULL,
                    CONFIG_TX_MSG_QUEUE_THREAD_PRIO, 0, K_NO_WAIT);

    k_thread_name_set(&tx_queue_sys.thread_data, CONFIG_TX_MSG_QUEUE_THREAD_NAME);

    tx_queue_sys.queue_inited = true;
}

tx_msg_queue_err_t tx_queue_register(const struct tx_queue_register_params *params) {

    if (!tx_queue_sys.queue_inited) {
        LOG_ERR("TX queue system not initialized");
        return TX_MSG_QUEUE_ERR_INIT;
    }

    if (!params || !params->queue || !params->name || !params->send_cb
        || strlen(params->name) >= TX_MSG_QUEUE_MAX_NAME_LEN) {
        LOG_ERR("Invalid parameters for TX queue registration");
        return TX_MSG_QUEUE_ERR_INVALID_PARAM;
    }
    struct tx_queue *queue = params->queue;

    strncpy(queue->name, params->name, TX_MSG_QUEUE_MAX_NAME_LEN);
    queue->name[TX_MSG_QUEUE_MAX_NAME_LEN - 1] = '\0';
    queue->send_cb = params->send_cb;
    queue->usr_data_handler = params->usr_data_handler;
    queue->user_data = params->user_data;

    memset(queue->used_flags, 0, sizeof(queue->used_flags));
    k_fifo_init(&queue->fifo);
    k_sem_init(&queue->msg_sem, CONFIG_TX_MSG_QUEUE_DEPTH, CONFIG_TX_MSG_QUEUE_DEPTH);

    if (!tx_queue_try_increment(&tx_queue_sys.queue_count, CONFIG_TX_MSG_QUEUE_MAX_QUANTITY)) {
        k_mutex_unlock(&tx_queue_sys.global_queue_list_mutex);
        LOG_ERR("TX queue limit (%d) reached", CONFIG_TX_MSG_QUEUE_MAX_QUANTITY);
        return TX_MSG_QUEUE_ERR_NO_SPACE;
    }

    k_mutex_lock(&tx_queue_sys.global_queue_list_mutex, K_FOREVER);
    sys_slist_append(&tx_queue_sys.global_queue_list, &queue->node);
    k_mutex_unlock(&tx_queue_sys.global_queue_list_mutex);

    LOG_INF("Registered TX queue: %s", queue->name);

    return TX_MSG_QUEUE_ERR_OK;
}

tx_msg_queue_err_t tx_queue_unregister(struct tx_queue *queue) {

    if (!tx_queue_sys.queue_inited) {
        LOG_ERR("TX queue system not initialized");
        return TX_MSG_QUEUE_ERR_INIT;
    }

    if (!queue) {
        LOG_ERR("Invalid TX queue for unregistration");
        return TX_MSG_QUEUE_ERR_INVALID_PARAM;
    }

    bool removed = false;

    k_mutex_lock(&tx_queue_sys.global_queue_list_mutex, K_FOREVER);
    removed = sys_slist_find_and_remove(&tx_queue_sys.global_queue_list, &queue->node);
    k_mutex_unlock(&tx_queue_sys.global_queue_list_mutex);

    if (!removed) {
        LOG_ERR("Failed to unregister TX queue '%s'", queue->name);
        return TX_MSG_QUEUE_ERR_UNREGISTER;
    }

    LOG_INF("TX queue '%s' unregistered", queue->name);
    return TX_MSG_QUEUE_ERR_OK;
}

struct tx_queue *tx_queue_get_by_name(const char *name) {

    if (!tx_queue_sys.queue_inited) {
        LOG_ERR("TX queue system not initialized");
        return NULL;
    }

    if (!name || strlen(name) >= TX_MSG_QUEUE_MAX_NAME_LEN) {
        LOG_ERR("Invalid name for TX queue search");
        return NULL;
    }

    struct tx_queue *queue;

    k_mutex_lock(&tx_queue_sys.global_queue_list_mutex, K_FOREVER);
    SYS_SLIST_FOR_EACH_CONTAINER(&tx_queue_sys.global_queue_list, queue, node) {
        if (strcmp(queue->name, name) == 0) {
            k_mutex_unlock(&tx_queue_sys.global_queue_list_mutex);
            return queue;
        }
    }
    k_mutex_unlock(&tx_queue_sys.global_queue_list_mutex);

    LOG_ERR("TX queue '%s' not found", name);
    return NULL;
}

tx_msg_queue_err_t tx_queue_msg_send_ex(struct tx_queue *queue, 
    const void *data, size_t len, uint32_t msg_id, void *usr_ctx) {

    if (!tx_queue_sys.queue_inited) {
        LOG_ERR("TX queue system not initialized");
        return TX_MSG_QUEUE_ERR_INIT;
    }

    if (!queue || !data || len == 0 || len > CONFIG_TX_MSG_QUEUE_PAYLOAD_MAX_SIZE) {
        LOG_ERR("Invalid parameters for TX queue send");
        return TX_MSG_QUEUE_ERR_INVALID_PARAM;
    }

    if (k_sem_take(&queue->msg_sem, K_NO_WAIT) != 0) {
        LOG_ERR("TX queue '%s' is full, cannot send data", queue->name);
        return TX_MSG_QUEUE_ERR_NO_SPACE;
    }

    struct tx_queue_msg *msg = s_alloc_tx_msg(queue);

    if (!msg) {
        k_sem_give(&queue->msg_sem);
        LOG_ERR("No available message slot in TX queue '%s'", queue->name);
        return TX_MSG_QUEUE_ERR_NO_SPACE;
    }

    msg->len = len;
    msg->msg_id = msg_id;
    msg->usr_ctx = usr_ctx;
    memcpy(msg->data, data, len);

    k_fifo_put(&queue->fifo, msg);
    k_sem_give(&tx_queue_sys.new_data_sem);

    LOG_DBG("TX queue '%s': sending %d bytes", queue->name, len);

    return TX_MSG_QUEUE_ERR_OK;
    }

tx_msg_queue_err_t tx_queue_msg_send(struct tx_queue *queue, const void *data, size_t len) {

    return tx_queue_msg_send_ex(queue, data, len, 0, NULL);
}
