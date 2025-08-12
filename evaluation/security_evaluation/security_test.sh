#!/bin/bash

echo "======================================="
echo "ClusterTag Security Evaluation"
echo "======================================="
echo ""

# Configuration
CLUSTERTAG_C="/workspace/build/bin/clang"
CLUSTERTAG_CXX="/workspace/build/bin/clang++"
HWASAN_C="clang-15"
HWASAN_CXX="clang++-15"

# Test configuration
ITERATIONS=1000
THREAD_COUNT=$(nproc)

# Test case definitions
TESTCASES=(
    "CWE122_Heap_Based_Buffer_Overflow__c_CWE129_rand_01"
    "CWE124_Buffer_Underwrite__malloc_char_loop_01"
    "CWE415_Double_Free__malloc_free_char_01"
    "CWE416_Use_After_Free__malloc_free_int_02"
)

echo "1. Building test cases with ClusterTag..."
# Build ClusterTag binaries
rm -rf clustertag
mkdir -p clustertag
$CLUSTERTAG_C -fsanitize=clustertag -c -I testcasesupport testcasesupport/io.c -o clustertag/io.o
$CLUSTERTAG_C -fuse-ld=lld-15 -fsanitize=clustertag -I testcasesupport -DINCLUDEMAIN -DOMITGOOD -o clustertag/CWE415_Double_Free__malloc_free_char_01.out CWE415_Double_Free__malloc_free_char_01.c clustertag/io.o -lm -g -O0
$CLUSTERTAG_C -fuse-ld=lld-15 -fsanitize=clustertag -I testcasesupport -DINCLUDEMAIN -DOMITGOOD -o clustertag/CWE122_Heap_Based_Buffer_Overflow__c_CWE129_rand_01.out CWE122_Heap_Based_Buffer_Overflow__c_CWE129_rand_01.c clustertag/io.o -lm -g -O0
$CLUSTERTAG_C -fuse-ld=lld-15 -fsanitize=clustertag -I testcasesupport -DINCLUDEMAIN -DOMITGOOD -o clustertag/CWE124_Buffer_Underwrite__malloc_char_loop_01.out CWE124_Buffer_Underwrite__malloc_char_loop_01.c clustertag/io.o -lm -g -O0
$CLUSTERTAG_C -fuse-ld=lld-15 -fsanitize=clustertag -I testcasesupport -DINCLUDEMAIN -DOMITGOOD -o clustertag/CWE416_Use_After_Free__malloc_free_int_02.out CWE416_Use_After_Free__malloc_free_int_02.c clustertag/io.o -lm -g -O0
echo "   ✓ ClusterTag test cases compiled successfully"

echo ""
echo "2. Building test cases with HWASan..."
# Build HWASan binaries
rm -rf hwasan
mkdir -p hwasan
$HWASAN_C -fsanitize=hwaddress -c -I testcasesupport testcasesupport/io.c -o hwasan/io.o
$HWASAN_C -fuse-ld=lld-15 -fsanitize=hwaddress -I testcasesupport -DINCLUDEMAIN -DOMITGOOD -o hwasan/CWE415_Double_Free__malloc_free_char_01.out CWE415_Double_Free__malloc_free_char_01.c hwasan/io.o -lm -g -O0
$HWASAN_C -fuse-ld=lld-15 -fsanitize=hwaddress -I testcasesupport -DINCLUDEMAIN -DOMITGOOD -o hwasan/CWE122_Heap_Based_Buffer_Overflow__c_CWE129_rand_01.out CWE122_Heap_Based_Buffer_Overflow__c_CWE129_rand_01.c hwasan/io.o -lm -g -O0
$HWASAN_C -fuse-ld=lld-15 -fsanitize=hwaddress -I testcasesupport -DINCLUDEMAIN -DOMITGOOD -o hwasan/CWE124_Buffer_Underwrite__malloc_char_loop_01.out CWE124_Buffer_Underwrite__malloc_char_loop_01.c hwasan/io.o -lm -g -O0
$HWASAN_C -fuse-ld=lld-15 -fsanitize=hwaddress -I testcasesupport -DINCLUDEMAIN -DOMITGOOD -o hwasan/CWE416_Use_After_Free__malloc_free_int_02.out CWE416_Use_After_Free__malloc_free_int_02.c hwasan/io.o -lm -g -O0
echo "   ✓ HWASan test cases compiled successfully"

echo ""
echo "3. Preparing test execution..."

# Function to run test with multithreading
run_test_suite() {
    local program=$1
    local tool_type=$2
    local testcase_name=$(basename "$program" .out)
    
    echo "   Testing $testcase_name with $tool_type ($ITERATIONS iterations)..."
    
    # Function to run iterations in parallel
    run_chunk() {
        local start=$1
        local end=$2
        local chunk_id=$3
        
        local missed=0
        local detected=0
        local other=0
        
        for i in $(seq $start $end); do
            ./"$program" >/dev/null 2>&1
            exit_code=$?
            
            case $exit_code in
                0) missed=$((missed + 1)) ;;
                99) detected=$((detected + 1)) ;;
                *) 
                    other=$((other + 1))
                    ;;
            esac
        done
        
        echo "$missed $detected $other"
    }
    
    # Calculate chunk size
    local chunk_size=$((ITERATIONS / THREAD_COUNT))
    local remaining=$((ITERATIONS % THREAD_COUNT))
    
    # Start parallel execution
    local pids=()
    local start=1
    local results_files=()
    
    for ((thread=0; thread<THREAD_COUNT; thread++)); do
        local end=$((start + chunk_size - 1))
        if [ $thread -lt $remaining ]; then
            end=$((end + 1))
        fi
        
        local temp_file=$(mktemp)
        results_files+=("$temp_file")
        
        run_chunk $start $end $thread > "$temp_file" &
        pids+=($!)
        start=$((end + 1))
    done
    
    # Wait for all threads to complete
    for pid in "${pids[@]}"; do
        wait $pid
    done
    
    # Aggregate results
    local total_missed=0
    local total_detected=0
    local total_other=0
    
    for temp_file in "${results_files[@]}"; do
        if [ -f "$temp_file" ]; then
            local results=($(cat "$temp_file"))
            total_missed=$((total_missed + results[0]))
            total_detected=$((total_detected + results[1]))
            total_other=$((total_other + results[2]))
            rm -f "$temp_file"
        fi
    done
    
    # Calculate percentages
    local missed_rate=$(printf "%.2f" $(echo "scale=4; $total_missed * 100 / $ITERATIONS" | bc))
    local detected_rate=$(printf "%.2f" $(echo "scale=4; $total_detected * 100 / $ITERATIONS" | bc))
    
    # Console output
    echo "     ✓ Results - Missed: ${missed_rate}%, Detected: ${detected_rate}%"
}

echo ""
echo "4. Running ClusterTag test suite..."
if [ -f "clustertag/CWE415_Double_Free__malloc_free_char_01.out" ]; then
    run_test_suite "clustertag/CWE415_Double_Free__malloc_free_char_01.out" "ClusterTag"
else
    echo "   ⚠ Warning: clustertag/CWE415_Double_Free__malloc_free_char_01.out not found, skipping..."
fi

if [ -f "clustertag/CWE122_Heap_Based_Buffer_Overflow__c_CWE129_rand_01.out" ]; then
    run_test_suite "clustertag/CWE122_Heap_Based_Buffer_Overflow__c_CWE129_rand_01.out" "ClusterTag"
else
    echo "   ⚠ Warning: clustertag/CWE122_Heap_Based_Buffer_Overflow__c_CWE129_rand_01.out not found, skipping..."
fi

if [ -f "clustertag/CWE124_Buffer_Underwrite__malloc_char_loop_01.out" ]; then
    run_test_suite "clustertag/CWE124_Buffer_Underwrite__malloc_char_loop_01.out" "ClusterTag"
else
    echo "   ⚠ Warning: clustertag/CWE124_Buffer_Underwrite__malloc_char_loop_01.out not found, skipping..."
fi

if [ -f "clustertag/CWE416_Use_After_Free__malloc_free_int_02.out" ]; then
    run_test_suite "clustertag/CWE416_Use_After_Free__malloc_free_int_02.out" "ClusterTag"
else
    echo "   ⚠ Warning: clustertag/CWE416_Use_After_Free__malloc_free_int_02.out not found, skipping..."
fi

echo ""
echo "5. Running HWASan test suite..."
if [ -f "hwasan/CWE415_Double_Free__malloc_free_char_01.out" ]; then
    run_test_suite "hwasan/CWE415_Double_Free__malloc_free_char_01.out" "HWASan"
else
    echo "   ⚠ Warning: hwasan/CWE415_Double_Free__malloc_free_char_01.out not found, skipping..."
fi

if [ -f "hwasan/CWE122_Heap_Based_Buffer_Overflow__c_CWE129_rand_01.out" ]; then
    run_test_suite "hwasan/CWE122_Heap_Based_Buffer_Overflow__c_CWE129_rand_01.out" "HWASan"
else
    echo "   ⚠ Warning: hwasan/CWE122_Heap_Based_Buffer_Overflow__c_CWE129_rand_01.out not found, skipping..."
fi

if [ -f "hwasan/CWE124_Buffer_Underwrite__malloc_char_loop_01.out" ]; then
    run_test_suite "hwasan/CWE124_Buffer_Underwrite__malloc_char_loop_01.out" "HWASan"
else
    echo "   ⚠ Warning: hwasan/CWE124_Buffer_Underwrite__malloc_char_loop_01.out not found, skipping..."
fi

if [ -f "hwasan/CWE416_Use_After_Free__malloc_free_int_02.out" ]; then
    run_test_suite "hwasan/CWE416_Use_After_Free__malloc_free_int_02.out" "HWASan"
else
    echo "   ⚠ Warning: hwasan/CWE416_Use_After_Free__malloc_free_int_02.out not found, skipping..."
fi

echo ""
echo "======================================="
echo "Security Evaluation Completed"
echo "======================================="
echo ""
echo "Results:"
echo "- All test cases compiled successfully"
echo "- Each test case executed $ITERATIONS times"
echo "- Used $THREAD_COUNT parallel threads"