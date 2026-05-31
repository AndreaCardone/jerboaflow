# --- build stage -----------------------------------------------------------
# Pinned major version; bump deliberately. Debian slim gives us a recent
# enough gcc, libmosquitto-dev and lua5.4-dev without alpine's musl quirks.
FROM debian:12-slim AS build

ARG WITH_LUA=1
ARG WITH_MQTT=1

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc make libc6-dev pkg-config \
        $(test "$WITH_LUA"  = 1 && echo liblua5.4-dev) \
        $(test "$WITH_MQTT" = 1 && echo libmosquitto-dev) \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile jerboa.h jerboa.c metrics.c main.c ./
COPY nodes/ ./nodes/

RUN make WITH_LUA=${WITH_LUA} WITH_MQTT=${WITH_MQTT}

# --- runtime stage ---------------------------------------------------------
FROM debian:12-slim

ARG WITH_LUA=1
ARG WITH_MQTT=1

# Only the runtime shared libs, not the -dev headers.
RUN apt-get update && apt-get install -y --no-install-recommends \
        $(test "$WITH_LUA"  = 1 && echo liblua5.4-0) \
        $(test "$WITH_MQTT" = 1 && echo libmosquitto1) \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 65532 --no-create-home --shell /usr/sbin/nologin jerboa

COPY --from=build /src/jerboa /usr/local/bin/jerboa

USER 65532:65532
WORKDIR /etc/jerboa

# Metrics endpoint binds to 127.0.0.1 by design. To scrape from another
# container, run a reverse proxy in the same pod/network, or override
# --metrics-port and use docker's network namespace.
EXPOSE 9090

# Default: read /etc/jerboa/flow.conf -- mount your own config there.
#
#   docker run --rm -v $PWD/flow.conf:/etc/jerboa/flow.conf:ro \
#              -p 127.0.0.1:9090:9090 jerboa --metrics-port 9090
ENTRYPOINT ["/usr/local/bin/jerboa", "flow.conf"]
