# INVISV L4Proxy

## What is INVISV L4Proxy?
INVISV **L4Proxy** is a high-performance first-hop MASQUE proxy Network Function Virtualization (NFV) module. INVISV **L4Proxy** provides server-side functionality needed for running a [Multi-Party Relay](https://invisv.com/articles/relay.html) service to protect users' network privacy.

## Installation

### Prerequisites
INVISV **L4Proxy** runs in BESS, the Berkeley Extensible Software Switch. A BESS installation with a compiled `bessd` binary is required before installation.

### Installation Instructions

1. **Clone the Repository**
   ```bash
   git clone https://github.com/Invisv-Privacy/l4proxy.git
   cd l4proxy
   ```

2. **Build BESS on your system**
   INVISV **L4Proxy** runs in BESS, the Berkeley Extensible Software Switch. A BESS directory, including a compiled bessd binary, must be present for the installation script to run properly.

3. **Run the Installer**
   Execute the installation script to begin the installation process:
   ```bash
   sh ./install.sh
   ```

   **Installation Options:**
   ```bash
   sh ./install.sh --help                   # Show help and options
   sh ./install.sh --dependencies           # Only check dependencies and setup
   sh ./install.sh --container              # Only build L4Proxy container
   sh ./install.sh --all                    # Run full installation (default)
   ```

4. **Follow the Prompts**
   The installer will check for dependencies, verify BESS installation, copy **L4Proxy** files, and optionally build a Docker container. Follow any prompts that appear during the installation.

### Scripts Overview

#### Main Installation Scripts

- **install.sh**: The main orchestration script that manages the entire installation process. Supports modular installation with options for dependencies-only, container-only, or full installation.

- **scripts/check_dependencies.sh**: Comprehensive dependency checker that:
  - Verifies Debian-based distribution compatibility
  - Installs required system packages (build tools, Python, libraries)
  - Checks Docker installation and configuration
  - Verifies BESS installation and build status
  - Copies L4Proxy files to the BESS directory

- **scripts/generate_netplan.sh**: Interactive script to generate Netplan configuration files for bonded network interfaces. Features:
  - Configures network interface bonding (802.3ad mode)
  - Supports multiple IP addresses and interfaces
  - Configurable gateway and nameserver settings
  - Optional preview and automatic deployment to `/etc/netplan/`

### Dependencies

The installation automatically checks for and installs the following dependencies:
- curl, make, git
- build-essential, g++, pkg-config
- Python 3 and pip
- libunwind8-dev, liblzma-dev, zlib1g-dev
- libpcap-dev, libssl-dev, libnuma-dev
- Docker (verification only - manual installation required if missing)
- BESS

### Post-Installation

After successful installation:
1. BESS will contain the **L4Proxy** modules and configurations
2. A Docker container with **L4Proxy** will be built (if selected)
3. Optionally configure network bonding using the netplan generator
4. You can now build and run BESS with the INVISV **L4Proxy** module

### Troubleshooting

- Ensure BESS is properly built before running the installer
- Docker must be installed and the user must be in the docker group
- For network configuration issues, use the netplan generator script
- Check that all required dependencies are installed on your Debian-based system

## Example application: L4Proxy with INVISV MASQUE
Along with INVISV MASQUE, available in the [INVISV MASQUE](https://github.com/Invisv-Privacy/masque) repository, INVISV **L4Proxy** can be used to build a full Multi-Party Relay service.

### Example requirements:
- A client running a MASQUE ingress proxy (e.g. INVISV MASQUE Relay HTTP Proxy), which accepts client connections and forwards them to the INVISV **L4Proxy**.
- A server running INVISV **L4Proxy** as a first-hop MASQUE proxy, which forwards traffic to the egress target.
- A server running a MASQUE egress target (e.g. h2o (example available in the INVISV MASQUE repository)), which connects to the destination server on behalf of the client.

### Example setup:
- The MASQUE h2o egress target (192.168.1.3 in the scenario) listens on port 8444 by default. **Note**: this port must match the port configured in the INVISV **L4Proxy** configuration in the container/run_l4_proxy.sh script (e.g., NEXT_HOP_PORT="8444").

  To run the MASQUE h2o egress target, execute the following command on the server in the masque repository:
  ```bash
  docker-compose up -d
  ```

- The INVISV **L4Proxy** server (192.168.1.2 in the scenario) runs in a Docker container on the server and listens on port 443 by default. **Note**: this port must match the port configured in the INVISV **L4Proxy** configuration in the container/run_l4_proxy.sh script (e.g., LISTEN_PORT="443").

  To run the INVISV **L4Proxy** container, execute the following command on the server:
  ```bash
  docker run -d --name l4proxy --privileged --cgroupns=host --net=host -v /sys/bus/pci/drivers:/sys/bus/pci/drivers -v /dev:/dev l4-proxy:latest /run_l4_proxy.sh -a 10514 -b 1 -c 2 -d eth0 -i 192.168.1.2 -n 192.168.1.3
  ```
  **Note**: Adjust the parameters as needed:
  - `-i`: IP address of the INVISV **L4Proxy** server
  - `-n`: IP address of the MASQUE h2o egress target
  - `-d`: Network interface to use (e.g., eth0)
  - `-a`: Port for the INVISV **L4Proxy** BESS control plane
  - `-b`: A unique BESS ID for the BESS instance
  - `-c`: Number of CPU cores to allocate to BESS

- The client (192.168.1.1 in the scenario) running the MASQUE ingress proxy (e.g., INVISV MASQUE Relay HTTP Proxy), which accepts client connections and forwards them to the INVISV **L4Proxy**.

  To run the INVISV MASQUE Relay HTTP Proxy, execute the following command on the client in the masque repository:
  ```bash
  go run ./example/relay-http-proxy -invisvRelay 192.168.1.2 -invisvRelayPort 443 -token fake-token -verbose=true -certDataFile ./testdata/h2o/server.crt 
  ```

  To test the full setup on the client, you can use curl with the `--proxy` option to connect through the MASQUE ingress proxy, INVISV **L4Proxy**, and the MASQUE egress target to reach a destination server:
   In a new terminal, make a request using the proxy:
   ```
   $ curl --proxy http://localhost:32190 -I  https://duckduckgo.com
   HTTP/1.1 200 OK
   Content-Length: 0

   HTTP/2 200 
   server: nginx
   date: Fri, 22 Aug 2025 19:27:11 GMT
   content-type: text/html; charset=UTF-8
   content-length: 65290
   vary: Accept-Encoding
   ```
   🎉🎉🎉🎉🎉🎉🎉🎉🎉🎉
