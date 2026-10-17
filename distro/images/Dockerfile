# LING OS 服务端容器镜像（OCI / Docker / Podman）
# 前置：make + scripts/bundle.sh dist，然后把 allbin 包复制为 dist/lingos-container-bundle.tar.gz
#   bash distro/images/build-container.sh 会自动准备该文件
# 构建：docker build -f distro/images/Dockerfile -t lingos-server:latest .
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV LINGOS_ROOT=/LINGOS

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl \
        python3 python3-requests \
        libcurl4 libseccomp2 libsqlite3-0 libmosquitto1 libmicrohttpd12 libssl3 \
        espeak-ng ffmpeg iproute2 procps tini \
    && rm -rf /var/lib/apt/lists/*

COPY dist/lingos-container-bundle.tar.gz /tmp/lingos.tgz
COPY distro/images/entrypoint.sh /opt/lingos/entrypoint.sh

RUN mkdir -p /tmp/lingos_bundle \
    && tar xzf /tmp/lingos.tgz -C /tmp/lingos_bundle --strip-components=1 \
    && bash /tmp/lingos_bundle/install.sh /LINGOS \
    && rm -rf /tmp/lingos_bundle /tmp/lingos.tgz \
    && chmod +x /opt/lingos/entrypoint.sh

EXPOSE 8080 2939
VOLUME ["/LINGOS/data", "/LINGOS/system/config", "/LINGOS/log"]

HEALTHCHECK --interval=30s --timeout=5s --start-period=40s --retries=3 \
    CMD curl -fsS http://127.0.0.1:8080/system/health || exit 1

ENTRYPOINT ["/usr/bin/tini", "--", "/opt/lingos/entrypoint.sh"]
