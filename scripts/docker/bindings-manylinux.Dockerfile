ARG BASE_IMAGE
FROM ${BASE_IMAGE}

# OpenSSL's Configure needs the complete Perl core modules on RPM-based images.
RUN yum install -y perl-core && yum clean all
RUN /opt/python/cp312-cp312/bin/python -m pip install --no-cache-dir packaging
