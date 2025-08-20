FROM ubuntu:focal

ARG BESS_DIR=src
ENV BESS_DIR=$BESS_DIR

ARG DEBIAN_FRONTEND=noninteractive

# Run the following command (label RUN) to run the container
# bessd port: 10514; traffic port: 8765
LABEL RUN="docker run -d --rm --privileged --cgroupns=host --net=host -v /sys/bus/pci/drivers:/sys/bus/pci/drivers -v /dev:/dev"

RUN apt-get -yq update
RUN apt-get -yq install iptables net-tools iproute2 python3 python3-pip dnsutils
RUN ln -s /usr/bin/python3 /usr/bin/python
RUN pip install --user protobuf==3.20.* grpcio

RUN mkdir -p /bess/core
RUN mkdir -p /bess/bessctl
RUN mkdir -p /bess/bessctl/conf/invisv

COPY container/run_l4_proxy.sh /
COPY ${BESS_DIR}/core/bessd /bess/core/bessd
COPY ${BESS_DIR}/bessctl/*.py /bess/bessctl/
COPY ${BESS_DIR}/bessctl/bessctl /bess/bessctl/
COPY ${BESS_DIR}/pybess /bess/pybess
COPY l4proxy/bessctl/conf/invisv/l4_proxy_deploy_vdev.bess /bess/bessctl/conf/invisv/

# Defaults to a bash shell, you could put your DPDK-based application here.
# CMD ["/run_l4_proxy.sh -a <BESSD_PORT> -c <CORES> -i <SERVICE_IP> -p <SERVICE_PORT> -n <NEXT_HOP_IP> -t <NEXT_HOP_PORT> -u <USER_IP>"]
CMD ["bash"]
