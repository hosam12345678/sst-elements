#!/bin/bash
# Run all B+tree unit tests
# Usage: ./run_unit_tests.sh [test_number]
#   - No argument: run all tests
#   - With number: run specific test (e.g., ./run_unit_tests.sh 3)

cd "$(dirname "$0")"

SST_BIN="/workspaces/sst/bin/sst"

# Color output
GREEN='\033[0;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test files
TESTS=(
    "test_unit_01_empty_tree.py"
    "test_unit_02_leaf_split.py"
    "test_unit_03_internal_split.py"
    "test_unit_04_root_split.py"
    "test_unit_05_search_single_level.py"
    "test_unit_06_search_multi_level.py"
    "test_unit_07_duplicate_keys.py"
)

# Test descriptions
DESCRIPTIONS=(
    "Insert into Empty Tree"
    "Leaf Split (Root Split)"
    "Internal Node Split"
    "Root Split Behavior"
    "Search in Single-Level Tree"
    "Search in Multi-Level Tree"
    "Duplicate Key Insertion"
)

run_test() {
    local test_num=$1
    local test_file=${TESTS[$test_num]}
    local description=${DESCRIPTIONS[$test_num]}
    
    echo -e "${BLUE}════════════════════════════════════════════════════════════════════${NC}"
    echo -e "${BLUE}Running Test $((test_num + 1)): $description${NC}"
    echo -e "${BLUE}File: $test_file${NC}"
    echo -e "${BLUE}════════════════════════════════════════════════════════════════════${NC}"
    echo
    
    if [ -f "$test_file" ]; then
        if $SST_BIN "$test_file" 2>&1; then
            echo -e "\n${GREEN}✓ Test $((test_num + 1)) PASSED${NC}\n"
            return 0
        else
            echo -e "\n${RED}✗ Test $((test_num + 1)) FAILED${NC}\n"
            return 1
        fi
    else
        echo -e "${RED}✗ Test file not found: $test_file${NC}\n"
        return 1
    fi
}

# Main execution
if [ $# -eq 0 ]; then
    # Run all tests
    echo -e "${BLUE}╔════════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║           B+Tree Unit Test Suite - All Tests                      ║${NC}"
    echo -e "${BLUE}║           Configuration: 1 Compute + 1 Memory Server              ║${NC}"
    echo -e "${BLUE}╚════════════════════════════════════════════════════════════════════╝${NC}"
    echo
    
    passed=0
    failed=0
    
    for i in "${!TESTS[@]}"; do
        if run_test "$i"; then
            ((passed++))
        else
            ((failed++))
        fi
        
        # Don't add separator after last test
        if [ $i -lt $((${#TESTS[@]} - 1)) ]; then
            echo -e "${BLUE}────────────────────────────────────────────────────────────────────${NC}"
            echo
        fi
    done
    
    # Summary
    echo -e "${BLUE}════════════════════════════════════════════════════════════════════${NC}"
    echo -e "${BLUE}Test Summary${NC}"
    echo -e "${BLUE}════════════════════════════════════════════════════════════════════${NC}"
    echo -e "Total tests:  ${#TESTS[@]}"
    echo -e "${GREEN}Passed:       $passed${NC}"
    if [ $failed -gt 0 ]; then
        echo -e "${RED}Failed:       $failed${NC}"
    else
        echo -e "Failed:       $failed"
    fi
    echo -e "${BLUE}════════════════════════════════════════════════════════════════════${NC}"
    
    if [ $failed -eq 0 ]; then
        echo -e "${GREEN}✓ All tests passed!${NC}"
        exit 0
    else
        echo -e "${RED}✗ Some tests failed${NC}"
        exit 1
    fi
else
    # Run specific test
    test_num=$((${1} - 1))
    
    if [ $test_num -lt 0 ] || [ $test_num -ge ${#TESTS[@]} ]; then
        echo -e "${RED}Invalid test number. Please specify 1-${#TESTS[@]}${NC}"
        exit 1
    fi
    
    run_test "$test_num"
fi
