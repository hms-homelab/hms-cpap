# HMS-CPAP - Multi-stage Docker build
# Produces ~150MB final image with C++ runtime

# =============================================================================
# Stage 1: Builder
# =============================================================================
FROM debian:bookworm-slim AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    ca-certificates \
    libcurl4-openssl-dev \
    libpq-dev \
    libpqxx-dev \
    libssl-dev \
    libjsoncpp-dev \
    && rm -rf /var/lib/apt/lists/*

# Build Paho MQTT C client from source (not in Debian repos)
WORKDIR /deps
RUN git clone --depth 1 --branch v1.3.13 https://github.com/eclipse/paho.mqtt.c.git && \
    cd paho.mqtt.c && \
    cmake -B build -DPAHO_WITH_SSL=ON -DPAHO_BUILD_SHARED=ON -DPAHO_BUILD_STATIC=OFF && \
    cmake --build build -j$(nproc) && \
    cmake --install build

# Build Paho MQTT C++ client from source
RUN git clone --depth 1 --branch v1.4.1 https://github.com/eclipse/paho.mqtt.cpp.git && \
    cd paho.mqtt.cpp && \
    cmake -B build -DPAHO_WITH_SSL=ON -DPAHO_BUILD_SHARED=ON -DPAHO_BUILD_STATIC=OFF && \
    cmake --build build -j$(nproc) && \
    cmake --install build

# Update linker cache after installing Paho libs
RUN ldconfig

# Copy source code
WORKDIR /build
COPY CMakeLists.txt VERSION ./
COPY src/ ./src/
COPY include/ ./include/

# Build HMS-CPAP (disable tests for production image)
RUN mkdir build && cd build && \
    cmake -DBUILD_TESTS=OFF .. && \
    make -j$(nproc) && \
    strip hms_cpap

# =============================================================================
# Stage 2: Runtime
# =============================================================================
FROM debian:bookworm-slim

# Install runtime dependencies only
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    libcurl4 \
    libpq5 \
    libpqxx-6.4 \
    libssl3 \
    libjsoncpp25 \
    && rm -rf /var/lib/apt/lists/*

# Copy Paho MQTT shared libraries from builder
COPY --from=builder /usr/local/lib/libpaho-mqtt* /usr/local/lib/
RUN ldconfig

# Create non-root user for security
RUN useradd -r -u 1000 -m -s /bin/bash cpap

# Copy binary from builder
COPY --from=builder /build/build/hms_cpap /usr/local/bin/hms_cpap
RUN chmod +x /usr/local/bin/hms_cpap

# Create data directories
RUN mkdir -p /data/cpap_archive /tmp/hms-cpap && \
    chown -R cpap:cpap /data /tmp/hms-cpap

# Switch to non-root user
USER cpap
WORKDIR /home/cpap

# Health check endpoint
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -f http://localhost:${HEALTH_CHECK_PORT:-8893}/health || exit 1

# Expose health check port
EXPOSE 8893

# Run HMS-CPAP
ENTRYPOINT ["/usr/local/bin/hms_cpap"]
