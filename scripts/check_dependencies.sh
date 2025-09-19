#!/bin/bash

# Check if running on Debian-based distribution (Debian, Ubuntu, Linux Mint, etc.)
check_distribution() {
    echo ""
    echo "===== Checking System Compatibility ====="
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        
        # Check if it's a Debian-based system by looking for apt
        if ! command -v apt-get >/dev/null 2>&1; then
            echo "Error: This installer requires a Debian-based distribution."
            echo "Detected: $PRETTY_NAME"
            return 1
        fi
        
        echo "Debian-based distribution detected: $PRETTY_NAME"
        return 0
    else
        echo "Error: Cannot determine OS distribution. Make sure you're running a Debian-based system."
        return 1
    fi
}

# Function to check if a command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Function to install dependencies using apt-get
install_dependencies() {
    echo ""
    echo "===== Installing Required Packages ====="
    
    # List of required packages
    local packages=(
        "curl" 
        "make"
        "git"
        "build-essential"
        "python3"
        "python3-pip"
        "python3-scapy"
        "libnuma-dev"
        "apt-transport-https"
        "ca-certificates"
        "g++" 
        "pkg-config"
        "libunwind8-dev"
        "liblzma-dev"
        "zlib1g-dev"
        "libpcap-dev"
        "libssl-dev"
        "python-is-python3"
        "libgflags-dev"
        "libgoogle-glog-dev"
        "libgraph-easy-perl"
        "libgtest-dev"
        "libgrpc++-dev"
        "libprotobuf-dev"
        "libc-ares-dev"
        "libbenchmark-dev"
        "libgtest-dev"
        "protobuf-compiler-grpc"
    )
    
    echo "The following packages will be installed: ${packages[*]}"
    
    # Update package lists
    echo "Updating package lists..."
    if ! sudo apt-get update; then
        echo "Failed to update package lists. Please check your internet connection."
        return 1
    fi
    
    # Install packages
    echo "Installing packages..."
    if ! sudo apt-get install -y "${packages[@]}"; then
        echo "Failed to install some packages. Please install them manually."
        return 1
    fi
    
    echo "All required packages installed successfully."
    return 0
}

# Function to check Docker installation and setup
check_docker() {
    echo ""
    echo "===== Checking Docker Installation ====="
    
    # Check if Docker is installed
    if ! command_exists docker; then
        echo "ERROR: Docker is not installed."
        echo "Please install Docker before proceeding:"
        echo "  sudo apt-get update"
        echo "  sudo apt-get install -y docker.io"
        echo "  sudo systemctl start docker"
        echo "  sudo systemctl enable docker"
        echo "  sudo usermod -aG docker \$USER"
        echo ""
        echo "After installation, log out and back in, then run this script again."
        return 1
    fi
    
    echo "Docker is installed."
    
    # Check if Docker daemon is running
    if ! sudo docker info >/dev/null 2>&1; then
        echo "ERROR: Docker daemon is not running."
        echo "Please start Docker:"
        echo "  sudo systemctl start docker"
        echo "  sudo systemctl enable docker"
        return 1
    fi
    
    echo "Docker daemon is running."
    
    # Check if user is in docker group
    if ! groups "$USER" | grep -q docker; then
        echo "ERROR: User $USER is not in the docker group."
        echo "Please add your user to the docker group:"
        echo "  sudo usermod -aG docker \$USER"
        echo ""
        echo "After running this command, log out and back in, then run this script again."
        return 1
    fi
    
    echo "User $USER is in the docker group."
    echo "Docker is properly configured."
    return 0
}

# Function to check BESS installation and build status
check_bess() {
    echo ""
    echo "===== Checking BESS Installation ====="
    
    # Check if BESS_DIR environment variable is set
    if [ -n "$BESS_DIR" ]; then
        echo "Using BESS directory from environment variable: $BESS_DIR"
    else
        # Default locations to check
        local default_locations=(
            "./bess"
            "../bess"
            "$HOME/bess"
            "/opt/bess"
        )
        
        echo "BESS_DIR not set. Checking default locations..."
        
        # Check default locations
        for location in "${default_locations[@]}"; do
            if [ -d "$location" ] && [ -f "$location/bin/bessd" ]; then
                echo "Found BESS installation at: $location"
                export BESS_DIR="$location"
                break
            fi
        done
        
        # If not found in default locations, ask user
        if [ -z "$BESS_DIR" ]; then
            echo "BESS not found in default locations."
            read -e -p "Please enter the path to your BESS installation directory: " user_bess_dir
            
            # Expand tilde if present
            user_bess_dir="${user_bess_dir/#\~/$HOME}"
            
            # Trim whitespace
            user_bess_dir=$(echo "$user_bess_dir" | xargs)
            
            if [ -n "$user_bess_dir" ]; then
                export BESS_DIR="$user_bess_dir"
            else
                echo "ERROR: No BESS directory specified."
                return 1
            fi
        fi
    fi
    
    # Validate BESS directory exists
    if [ ! -d "$BESS_DIR" ]; then
        echo "ERROR: BESS directory does not exist: $BESS_DIR"
        echo "Please ensure BESS is installed and the path is correct."
        return 1
    fi
    
    echo "Checking BESS directory: $BESS_DIR"
    
    # Check if BESS is built (bessd binary exists)
    if [ ! -f "$BESS_DIR/bin/bessd" ]; then
        echo "ERROR: BESS is not built. Missing binary: $BESS_DIR/bin/bessd"
        echo "Please build BESS first:"
        echo "  cd $BESS_DIR"
        echo "  ./build.py"
        return 1
    fi
    
    echo "BESS binary found: $BESS_DIR/bin/bessd"
    
    # Check if bessctl exists
    if [ ! -f "$BESS_DIR/bin/bessctl" ]; then
        echo "WARNING: bessctl not found at $BESS_DIR/bin/bessctl"
        echo "This may indicate an incomplete BESS build."
    else
        echo "BESS control utility found: $BESS_DIR/bin/bessctl"
    fi
    
    # Verify the binary is executable
    if [ ! -x "$BESS_DIR/bin/bessd" ]; then
        echo "ERROR: BESS binary is not executable: $BESS_DIR/bin/bessd"
        return 1
    fi
    
    echo "BESS is properly installed and built."
    echo "BESS_DIR set to: $BESS_DIR"
    return 0
}

# Function to copy L4Proxy files to BESS directory
copy_l4proxy_files() {
    echo ""
    echo "===== Copying L4Proxy Files to BESS Directory ====="
    
    # Get the root directory of the L4Proxy project (go up from scripts directory)
    L4PROXY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    
    echo "L4Proxy directory: $L4PROXY_DIR"
    echo "BESS directory: $BESS_DIR"
    
    # Create the needed subdirectories in BESS
    echo "Creating L4Proxy directories in BESS..."
    mkdir -p "$BESS_DIR/core/modules"
    mkdir -p "$BESS_DIR/bessctl/conf/invisv"
    mkdir -p "$BESS_DIR/bessctl/conf/module_tests"
    mkdir -p "$BESS_DIR/protobuf"
    
    # Copy module files
    echo "Copying L4Proxy module files..."
    if [ -d "$L4PROXY_DIR/l4proxy/core/modules" ]; then
        cp -r "$L4PROXY_DIR/l4proxy/core/modules/"* "$BESS_DIR/core/modules/"
        echo "L4Proxy module files copied successfully."
    else
        echo "Warning: L4Proxy module directory not found at $L4PROXY_DIR/l4proxy/core/modules"
    fi
    
    # Copy configuration files
    echo "Copying L4Proxy configuration files..."
    if [ -d "$L4PROXY_DIR/l4proxy/bessctl/conf/invisv" ]; then
        cp -r "$L4PROXY_DIR/l4proxy/bessctl/conf/invisv/"* "$BESS_DIR/bessctl/conf/invisv/"
        echo "L4Proxy configuration files copied successfully."
    else
        echo "Warning: L4Proxy configuration directory not found at $L4PROXY_DIR/l4proxy/bessctl/conf/invisv"
    fi

    # Copy configuration test files
    echo "Copying L4Proxy configuration test files..."
    if [ -d "$L4PROXY_DIR/l4proxy/bessctl/module_tests" ]; then
        cp -r "$L4PROXY_DIR/l4proxy/bessctl/module_tests/"* "$BESS_DIR/bessctl/conf/module_tests/"
        echo "L4Proxy configuration test files copied successfully."
    else
        echo "Warning: L4Proxy configuration test directory not found at $L4PROXY_DIR/l4proxy/bessctl/module_tests"
    fi
    
    # Copy protobuf files
    echo "Copying L4Proxy protobuf files..."
    if [ -d "$L4PROXY_DIR/l4proxy/protobuf" ]; then
        cp -r "$L4PROXY_DIR/l4proxy/protobuf/"* "$BESS_DIR/protobuf/"
        echo "L4Proxy protobuf files copied successfully."
    else
        echo "Warning: L4Proxy protobuf directory not found at $L4PROXY_DIR/l4proxy/protobuf"
    fi
    
    echo "L4Proxy files have been copied to the BESS directory."
    return 0
}

# Check distribution compatibility
if ! check_distribution; then
    exit 1
fi

# Check BESS installation
if ! check_bess; then
    echo "BESS setup is incomplete. Please resolve the issues above and try again."
    exit 1
fi

# Copy L4Proxy files to BESS directory
if ! copy_l4proxy_files; then
    echo "Warning: Failed to copy some L4Proxy files. You may need to copy them manually."
fi

# Check Docker
if ! check_docker; then
    echo "Docker setup is incomplete. Please resolve the issues above and try again."
    exit 1
fi

# Install dependencies
if ! install_dependencies; then
    echo "Failed to install dependencies. Please resolve the issues and try again."
    exit 1
fi

# Install Python dependencies with pip
install_python_dependencies() {
    echo ""
    echo "===== Installing Python Dependencies ====="
    echo "Installing grpcio and scapy Python packages..."
    
    # First install grpcio and scapy
    if ! pip install --user grpcio scapy; then
        echo "Failed to install grpcio and scapy. Please install them manually."
        return 1
    fi
    
    # Try protobuf 3.20.* first
    echo "Attempting to install protobuf 3.20.*..."
    if pip install --user protobuf==3.20.* 2>/dev/null; then
        echo "Successfully installed protobuf 3.20.*"
    else
        echo "Installation of protobuf 3.20.* failed, trying 3.18.*..."
        
        # Try protobuf 3.18.* as fallback
        if pip install --user protobuf==3.18.* 2>/dev/null; then
            echo "Successfully installed protobuf 3.18.*"
        else
            echo "Failed to install protobuf. Please install a compatible version manually."
            echo "Try: pip install --user protobuf==3.18.* or protobuf==3.20.*"
            return 1
        fi
    fi
    
    echo "Python dependencies installed successfully."
    return 0
}

# Install Python dependencies
if ! install_python_dependencies; then
    echo "Failed to install Python dependencies. Please resolve the issues and try again."
    exit 1
fi

echo "export BESS_DIR=\"$BESS_DIR\"" > /tmp/bess_dir_export

echo "All checks completed successfully."