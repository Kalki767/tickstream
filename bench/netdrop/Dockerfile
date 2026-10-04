# Runtime-only image for the network-drop test (bench/netdrop.sh). The
# client binary is mounted from the host build; this image just supplies its
# shared libraries plus iptables, which runs inside the container's own
# network namespace (needs --cap-add NET_ADMIN, nothing on the host changes).
FROM ubuntu:24.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates libssl3t64 libpq5 libasan8 libubsan1 iptables \
 && rm -rf /var/lib/apt/lists/*
