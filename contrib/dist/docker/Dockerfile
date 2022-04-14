FROM ubuntu:18.04

ARG USER=docker
ARG PASSWORD=docker

RUN apt-get update && \
  apt-get install -y \
    ftp \
    gcc \
    gdb \
    git \
    librdkafka-dev \
    libssl-dev \
    make \
    net-tools \
    netcat \
    openssl \
    vim \
    zlib1g-dev

RUN apt-get install -y \
    libcompress-raw-zlib-perl \
    libdata-dumper-simple-perl \
    libdatetime-perl \
    libfile-copy-recursive-perl \
    libfile-path-tiny-perl \
    libfile-spec-native-perl \
    libjson-perl \
    libnet-ssh2-perl \
    libnet-ssleay-perl \
    libnet-telnet-perl \
    libposix-2008-perl \
    libtest-unit-perl \
    libtime-hr-perl \
    libwww-perl
RUN PERL_MM_USE_DEFAULT=1 perl -MCPAN -e "CPAN::Shell->notest('install', 'Gzip::Faster')"
RUN PERL_MM_USE_DEFAULT=1 perl -MCPAN -e "CPAN::Shell->notest('install', 'Kafka')"

RUN mkdir -p /var/log/proftpd

RUN useradd -s /bin/bash -m -d "/home/$USER" "$USER" && \
  yes "$PASSWORD" | passwd "$USER"

EXPOSE 2121
CMD ["bash"]
