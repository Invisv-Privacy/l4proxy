#!/bin/bash

# This script orchestrates the installation process for the INVISV L4Proxy.

# Function to display usage information
show_usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Install INVISV L4Proxy components"
    echo ""
    echo "OPTIONS:"
    echo "  -h, --help              Show this help message"
    echo "  -d, --dependencies      Check dependencies, verify BESS, and copy L4Proxy files"
    echo "  -c, --container         Build L4Proxy container (requires dependencies to be checked first)"
    echo "  -a, --all               Run full installation (default)"
    echo ""
    echo "Examples:"
    echo "  $0                      # Run full installation"
    echo "  $0 --dependencies       # Only check dependencies and setup"
    echo "  $0 --container         # Only build container"
    echo "  $0 --all               # Run full installation"
}

# Function to check dependencies
check_dependencies() {
    echo "Checking dependencies..."
    if bash scripts/check_dependencies.sh; then
        # Source a simple script to get BESS_DIR
        if [ -f /tmp/bess_dir_export ]; then
            . /tmp/bess_dir_export
            rm -f /tmp/bess_dir_export
        fi
        echo ""
        return 0
    else
        echo ""
        echo "ERROR: Dependency check failed. Please resolve the issues and try again."
        return 1
    fi
}

# Function to build L4Proxy container
build_l4proxy_container() {
    echo "===== Building L4Proxy container... ====="
    if [ -z "$BESS_DIR" ]; then
        echo "ERROR: BESS_DIR is not set. Cannot build container."
        return 1
    fi
    
    echo "Using BESS_DIR: $BESS_DIR"
    
    # Build the container with the correct BESS_DIR
    if docker build -f container/Dockerfile --build-arg BESS_DIR="$BESS_DIR" -t l4-proxy --no-cache .; then
        echo "L4Proxy container built successfully."
        return 0
    else
        echo "ERROR: Failed to build L4Proxy container."
        return 1
    fi
}

# Function to prompt for yes/no question
prompt_yes_no() {
    local prompt="$1"
    local default="$2"
    while true; do
        printf "%s [%s]: " "$prompt" "$default"
        read input
        input=${input:-$default}
        case $input in
            [Yy]|[Yy][Ee][Ss]) return 0 ;;
            [Nn]|[Nn][Oo]) return 1 ;;
            *) echo "Please answer yes or no." ;;
        esac
    done
}

# Function to offer netplan generation
offer_netplan_generation() {
    echo ""
    echo "===== Network Configuration ====="
    if prompt_yes_no "Would you like to generate a netplan configuration for bonded interfaces?" "n"; then
        if [ -f "scripts/generate_netplan.sh" ]; then
            echo "Running netplan generator..."
            bash scripts/generate_netplan.sh
        else
            echo "ERROR: scripts/generate_netplan.sh not found."
        fi
    else
        echo "Skipping netplan generation."
    fi
}

# Function to run full installation
run_full_installation() {
    echo "Starting INVISV L4Proxy installation..."
    
    # Check dependencies, verify BESS, and copy L4Proxy files
    if ! check_dependencies; then
        exit 1
    fi
    
    # Build L4Proxy container
    if ! build_l4proxy_container; then
        echo "WARNING: Container build failed. You may need to build it manually."
    fi
    
    echo ""
    echo "INVISV L4Proxy setup complete."
    echo "BESS_DIR is set to: ${BESS_DIR}"
    
    # Offer to generate netplan configuration
    offer_netplan_generation
}

# Parse command line arguments
MODE="all"

while [ $# -gt 0 ]; do
    case $1 in
        -h|--help)
            show_usage
            exit 0
            ;;
        -d|--dependencies)
            MODE="dependencies"
            shift
            ;;
        -c|--container)
            MODE="container"
            shift
            ;;
        -a|--all)
            MODE="all"
            shift
            ;;
        *)
            echo "Unknown option: $1"
            show_usage
            exit 1
            ;;
    esac
done

# Execute based on selected mode
case $MODE in
    "dependencies")
        if check_dependencies; then
            echo "Dependencies check and L4Proxy setup completed successfully."
            echo "BESS_DIR is set to: ${BESS_DIR}"
            # Offer to generate netplan configuration
            offer_netplan_generation
            exit 0
        else
            exit 1
        fi
        ;;
    "container")
        # Check if BESS_DIR is already set in environment
        if [ -z "$BESS_DIR" ]; then
            # Check if available from previous run
            if [ -f /tmp/bess_dir_export ]; then
                . /tmp/bess_dir_export
                rm -f /tmp/bess_dir_export
            else
                # Prompt user for BESS_DIR
                echo "BESS_DIR not set."
                printf "Please enter the path to your BESS installation directory: "
                read user_bess_dir
                
                # Expand tilde if present (POSIX compatible)
                case "$user_bess_dir" in
                    \~*)
                        user_bess_dir="$HOME${user_bess_dir#\~}"
                        ;;
                esac
                
                # Trim whitespace
                user_bess_dir=$(echo "$user_bess_dir" | xargs)
                
                if [ -n "$user_bess_dir" ]; then
                    export BESS_DIR="$user_bess_dir"
                else
                    echo "ERROR: No BESS directory specified."
                    exit 1
                fi
                
                # Validate BESS directory exists and has bessd
                if [ ! -d "$BESS_DIR" ]; then
                    echo "ERROR: BESS directory does not exist: $BESS_DIR"
                    exit 1
                fi
                
                if [ ! -f "$BESS_DIR/bin/bessd" ]; then
                    echo "ERROR: BESS is not built. Missing binary: $BESS_DIR/bin/bessd"
                    echo "Please build BESS first:"
                    echo "  cd $BESS_DIR"
                    echo "  ./build.py"
                    exit 1
                fi
                
                echo "Using BESS directory: $BESS_DIR"
            fi
        fi
        
        if build_l4proxy_container; then
            echo "Container build completed successfully."
            echo "BESS_DIR used: ${BESS_DIR}"
            exit 0
        else
            exit 1
        fi
        ;;
    "all")
        run_full_installation
        ;;
    *)
        echo "Invalid mode: $MODE"
        show_usage
        exit 1
        ;;
esac