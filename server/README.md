## scAtteR server

The server component of the `scAtteR` system as described in the paper, "Characterizing Distributed Mobile Augmented Reality Applications at the Edge".

### Dependencies

  - `OpenCV` - tested with version 4.7.0
  - `CUDA` - tested with version 12.1
  - `grpc`
  - `nlohmann/json` for JSON structures in C++


The following depdencies are included as submodules in the repository
  - [`FALCONN`](https://github.com/cao-jacky/FALCONN.git) - CPU LSH, library modified from [FALCONN](https://github.com/FALCONN-LIB/FALCONN)
  - [`CudaSift`](https://github.com/Celebrandil/CudaSift) - CUDA version of SIFT with detection, extraction, matching
  - [`Eigen`](https://github.com/eigenteam/eigen-git-mirror) - Dense data structure
  - [`VLFeat`](https://github.com/vlfeat/vlfeat) - CPU GMM training

### Installation and compilation

Ensure that `OpenCV`, `CUDA`, and `grpc` are installed before following these instructions. There are considerable numbers of tutorials on how to do so, please find a relevant webpage online for these libraries. 

Here are the build instructions to get the `scAtteR` server software succesfully running. VIM is used here, but feel free to use your favourite preferred code editor instead. 

```sh
# Ensure that the nlohmann/json library is installed
sudo apt-get update
sudo apt-get install nlohmann-json3-dev

# Clone the scAtteR repository and the required submodules
git clone --recurse-submodules https://github.com/cao-jacky/scAtteR

# Configure the CudaSift library to replace old dependencies
cd server/lib/cudasift 

vim CMakeLists.txt 

# Update CMake version
:%s/cmake_minimum_required(VERSION 2.6)/cmake_minimum_required(VERSION 3.22)/g 

# Removing unnecessary REQUIRED parameter
:%s/find_package(OpenCV REQUIRED)/find_package(OpenCV)/g 

# Update the NVIDIA GPU architecture sm_xy version according to your hardware https://arnon.dk/matching-sm-architectures-arch-and-gencode-for-various-nvidia-cards/
:%s/sm_35/sm_60/g 

# Allow CudaSift to compile with any gcc version and not just gcc-6
:%s/gcc-6/gcc/g 

# Change CUDA to call a library and not an executable
:%s/cuda_add_executable/cuda_add_library/g 

# Save changes to CMakeLists.txt and quit
:wq

# Compile CudaSift
sed -i 's/executable/library/g' CMakeLists.txt
cmake -D CUDA_TOOLKIT_ROOT_DIR=/usr/local/cuda -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release .
make

# Update VLFeat library to function properly with scAtteR
cd ../vlfeat

vim vl/kmeans.c
:%s/default(none)//g
:wq

# Return to scAtteR server's source files and compile
cd ../../build
make

# When building scAtteR, there may be several errors which occur. This is due to incompatabilities between libraries or NVIDIA GPU hardware, and must be debugged accordingly. We apologise that there cannot be more help with this. 
make 

```
### Running
The executable `server` can be used to run the different available services (`primary`, `sift`, `encoding`, `lsh`, `matching`) of the augmented reality pipeline. The following command can be used to run any of these services. Note the need to add the IP of the `primary`` service as the second parameter. 

```sh
./server service_name primary_service_ip
```

You may notice that the services may not communicate to each other, as the default service IP and port settings need to be changed. This can be done by editing the JSON structure at the top of `server.cpp` file in `src` and changing the `ip` and `port` variables of the servies. The services can be deployed on one machine through localhost or distributed amongst several nodes - the IP and port details should be changed accordingly. 

Here is the service structure and explanations:
```cpp
json_services = {
   {"primary", {"10.38.151.146", "50001"}}, // service name as the key, then a sub-structure which contains its IP and port 
   {"sift", {"10.38.151.146", "51002"}},
   {"encoding", {"10.38.151.146", "50003"}},
   {"lsh", {"10.38.151.146", "50004"}},
   {"matching", {"10.38.151.146", "50005"}}};
```

### Deployment specifications

The server was developed and tested on a machine with the following specifications:

- CPU: Intel Core i7-8700 3.20GHz x 12
- GPU: GeForce RTX 2080 Ti
- Memory: 32GB
- OS: Ubuntu 22.04 LTS

Plus the program was compiled with the following CUDA/GCC/G++ compilers:

- CUDA: 12.1
- GCC: 11.4.0
- G++: 11.4.0




