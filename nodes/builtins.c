/* nodes/builtins.c -- registers every built-in node type. */
#include "nodes.h"

static const NodeType *all[] = {
    &ndt_generator,
    &ndt_uppercase,
    &ndt_printer,
    &ndt_filter,
    &ndt_filter_re,
    &ndt_logic,
    &ndt_throttle,
    &ndt_delay,
    &ndt_batch,
    &ndt_split,
    &ndt_count,
    &ndt_random,
    &ndt_file,
    &ndt_fwriter,
    &ndt_tee,
    &ndt_null,
    &ndt_http_in,
    &ndt_http_out,
    &ndt_epoll_in,
    &ndt_exec,
#ifdef WITH_LUA
    &ndt_lua,
#endif
#ifdef WITH_MQTT
    &ndt_mqtt,
#endif
#ifdef WITH_GPIO
    &ndt_gpio_in,
    &ndt_gpio_out,
#endif
};

void nodes_register_builtins(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    for (size_t i = 0; i < sizeof(all)/sizeof(all[0]); i++)
        node_register(all[i]);
}
