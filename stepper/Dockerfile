FROM debian:bookworm-slim
COPY mipsel-linux-muslsf-cross.tgz /tmp/tc.tgz
RUN tar -C /opt -xzf /tmp/tc.tgz && rm /tmp/tc.tgz
ENV PATH="/opt/mipsel-linux-muslsf-cross/bin:${PATH}"
WORKDIR /src
