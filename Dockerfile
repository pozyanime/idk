FROM ubuntu:20.04

RUN apt-get update && \
    apt-get install -y shellinabox systemd && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

RUN echo 'root:root' | chpasswd

EXPOSE 4200

CMD ["/usr/bin/shellinaboxd", "-t", "-s", "/:LOGIN"]
