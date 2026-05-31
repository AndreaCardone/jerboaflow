/* nodes/nodes.h -- declarations for built-in node types and registration. */
#ifndef JERBOA_NODES_H
#define JERBOA_NODES_H

#include "../jerboa.h"

extern const NodeType ndt_generator;
extern const NodeType ndt_uppercase;
extern const NodeType ndt_printer;
extern const NodeType ndt_filter;
extern const NodeType ndt_filter_re;
extern const NodeType ndt_logic;
extern const NodeType ndt_throttle;
extern const NodeType ndt_delay;
extern const NodeType ndt_batch;
extern const NodeType ndt_split;
extern const NodeType ndt_count;
extern const NodeType ndt_random;
extern const NodeType ndt_file;
extern const NodeType ndt_fwriter;
extern const NodeType ndt_tee;
extern const NodeType ndt_null;
extern const NodeType ndt_http_in;
extern const NodeType ndt_http_out;
extern const NodeType ndt_epoll_in;
extern const NodeType ndt_exec;
#ifdef WITH_LUA
extern const NodeType ndt_lua;
#endif
#ifdef WITH_MQTT
extern const NodeType ndt_mqtt;
#endif
#ifdef WITH_GPIO
extern const NodeType ndt_gpio_in;
extern const NodeType ndt_gpio_out;
#endif

/* Register all built-ins. Idempotent. Call once before flow_load(). */
void nodes_register_builtins(void);

#endif
