# syntax=docker/dockerfile:1
# webby-kong: the page, a ROM-free dk64.wasm and tools/serve.py. No ROM is needed to build; mount yours to play:
#   docker run -p 8080:8080 -v /path/to/dk64.z64:/rom/dk64.z64:ro -v webby-kong-saves:/data ghcr.io/mjquinlan2000/webby-kong
# The wasm is architecture-independent: it is built once on the build platform.
FROM --platform=$BUILDPLATFORM emscripten/emsdk:6.0.13 AS build
RUN apt-get update && apt-get install -y --no-install-recommends git ninja-build python3-venv \
    && rm -rf /var/lib/apt/lists/*
COPY tools/requirements.txt /tmp/requirements.txt
RUN python3 -m venv /opt/venv && /opt/venv/bin/pip install --no-cache-dir 'cmake>=3.24,<5' -r /tmp/requirements.txt
ENV PATH=/opt/venv/bin:$PATH
WORKDIR /src
COPY . .
RUN test -f decomp/src/boot/main.c || { echo "decomp/ is empty: git submodule update --init"; exit 1; }
RUN python tools/prepare.py all \
    && emcmake cmake -S . -B build/web -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    && cmake --build build/web --target dk64

FROM python:3.12-slim
RUN useradd --system --uid 10001 --no-create-home kong && mkdir -p /data /rom && chown kong /data
COPY web/ /srv/web/
COPY --from=build /src/build/web/dk64.js /src/build/web/dk64.wasm /srv/web/
COPY tools/serve.py /app/serve.py
USER kong
VOLUME /data
EXPOSE 8080
CMD ["python3", "/app/serve.py", "--dir", "/srv/web", "--bind", "0.0.0.0", "--port", "8080", "--db", "/data/saves.sqlite3", "--rom", "/rom"]
