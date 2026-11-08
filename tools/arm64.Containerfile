FROM registry.fedoraproject.org/fedora:44
RUN dnf install -y --setopt=install_weak_deps=False gcc make liburing-devel && dnf clean all
WORKDIR /src
