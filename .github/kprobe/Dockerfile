# Mirrors the build-linux job of pgmoneta's .github/workflows/ci.yml (clang, Debug, PG 15)
FROM rockylinux/rockylinux:10
RUN dnf -y install dnf-plugins-core && dnf config-manager --set-enabled crb && dnf -y install epel-release \
 && dnf -y install git gcc clang clang-tools-extra cmake make libev libev-devel openssl openssl-devel \
    systemd systemd-devel zlib zlib-devel libzstd libzstd-devel lz4 lz4-devel libssh libssh-devel libatomic \
    python3-docutils bzip2 bzip2-devel libarchive libarchive-devel libyaml-devel check check-devel llvm \
    libasan libubsan net-tools procps-ng ncurses-devel util-linux sudo which zstd gdb strace iproute \
 && dnf install -y https://download.postgresql.org/pub/repos/yum/reporpms/EL-10-$(uname -m)/pgdg-redhat-repo-latest.noarch.rpm \
 && (dnf -qy module disable postgresql 2>/dev/null || true) \
 && dnf install -y postgresql15 postgresql15-server postgresql15-contrib postgresql15-libs postgresql15-devel \
 && dnf clean all
ENV PATH=/usr/pgsql-15/bin:$PATH
RUN cd /tmp && git clone --branch main --single-branch --depth 1 https://github.com/pgmoneta/pgmoneta_ext.git \
 && mkdir pgmoneta_ext/build && cd pgmoneta_ext/build && cmake -DDOCS=false .. && make && make install
RUN useradd -m -s /bin/bash runner && echo "runner ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers
