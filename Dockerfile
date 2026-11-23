# syntax=docker/dockerfile:1
# webby-kong: the page, a ROM-free dk64.wasm and tools/serve.py. No ROM is needed to build; mount yours to play:
#   docker run -p 8080:8080 --env-file .env -v /path/to/dk64.z64:/rom/dk64.z64:ro -v webby-kong-saves:/data \
#     ghcr.io/unstripped/webby-kong
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
# runtime deps of the optional passkey sign-in (WEBBY_KONG_AUTH=1): webauthn + cryptography/cbor2, wheels only
COPY tools/requirements-server.txt /app/requirements-server.txt
RUN pip install --no-cache-dir -r /app/requirements-server.txt
COPY web/ /srv/web/
COPY --from=build /src/build/web/dk64.js /src/build/web/dk64.wasm /srv/web/
ARG VERSION=dev
ARG REVISION=unknown
RUN printf '{"version":"%s","revision":"%s"}\n' "$VERSION" "$REVISION" > /srv/web/version.json
COPY tools/serve.py /app/serve.py
COPY tools/saves.py /app/saves.py
COPY tools/auth /app/auth
# The server reads everything from WEBBY_KONG_* environment variables (the table is in README "Configuration"), so
# `docker run --env-file .env` takes the same file a local `mise run serve` does, and a value there overrides the
# image's. Only the image's own layout is pinned here; the optional passkey gate stays off unless WEBBY_KONG_AUTH=1.
ENV WEBBY_KONG_DIR=/srv/web \
    WEBBY_KONG_BIND=0.0.0.0 \
    WEBBY_KONG_DB=/data/saves.sqlite3 \
    WEBBY_KONG_ROM=/rom
# users, passkeys, sessions and invites live in the same /data volume as the saves.
USER kong
VOLUME /data
EXPOSE 8080
CMD ["python3", "/app/serve.py"]
