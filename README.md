# ClusterTag
ClusterTag is a novel cluster-based memory allocator that seamlessly integrates with tag-based sanitizers such as HWASan. It aims to simultaneously mitigate tag collisions in both temporal and spatial dimensions while maintaining comparable performance overhead.


## How to Use

### Prerequisites
- ARMv8 or ARMv9 architecture supports Top Byte Ignore (TBI) mechanism
- At least 8GB of RAM and 20GB of free disk space for building LLVM
- Docker installed on your system

### Building ClusterTag

```bash
git clone https://github.com/Yiruma96/ClusterTag-repo.git clustertag
docker build -t clustertag .
docker run -it clustertag
```
The built LLVM with ClusterTag will be available in `/workspace/build` inside the container.

### Compiling Programs with ClusterTag
Inside the container, you can compile your programs using ClusterTag-enhanced Clang:
```bash
/workspace/build/bin/clang -fuse-ld=lld-15 -fsanitize=clustertag your_program.c -o your_program
```

### Runtime Configuration
ClusterTag provides a runtime option to customize its behavior:
- 1/ct_pool_density of the space within a pool is used to store clusters
- The larger the value of ct_pool_density, the lower the probability of tag collisions
- The following evaluations are all conducted with the default density of 5

```bash
# Set randomization density (default: 5)
export ct_pool_density=5
```

## How to Evaluation

We provide comprehensive evaluations in two categories: functional evaluation and security evaluation.

### Functional Evaluation

The functional evaluation verifies ClusterTag's compatibility with real-world applications. We use ClusterTag to compile FFmpeg and perform video format conversion tasks.


```bash
cd /workspace/evaluation/functional_evaluation
./functional_test.sh
```

Expected output:
```
===================================
ClusterTag Functional Evaluation
===================================

1. Extracting FFmpeg source...
   ✓ FFmpeg source extracted successfully

2. Configuring FFmpeg with ClusterTag...
   ✓ FFmpeg configured successfully

3. Building FFmpeg with ClusterTag...
   ✓ FFmpeg built successfully

4. Running functional test...
   ✓ Functional test PASSED: small.gif created successfully

===================================
Functional Evaluation Completed
===================================
```

### Security Evaluation

The security evaluation uses test cases from the Juliet Test Suite to compare the vulnerability detection capabilities of ClusterTag and HWASan across four types of common memory safety violations:

1. **No-adjacent Overflow**: CWE122_Heap_Based_Buffer_Overflow__c_CWE129_rand_01.c
2. **Adjacent Overflow**: CWE124_Buffer_Underwrite__malloc_char_loop_01.c
3. **Double free**: CWE415_Double_Free__malloc_free_char_01.c
4. **Use-after-free**: CWE416_Use_After_Free__malloc_free_int_02.c

```bash
cd /workspace/evaluation/security_evaluation
./security_test.sh
```

This evaluation:
- Compiles test cases with both ClusterTag and HWASan for comparison
- Executes 1000 iterations of each vulnerability test case
- Calculates detection rates and missed vulnerability percentages

Expected output: 
- ClusterTag consistently detects vulnerabilities in all 1000 iterations
- HWASan may exhibit probabilistic false negatives due to tag collisions, with a false negative rate of approximately 1/256
```
=======================================
ClusterTag Security Evaluation
=======================================

1. Building test cases with ClusterTag...
   ✓ ClusterTag test cases compiled successfully

2. Building test cases with HWASan...
   ✓ HWASan test cases compiled successfully

3. Preparing test execution...

4. Running ClusterTag test suite...
   Testing CWE415_Double_Free__malloc_free_char_01 with ClusterTag (1000 iterations)...
     ✓ Results - Missed: 0.00%, Detected: 100.00%
   Testing CWE122_Heap_Based_Buffer_Overflow__c_CWE129_rand_01 with ClusterTag (1000 iterations)...
     ✓ Results - Missed: 0.00%, Detected: 100.00%
   Testing CWE124_Buffer_Underwrite__malloc_char_loop_01 with ClusterTag (1000 iterations)...
     ✓ Results - Missed: 0.00%, Detected: 100.00%
   Testing CWE416_Use_After_Free__malloc_free_int_02 with ClusterTag (1000 iterations)...
     ✓ Results - Missed: 0.00%, Detected: 100.00%

5. Running HWASan test suite...
   Testing CWE415_Double_Free__malloc_free_char_01 with HWASan (1000 iterations)...
     ✓ Results - Missed: 0.30%, Detected: 99.70%
   Testing CWE122_Heap_Based_Buffer_Overflow__c_CWE129_rand_01 with HWASan (1000 iterations)...
     ✓ Results - Missed: 0.30%, Detected: 99.70%
   Testing CWE124_Buffer_Underwrite__malloc_char_loop_01 with HWASan (1000 iterations)...
     ✓ Results - Missed: 0.70%, Detected: 99.30%
   Testing CWE416_Use_After_Free__malloc_free_int_02 with HWASan (1000 iterations)...
     ✓ Results - Missed: 0.50%, Detected: 99.50%

=======================================
Security Evaluation Completed
=======================================
```

## Paper

[ClusterTag: Cluster-based Memory Management for Tag-based Sanitizers]()


## License

This project is based on LLVM 15.0.0 and follows the same licensing terms as the LLVM project. See the individual LICENSE files in the LLVM subdirectories for detailed licensing information.
