#!/bin/bash
#
# HPPLite Integration Test Runner
#
# Uses HD derivation for factory deployer, random keys for tests.
# Master wallet funds derived/random keys with gas.
#
# Requirements:
#   - foundry (cast, forge)
#   - .env with HPPLITE_PRIVATE_KEY (master) and HPPLITE_MNEMONIC
#   - Built test_factory binary
#
# Usage:
#   ./run_integration.sh              # Run tests (deploy factory if needed)
#   ./run_integration.sh --create     # Include rollup creation test
#   ./run_integration.sh --deploy     # Force redeploy factory
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
CONTRACTS_DIR="$PROJECT_DIR/contracts"

# Load .env
if [ -f "$PROJECT_DIR/.env" ]; then
    source "$PROJECT_DIR/.env"
elif [ -f "$SCRIPT_DIR/.env" ]; then
    source "$SCRIPT_DIR/.env"
else
    echo "ERROR: No .env file found"
    echo "Create $PROJECT_DIR/.env with:"
    echo "  HPPLITE_PRIVATE_KEY=0x...  (master wallet)"
    echo "  HPPLITE_MNEMONIC=\"word1 word2 ...\"  (for HD derivation)"
    exit 1
fi

# Defaults
RPC_URL="${HPPLITE_L1_RPC:-https://sepolia.hpp.io}"
CHAIN_ID="${HPPLITE_L1_CHAIN_ID:-181228}"
GAS_AMOUNT="0.01ether"  # Amount to fund test keys

# HD derivation path for factory deployer
# m/44'/60'/0'/0/0 = first account
FACTORY_DERIVATION_PATH="m/44'/60'/0'/0/0"

echo "=== HPPLite Integration Test Runner ==="
echo ""

# Check requirements
command -v cast >/dev/null || { echo "ERROR: cast not found (install foundry)"; exit 1; }
command -v forge >/dev/null || { echo "ERROR: forge not found (install foundry)"; exit 1; }

# Validate master key
if [ -z "$HPPLITE_PRIVATE_KEY" ]; then
    echo "ERROR: HPPLITE_PRIVATE_KEY not set"
    exit 1
fi

MASTER_ADDRESS=$(cast wallet address "$HPPLITE_PRIVATE_KEY")
echo "Master wallet: $MASTER_ADDRESS"

# Check master balance
MASTER_BALANCE=$(cast balance "$MASTER_ADDRESS" --rpc-url "$RPC_URL")
echo "Master balance: $MASTER_BALANCE"
echo ""

# === Factory Deployment ===

# Derive factory deployer from mnemonic (if available) or use deterministic derivation
if [ -n "$HPPLITE_MNEMONIC" ]; then
    echo "Deriving factory deployer from mnemonic..."
    FACTORY_DEPLOYER_KEY=$(cast wallet derive-private-key "$HPPLITE_MNEMONIC" 0)
else
    echo "No mnemonic set, using deterministic derivation from master key..."
    # Simple deterministic: keccak256(master_key || "factory" || 0)
    FACTORY_DEPLOYER_KEY=$(cast keccak "$(echo -n "${HPPLITE_PRIVATE_KEY}factory0" | cast --from-utf8)")
fi

FACTORY_DEPLOYER_ADDRESS=$(cast wallet address "$FACTORY_DEPLOYER_KEY")
echo "Factory deployer: $FACTORY_DEPLOYER_ADDRESS"

# Check if factory needs deployment
FACTORY_ADDRESS_FILE="$SCRIPT_DIR/.factory_address"
DEPLOY_FACTORY=false

if [ "$1" = "--deploy" ]; then
    DEPLOY_FACTORY=true
    shift
elif [ -f "$FACTORY_ADDRESS_FILE" ]; then
    FACTORY_ADDRESS=$(cat "$FACTORY_ADDRESS_FILE")
    echo "Found factory address: $FACTORY_ADDRESS"

    # Verify it exists on chain
    CODE=$(cast code "$FACTORY_ADDRESS" --rpc-url "$RPC_URL" 2>/dev/null || echo "0x")
    if [ "$CODE" = "0x" ]; then
        echo "Factory not found on chain, will redeploy"
        DEPLOY_FACTORY=true
    fi
else
    DEPLOY_FACTORY=true
fi

if [ "$DEPLOY_FACTORY" = true ]; then
    echo ""
    echo "=== Deploying Factory ==="

    # Fund deployer if needed
    DEPLOYER_BALANCE=$(cast balance "$FACTORY_DEPLOYER_ADDRESS" --rpc-url "$RPC_URL")
    if [ "$DEPLOYER_BALANCE" = "0" ]; then
        echo "Funding factory deployer with $GAS_AMOUNT..."
        cast send "$FACTORY_DEPLOYER_ADDRESS" --value "$GAS_AMOUNT" \
            --private-key "$HPPLITE_PRIVATE_KEY" --rpc-url "$RPC_URL" >/dev/null
        echo "Funded."
    fi

    # Deploy factory
    echo "Deploying HPPLiteFactory..."
    cd "$CONTRACTS_DIR"

    DEPLOY_OUTPUT=$(HPPLITE_PRIVATE_KEY="$FACTORY_DEPLOYER_KEY" \
        forge script script/DeployFactory.s.sol \
        --rpc-url "$RPC_URL" --broadcast 2>&1)

    # Extract factory address from output
    FACTORY_ADDRESS=$(echo "$DEPLOY_OUTPUT" | grep "HPPLiteFactory deployed at:" | awk '{print $NF}')

    if [ -z "$FACTORY_ADDRESS" ]; then
        echo "ERROR: Failed to extract factory address"
        echo "$DEPLOY_OUTPUT"
        exit 1
    fi

    echo "$FACTORY_ADDRESS" > "$FACTORY_ADDRESS_FILE"
    echo "Factory deployed at: $FACTORY_ADDRESS"
    cd "$SCRIPT_DIR"
fi

echo ""
echo "=== Generating Random Test Key ==="

# Generate random private key for this test run
TEST_PRIVATE_KEY=$(cast wallet new --json | jq -r '.[0].private_key')
TEST_ADDRESS=$(cast wallet address "$TEST_PRIVATE_KEY")
echo "Test wallet: $TEST_ADDRESS"

# Fund test wallet
echo "Funding test wallet with $GAS_AMOUNT..."
cast send "$TEST_ADDRESS" --value "$GAS_AMOUNT" \
    --private-key "$HPPLITE_PRIVATE_KEY" --rpc-url "$RPC_URL" >/dev/null
echo "Funded."

echo ""
echo "=== Running Integration Tests ==="

# Create temporary .env for test
TEST_ENV="$SCRIPT_DIR/.env.test"
cat > "$TEST_ENV" << EOF
HPPLITE_L1_RPC=$RPC_URL
HPPLITE_FACTORY_ADDRESS=$FACTORY_ADDRESS
HPPLITE_PRIVATE_KEY=$TEST_PRIVATE_KEY
EOF

# Run test
cd "$BUILD_DIR"
cp "$TEST_ENV" .env

./test_factory --create "$@"
TEST_RESULT=$?

# Cleanup
rm -f .env "$TEST_ENV"

echo ""
if [ $TEST_RESULT -eq 0 ]; then
    echo "=== All tests passed ==="
else
    echo "=== Tests failed ==="
fi

exit $TEST_RESULT
