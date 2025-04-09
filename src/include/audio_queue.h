#include <switch.h>

/* size of the buffer */
#define AUDIO_QUEUE_SIZE (320 * 50 * 15)
#define AUDIO_QUEU_TIMEOUT_USEC (SWITCH_MAX_INTERVAL * 1000)

/**
 * Audio queue internals
 */
struct audio_queue {
    /** the buffer of audio data */
    switch_buffer_t *buffer;
    /** synchronizes access to queue */
    switch_mutex_t *mutex;
    /** signaling for blocked readers/writers */
    switch_thread_cond_t *cond;
    /** total bytes written */
    switch_size_t write_bytes;
    /** total bytes read */
    switch_size_t read_bytes;
    /** number of bytes reader is waiting for */
    switch_size_t waiting;
    /** name of this queue (for logging) */
    char *name;
    /** optional session uuid associated with this queue (for logging) */
    char *session_uuid;
};
typedef struct audio_queue audio_queue_t;
static switch_status_t audio_queue_create(audio_queue_t **queue, const char *name, const char *session_uuid, switch_memory_pool_t *pool);
static switch_status_t audio_queue_write(audio_queue_t *queue, void *data, switch_size_t *data_len);
static switch_status_t audio_queue_read(audio_queue_t *queue, void *data, switch_size_t *data_len, int block);
static switch_status_t audio_queue_clear(audio_queue_t *queue);
static switch_status_t audio_queue_signal(audio_queue_t *queue);
static switch_status_t audio_queue_destroy(audio_queue_t *queue);


/**
 * Create the audio queue
 *
 * @param audio_queue the created queue
 * @param name the name of this queue (for logging)
 * @param session_uuid optional session associated with this channel
 * @param pool memory pool to allocate queue from
 * @return SWITCH_STATUS_SUCCESS if successful.  SWITCH_STATUS_FALSE if unable to allocate queue
 */
static switch_status_t audio_queue_create(audio_queue_t **audio_queue, const char *name, const char *session_uuid, switch_memory_pool_t *pool) {
    switch_status_t status = SWITCH_STATUS_SUCCESS;
    audio_queue_t *laudio_queue = NULL;
    char *lname = (char *)"";
    char *lsession_uuid = NULL;
    *audio_queue = NULL;

    lname = zstr(name) ? (char *)"" : switch_core_strdup(pool, name);
    lsession_uuid = zstr(session_uuid) ? NULL : switch_core_strdup(pool, session_uuid);

    if (zstr(name)) {
        lname = (char *)"";
    }
    else {
        lname = switch_core_strdup(pool, name);
    }

    if ((laudio_queue = (audio_queue_t *)switch_core_alloc(pool, sizeof(audio_queue_t))) == NULL) {
		printf("(%s) unable to create audio queue\n", lname);
        status = SWITCH_STATUS_FALSE;
        goto done;
    }

    laudio_queue->name = lname;
    laudio_queue->session_uuid = lsession_uuid;

    if (switch_buffer_create(pool, &laudio_queue->buffer, AUDIO_QUEUE_SIZE) != SWITCH_STATUS_SUCCESS) {
		printf("(%s) unable to create audio queue buffer\n", laudio_queue->name);
        status = SWITCH_STATUS_FALSE;
        goto done;
    }

    if (switch_mutex_init(&laudio_queue->mutex, SWITCH_MUTEX_UNNESTED, pool) != SWITCH_STATUS_SUCCESS) {
		printf("(%s) unable to create audio queue mutex\n", laudio_queue->name);
        status = SWITCH_STATUS_FALSE;
        goto done;
    }

    if (switch_thread_cond_create(&laudio_queue->cond, pool) != SWITCH_STATUS_SUCCESS) {
		printf("(%s) unable to create audio queue condition variable\n",
                          laudio_queue->name);
        status = SWITCH_STATUS_FALSE;
        goto done;
    }

    laudio_queue->write_bytes = 0;
    laudio_queue->read_bytes = 0;
    laudio_queue->waiting = 0;
    *audio_queue = laudio_queue;
    printf("(%s) audio queue created\n", laudio_queue->name);

done:

    if (status != SWITCH_STATUS_SUCCESS) {
        audio_queue_destroy(laudio_queue);
    }
    return status;
}

/**
 * Write to the audio queue
 *
 * @param queue the queue to write to
 * @param data the data to write
 * @param data_len the number of octets to write
 * @return SWITCH_STATUS_SUCCESS if data was written, SWITCH_STATUS_FALSE if data can't be written because queue is full
 */
static switch_status_t audio_queue_write(audio_queue_t *queue, void *data, switch_size_t *data_len) {
    switch_status_t status = SWITCH_STATUS_SUCCESS;
    switch_mutex_lock(queue->mutex);
    if (switch_buffer_write(queue->buffer, data, *data_len) > 0) {
        queue->write_bytes = queue->write_bytes + *data_len;
        if (queue->waiting <= switch_buffer_inuse(queue->buffer)) {
            switch_thread_cond_signal(queue->cond);
        }
    }
    else {
        *data_len = 0;      
        status = SWITCH_STATUS_FALSE;
    }
    switch_mutex_unlock(queue->mutex);
    return status;
}

/**
 * Read from the audio queue
 *
 * @param queue the queue to read from
 * @param data the read data
 * @param data_len the amount of data requested / actual amount of data read (returned)
 * @param block 1 if blocking is allowed
 * @return SWITCH_STATUS_SUCCESS if successful.  SWITCH_STATUS_FALSE if no data was requested, or there was a timeout
 * while waiting to read
 */
static switch_status_t audio_queue_read(audio_queue_t *queue, void *data, switch_size_t *data_len, int block) {
    switch_size_t requested = *data_len;
    switch_status_t status = SWITCH_STATUS_SUCCESS;
    switch_mutex_lock(queue->mutex);

    /* allow the initial frame to buffer */
    if (!queue->read_bytes && switch_buffer_inuse(queue->buffer) < requested) {
        *data_len = 0;
        status = SWITCH_STATUS_SUCCESS;
        goto done;
    }

    /* wait for data, if allowed */
    if (block) {
        while (switch_buffer_inuse(queue->buffer) < requested) {
            queue->waiting = requested;
			if (switch_thread_cond_timedwait(queue->cond, queue->mutex, AUDIO_QUEU_TIMEOUT_USEC) ==
				SWITCH_STATUS_TIMEOUT) {
                break;
            }
        }
        queue->waiting = 0;
    }

    if (switch_buffer_inuse(queue->buffer) < requested) {
        requested = switch_buffer_inuse(queue->buffer);
    }
    if (requested == 0) {
        *data_len = 0;
        status = SWITCH_STATUS_FALSE;
        goto done;
    }

    /* read the data */
    *data_len = switch_buffer_read(queue->buffer, data, requested);
    queue->read_bytes = queue->read_bytes + *data_len;

done:

    switch_mutex_unlock(queue->mutex);
    return status;
}

/**
 * Empty the queue
 *
 * @param queue the queue to empty
 * @return SWITCH_STATUS_SUCCESS
 */
static switch_status_t audio_queue_clear(audio_queue_t *queue) {
    switch_mutex_lock(queue->mutex);
    switch_buffer_zero(queue->buffer);
    switch_thread_cond_signal(queue->cond);
    switch_mutex_unlock(queue->mutex);
    queue->read_bytes = 0;
    queue->write_bytes = 0;
    queue->waiting = 0;
    return SWITCH_STATUS_SUCCESS;
}

/**
 * Wake any threads waiting on this queue
 *
 * @param queue the queue to empty
 * @return SWITCH_STATUS_SUCCESS
 */
static switch_status_t audio_queue_signal(audio_queue_t *queue) {
    switch_mutex_lock(queue->mutex);
    switch_thread_cond_signal(queue->cond);
    switch_mutex_unlock(queue->mutex);
    return SWITCH_STATUS_SUCCESS;
}

/**
 * Destroy the audio queue
 *
 * @param queue the queue to clean up
 * @return SWITCH_STATUS_SUCCESS
 */
static switch_status_t audio_queue_destroy(audio_queue_t *queue) {
    if (queue) {
        const char *name = queue->name;
        if (zstr(name)) {
            name = "";
        }
		printf("(%s) audio queue destroyed\n", name);
    }
    return SWITCH_STATUS_SUCCESS;
}