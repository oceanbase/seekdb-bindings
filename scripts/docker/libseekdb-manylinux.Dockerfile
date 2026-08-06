ARG BASE_IMAGE=quay.io/pypa/manylinux_2_28:2026.03.20-1
FROM ${BASE_IMAGE}

# Native prerequisites shared by seekdb, libseekdb, dependency collection,
# and package verification. Keeping them in a builder image avoids paying the
# EL8 yum setup cost for every release attempt.
RUN yum install -y \
      cargo \
      cpio \
      curl \
      file \
      git \
      libaio \
      libaio-devel \
      openssl-devel \
      patchelf \
      rust \
      wget \
    && yum clean all \
    && rm -rf /var/cache/dnf /var/cache/yum

RUN touch /opt/libseekdb-builder-ready
