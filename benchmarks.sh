#!/bin/bash
set -euo pipefail

###############################################################################
# Paths & Config
###############################################################################

ROOT_DIR="$(pwd)"
BUILD_DIR="$ROOT_DIR/build"
BENCH_DIR="$BUILD_DIR/benchmarks"
SUBLOG="$ROOT_DIR/sub.log"
TCP_SERVER_PORT=8080
TCP_TIMEOUT=30

###############################################################################
# Colors
###############################################################################

GREEN='\033[1;32m'
BLUE='\033[1;34m'
YELLOW='\033[1;33m'
RED='\033[1;31m'
NC='\033[0m'

###############################################################################
# Cleanup Function
###############################################################################

cleanup() {
    echo -e "${BLUE}Cleaning up...${NC}"
    rm -f "$SUBLOG" || true
    pkill -f bench_tcp_server || true
    pkill -f bench_tcp_client || true
    pkill -f bench_tcp_mt || true
    sleep 0.1
}
trap cleanup EXIT INT TERM

###############################################################################
# 0. Build Everything
###############################################################################

echo -e "${BLUE}=== 0. Building USRL (Release) ===${NC}"
mkdir -p "$BUILD_DIR"
pushd "$BUILD_DIR" > /dev/null
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"
popd > /dev/null

###############################################################################
# 1. Initialize SHM Core
###############################################################################

echo -e "\n${BLUE}=== 1. Initializing SHM Core ===${NC}"
# Use sudo only if necessary (or rely on user permissions)
if [ -w /dev/shm ]; then
    rm -f /dev/shm/usrl_core || true
else
    sudo rm -f /dev/shm/usrl_core || true
fi

pushd "$BENCH_DIR" > /dev/null
./init_bench || { echo -e "${RED}init_bench failed!${NC}"; exit 1; }
popd > /dev/null

ls -lh /dev/shm/usrl_core && echo -e "${GREEN}✓ SHM Core Ready (256MB)${NC}"

###############################################################################
# 2. SHM Benchmark Helper
###############################################################################

run_shm_test() {
    local name="$1" topic="$2" type="$3" writers="${4:-1}" size="${5:-64}"
    
    echo -e "\n${YELLOW}>>> SHM: $name (${type}, W:$writers, ${size}B) ${NC}"
    
    rm -f "$SUBLOG"
    
    # Subscriber
    pushd "$BENCH_DIR" > /dev/null
    ./bench_sub "$topic" > "$SUBLOG" 2>&1 &
    local sub_pid=$!
    popd > /dev/null
    
    sleep 0.2
    
    # Publisher(s)
    pushd "$BENCH_DIR" > /dev/null
    if [[ "$type" == "SWMR" ]]; then
        timeout 25s ./bench_pub_swmr "$topic" "$size"
    else
        timeout 25s ./bench_pub_mwmr "$topic" "$writers" "$size"
    fi
    popd > /dev/null
    
    sleep 0.2
    kill "$sub_pid" 2>/dev/null || true
    
    echo -e "${GREEN}✓ Subscriber: $(tail -1 "$SUBLOG" 2>/dev/null || echo "No data")${NC}"
}

###############################################################################
# 3. TCP Benchmark Helper (Single Thread)
###############################################################################

run_tcp_test() {
    local name="$1"
    
    echo -e "\n${YELLOW}>>> TCP: $name ${NC}"
    
    # Start TCP Server
    echo -e "${BLUE}[Server] ${NC}./bench_tcp_server $TCP_SERVER_PORT"
    pushd "$BENCH_DIR" > /dev/null
    ./bench_tcp_server "$TCP_SERVER_PORT" &
    local server_pid=$!
    popd > /dev/null
    
    # Wait for server to bind
    for i in {1..20}; do
        if nc -z 127.0.0.1 "$TCP_SERVER_PORT" 2>/dev/null; then
            break
        fi
        sleep 0.1
    done
    
    # Run TCP Client
    echo -e "${BLUE}[Client]${NC} ./bench_tcp_client 127.0.0.1 $TCP_SERVER_PORT"
    pushd "$BENCH_DIR" > /dev/null
    timeout "$TCP_TIMEOUT" ./bench_tcp_client "127.0.0.1" "$TCP_SERVER_PORT"
    local client_rc=$?
    popd > /dev/null
    
    # Cleanup
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    
    if [[ $client_rc -eq 0 ]]; then
        echo -e "${GREEN}✓ TCP Single-Thread Complete${NC}"
    else
        echo -e "${RED}✗ TCP Single-Thread Failed (rc=$client_rc)${NC}"
    fi
}

###############################################################################
# 4. TCP Multi-Threaded Helper (New)
###############################################################################

run_tcp_mt_test() {
    local threads="$1"
    echo -e "\n${YELLOW}>>> TCP: Multi-Threaded ($threads Threads) ${NC}"
    
    # Start TCP Server (It handles multiple clients now!)
    echo -e "${BLUE}[Server] ${NC}./bench_tcp_server $TCP_SERVER_PORT"
    pushd "$BENCH_DIR" > /dev/null
    ./bench_tcp_server "$TCP_SERVER_PORT" &
    local server_pid=$!
    popd > /dev/null
    
    # Wait for server to bind
    for i in {1..20}; do
        if nc -z 127.0.0.1 "$TCP_SERVER_PORT" 2>/dev/null; then
            break
        fi
        sleep 0.1
    done
    
    # Run MT Client
    echo -e "${BLUE}[Client]${NC} ./bench_tcp_mt 127.0.0.1 $TCP_SERVER_PORT $threads"
    pushd "$BENCH_DIR" > /dev/null
    timeout "$TCP_TIMEOUT" ./bench_tcp_mt "127.0.0.1" "$TCP_SERVER_PORT" "$threads"
    local client_rc=$?
    popd > /dev/null
    
    # Cleanup
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    
    if [[ $client_rc -eq 0 ]]; then
        echo -e "${GREEN}✓ TCP MT ($threads) Complete${NC}"
    else
        echo -e "${RED}✗ TCP MT Failed (rc=$client_rc)${NC}"
    fi
}

###############################################################################
# 5. Master Benchmark Matrix
###############################################################################

echo -e "\n${BLUE}=== SHM BENCHMARKS ===${NC}"
run_shm_test "Small Ring"        "small_ring_swmr"   "SWMR" 1 64
run_shm_test "Large Ring"        "large_ring_swmr"   "SWMR" 1 64  
run_shm_test "Huge Messages"     "huge_msg_swmr"     "SWMR" 1 8192
run_shm_test "MWMR Standard"     "mwmr_std"          "MWMR" 4 64
run_shm_test "MWMR Contention"   "mwmr_contention"   "MWMR" 8 64

echo -e "\n${BLUE}=== TCP BENCHMARKS ===${NC}"
run_tcp_test "Single Thread Request/Response"
run_tcp_mt_test 4
run_tcp_mt_test 8

###############################################################################
# 6. Summary
###############################################################################

echo -e "\n${GREEN} BENCHMARK SUITE COMPLETE! ${NC}"
echo -e "${BLUE} SHM Logs: $SUBLOG${NC}"
echo -e "${BLUE}🔧 SHM: /dev/shm/usrl_core${NC}"
ls -lh /dev/shm/usrl_core 2>/dev/null || true
