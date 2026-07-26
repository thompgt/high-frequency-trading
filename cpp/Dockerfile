# Container image for the C++ engine.
#
# Two stages: build with a full toolchain, ship a runtime image with none. The
# engine has no dependencies beyond libstdc++, so the final image is a compiler
# output and a config file.

FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      g++ make ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile ./
COPY include ./include
COPY src ./src
COPY tests ./tests
COPY bench ./bench

# Build with -Werror (the Makefile default) and run the unit tests as part of
# the image build. An image that does not pass its own tests must not exist.
RUN make -j"$(nproc)" && ./build/hft_tests

# --------------------------------------------------------------------------

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
      libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

# Run as a normal user: nothing here needs root, and the engine writes only to
# its own output and journal directories.
RUN useradd --system --create-home --uid 10001 hft
WORKDIR /app

COPY --from=build /src/build/hft_engine /usr/local/bin/hft_engine
COPY config ./config

# Mount volumes over these to keep artefacts and, more importantly, the
# journal. A journal inside the container layer is lost with the container --
# which defeats the entire point of having one.
RUN mkdir -p /app/out /var/lib/hft && chown -R hft:hft /app/out /var/lib/hft
VOLUME ["/app/out", "/var/lib/hft"]

USER hft

# SIGTERM must reach the engine so it can checkpoint and write its
# end-of-session marker; without that, every container stop looks like a crash
# to the next start. Exec form (no shell) is what makes that work.
STOPSIGNAL SIGTERM

ENTRYPOINT ["hft_engine"]
CMD ["--config", "config/engine.conf", "--out-dir", "/app/out"]
