ARG BASE_IMAGE
FROM ${BASE_IMAGE}

# OpenSSL's Configure needs the complete Perl core modules on RPM-based images.
RUN yum install -y perl-core && yum clean all
