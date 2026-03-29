# HMS-CPAP - Multi-stage Docker build
# Produces ~100MB final image with C++ runtime + Angular Web UI

# =============================================================================
# Stage 1: Angular UI Builder (optional — skipped if frontend/ not present)
# =============================================================================
FROM node:22-slim AS ui-builder

WORKDIR /ui
COPY frontend/package*.json ./
RUN npm ci --no-audit --no-fund
COPY frontend/ ./
RUN npx ng build --configuration production

# =============================================================================
# Stage 2: C++ Builder
# =============================================================================
FROM debian:trixie-slim AS builder

# Install build dependencies (all from Debian repos, multi-arch safe)
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ca-certificates \
    git \
    libcurl4-openssl-dev \
    libpq-dev \
    libpqxx-dev \
    libssl-dev \
    libjsoncpp-dev \
    libpaho-mqtt-dev \
    libpaho-mqttpp-dev \
    nlohmann-json3-dev \
    libspdlog-dev \
    libsqlite3-dev \
    libdrogon-dev \
    uuid-dev libmariadb-dev libhiredis-dev libbrotli-dev \
    libyaml-cpp-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy source code
WORKDIR /build
COPY CMakeLists.txt VERSION ./
COPY src/ ./src/
COPY include/ ./include/
COPY llm_prompt.txt ./

# Build HMS-CPAP with Web UI support
RUN mkdir build && cd build && \
    cmake -DBUILD_TESTS=OFF -DBUILD_WITH_WEB=ON .. && \
    make -j$(nproc) && \
    strip hms_cpap

# =============================================================================
# Stage 3: Runtime
# =============================================================================
FROM debian:trixie-slim

# Install runtime dependencies only
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    libcurl4t64 \
    libpq5 \
    libpqxx-7.10 \
    libssl3 \
    libjsoncpp26 \
    libpaho-mqtt1.3 \
    libpaho-mqttpp3-1 \
    libspdlog1.15 \
    libfmt10 \
    libsqlite3-0 \
    libdrogon1t64 \
    libtrantor1 \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user for security
RUN useradd -r -u 1000 -m -s /bin/bash cpap

# Copy binary from builder
COPY --from=builder /build/build/hms_cpap /usr/local/bin/hms_cpap
RUN chmod +x /usr/local/bin/hms_cpap

# Copy Angular UI from ui-builder
COPY --from=ui-builder /ui/dist/frontend/browser/ /home/cpap/static/browser/

# Create data directories
RUN mkdir -p /data/cpap_archive /tmp/hms-cpap && \
    chown -R cpap:cpap /data /tmp/hms-cpap /home/cpap/static

# Switch to non-root user
USER cpap
WORKDIR /home/cpap

# Health check endpoint
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -f http://localhost:${HEALTH_CHECK_PORT:-8893}/health || exit 1

# Expose health check / web UI port
EXPOSE 8893

# Run HMS-CPAP
ENTRYPOINT ["/usr/local/bin/hms_cpap"]
