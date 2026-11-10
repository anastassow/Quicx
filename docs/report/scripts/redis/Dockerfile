# Quicx built from this repository, for the Redis comparison.
#   docker build -f docs/report/scripts/redis/Dockerfile -t quicx:local .
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends gcc make libc6-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile quicx.conf ./
COPY quicx ./quicx
RUN make quicxd

FROM ubuntu:24.04
COPY --from=build /src/quicxd /usr/local/bin/quicx
COPY quicx.conf /etc/quicx.conf
