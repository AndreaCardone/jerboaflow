/* nodes/mqtt.c -- MQTT subscribe (source) / publish (sink) via libmosquitto.
 *
 * Config:
 *   <name> mqtt sub <host> <port> <topic>     -- source: emits payloads
 *   <name> mqtt pub <host> <port> <topic>     -- sink:   publishes inputs
 *
 * Sub mode runs the mosquitto network loop in its own thread (loop_start);
 * the on_message callback pushes a Packet onto an internal PQueue. The
 * jerboa source thread polls that inbox and also watches flow->stop, so
 * shutdown is responsive without needing timed condvars.
 *
 * Pub mode is an ordinary worker node: each input is published synchronously
 * and then released. Nothing is forwarded downstream. */

#include "nodes.h"

#include <mosquitto.h>
#include <pthread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Refcounted libmosquitto init/cleanup so a long-running daemon that
 * reloads its flow doesn't leak the library's globals on each cycle.
 * mosquitto_lib_init/cleanup are not themselves refcounted, so we
 * wrap them. */
static pthread_mutex_t g_lib_mtx = PTHREAD_MUTEX_INITIALIZER;
static int             g_lib_refs = 0;

static int mqtt_lib_ref(void) {
    pthread_mutex_lock(&g_lib_mtx);
    int rc = 0;
    if (g_lib_refs == 0) {
        if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS) rc = -1;
    }
    if (rc == 0) g_lib_refs++;
    pthread_mutex_unlock(&g_lib_mtx);
    return rc;
}

static void mqtt_lib_unref(void) {
    pthread_mutex_lock(&g_lib_mtx);
    if (g_lib_refs > 0 && --g_lib_refs == 0)
        mosquitto_lib_cleanup();
    pthread_mutex_unlock(&g_lib_mtx);
}

typedef enum { MODE_SUB, MODE_PUB } Mode;

typedef struct {
    Mode               mode;
    struct mosquitto  *m;
    char               host[128];
    int                port;
    char               topic[128];
    PQueue            *inbox;     /* sub only */
    int                connected; /* pub only: was a connection ever ready */
} Ctx;

/* ---------- mosquitto callbacks (sub) ---------- */

static void on_connect_sub(struct mosquitto *m, void *userdata, int rc) {
    Ctx *c = userdata;
    if (rc != 0) {
        fprintf(stderr, "mqtt: connect failed: %s\n", mosquitto_connack_string(rc));
        return;
    }
    if (mosquitto_subscribe(m, NULL, c->topic, 0) != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "mqtt: subscribe(%s) failed\n", c->topic);
}

static void on_message_sub(struct mosquitto *m, void *userdata,
                           const struct mosquitto_message *msg) {
    (void)m;
    Ctx *c = userdata;
    if (!msg || msg->payloadlen < 0) return;
    Packet *p = packet_new(msg->payload, (size_t)msg->payloadlen);
    if (!p) return;
    if (pqueue_push(c->inbox, p) != 0) packet_release(p);
}

/* ---------- process: sub ---------- */

static Packet *process_sub(Node *self, size_t idx, Packet *in) {
    (void)idx; (void)in;
    Ctx *c = self->ctx;
    /* Blocks on the inbox condvar; on_stop_sub closes it at shutdown,
     * which wakes us with pop returning NULL. */
    return pqueue_pop(c->inbox);
}

static void on_stop_sub(Node *self) {
    Ctx *c = self->ctx;
    if (c->inbox) pqueue_close(c->inbox);
}

/* ---------- process: pub ---------- */

static Packet *process_pub(Node *self, size_t idx, Packet *in) {
    (void)idx;
    if (!in) return NULL;
    Ctx *c = self->ctx;
    int rc = mosquitto_publish(c->m, NULL, c->topic,
                               (int)in->len, in->data, 0, false);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "mqtt/%s: publish failed: %s\n",
                self->name, mosquitto_strerror(rc));
    packet_release(in);
    return NULL;
}

/* ---------- teardown ---------- */

static void ctx_free(void *p) {
    Ctx *c = p;
    if (c->m) {
        mosquitto_loop_stop(c->m, true);
        mosquitto_disconnect(c->m);
        mosquitto_destroy(c->m);
    }
    if (c->inbox) {
        Packet *q;
        while (pqueue_trypop(c->inbox, &q) == 0) packet_release(q);
        pqueue_free(c->inbox);
    }
    free(c);
    mqtt_lib_unref();
}

/* ---------- init ---------- */

static int init(Node *n, const char *args) {
    if (!args || !*args) {
        fprintf(stderr, "jerboa: mqtt needs: sub|pub <host> <port> <topic>\n");
        return -1;
    }
    char mode_s[8] = {0};
    char host[128] = {0};
    int  port = 1883;
    char topic[128] = {0};
    if (sscanf(args, "%7s %127s %d %127s", mode_s, host, &port, topic) != 4) {
        fprintf(stderr, "jerboa: mqtt: expected `sub|pub <host> <port> <topic>`\n");
        return -1;
    }

    Ctx *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    if (strcmp(mode_s, "sub") == 0)      c->mode = MODE_SUB;
    else if (strcmp(mode_s, "pub") == 0) c->mode = MODE_PUB;
    else { fprintf(stderr, "jerboa: mqtt: mode must be sub or pub\n"); free(c); return -1; }
    c->port = port;
    memcpy(c->host,  host,  sizeof(c->host));
    memcpy(c->topic, topic, sizeof(c->topic));

    if (mqtt_lib_ref() < 0) { fprintf(stderr, "mqtt: lib_init failed\n"); free(c); return -1; }
    c->m = mosquitto_new(NULL, true, c);
    if (!c->m) { fprintf(stderr, "mqtt: mosquitto_new failed\n"); mqtt_lib_unref(); free(c); return -1; }

    if (c->mode == MODE_SUB) {
        c->inbox = pqueue_new(1024);
        if (!c->inbox) { mosquitto_destroy(c->m); mqtt_lib_unref(); free(c); return -1; }
        mosquitto_connect_callback_set(c->m, on_connect_sub);
        mosquitto_message_callback_set(c->m, on_message_sub);
        if (mosquitto_connect(c->m, c->host, c->port, 30) != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "mqtt: connect to %s:%d failed\n", c->host, c->port);
            mosquitto_destroy(c->m); pqueue_free(c->inbox); mqtt_lib_unref(); free(c); return -1;
        }
        if (mosquitto_loop_start(c->m) != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "mqtt: loop_start failed\n");
            mosquitto_destroy(c->m); pqueue_free(c->inbox); mqtt_lib_unref(); free(c); return -1;
        }
        n->process         = process_sub;
        n->on_stop         = on_stop_sub;
        n->src_interval_ms = 0;   /* we block inside process_sub ourselves */
    } else {
        if (mosquitto_connect(c->m, c->host, c->port, 30) != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "mqtt: connect to %s:%d failed\n", c->host, c->port);
            mosquitto_destroy(c->m); mqtt_lib_unref(); free(c); return -1;
        }
        if (mosquitto_loop_start(c->m) != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "mqtt: loop_start failed\n");
            mosquitto_destroy(c->m); mqtt_lib_unref(); free(c); return -1;
        }
        n->process = process_pub;
    }

    n->ctx      = c;
    n->ctx_free = ctx_free;
    return 0;
}

const NodeType ndt_mqtt = { "mqtt", init };
