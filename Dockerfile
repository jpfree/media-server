# Build stage
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libswresample-dev \
    libsqlite3-dev \
    pkg-config \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Install LibDataChannel
WORKDIR /tmp
RUN git clone -b master --depth 1 https://github.com/paullouisageneau/libdatachannel.git && \
    cd libdatachannel && \
    git submodule update --init --recursive && \
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DNO_EXAMPLES=ON -DNO_TESTS=ON && \
    cmake --build build -j$(nproc) && \
    cmake --install build && \
    mkdir -p /opt/libdatachannel && \
    find /usr -name "libdatachannel.so*" -exec cp -P {} /opt/libdatachannel/ \;

# Build Media Server
WORKDIR /app
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_HTTPS=OFF -DENABLE_RTC=ON && \
    cmake --build build -j$(nproc)

# Runtime stage
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libssl3 \
    libavcodec60 \
    libavformat60 \
    libavutil58 \
    libswresample4 \
    libsqlite3-0 \
    ca-certificates \
    net-tools \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Copy LibDataChannel libraries from builder (arch-neutral staging path)
COPY --from=builder /opt/libdatachannel/ /usr/local/lib/
RUN ldconfig

# Copy application artifacts
WORKDIR /app
COPY --from=builder /app/output .

# Expose ports based on default config
EXPOSE 26080/tcp
EXPOSE 26090/tcp
EXPOSE 26090/udp
EXPOSE 5080/tcp
EXPOSE 5080/udp

# Define volumes for persistent data
VOLUME ["/app/conf", "/app/log", "/app/files"]

# Run the media server
ENTRYPOINT ["./media_server"]
