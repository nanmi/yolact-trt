cmake_minimum_required(VERSION 3.10)

project(yolact)

add_definitions(-std=c++11)

option(CUDA_USE_STATIC_CUDA_RUNTIME OFF)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Wfatal-errors -D_MWAITXINTRIN_H_INCLUDED")

include_directories(${PROJECT_SOURCE_DIR})

# include and link dirs of cuda and tensorrt, you need adapt them if yours are different
# cuda
find_package(CUDA REQUIRED)
set(CUDA_NVCC_FLAGS -gencode arch=compute_86,code=sm_86)
include_directories(/usr/local/cuda/include)
link_directories(/usr/local/cuda/lib64)

# cudnn
include_directories(/usr/local/cuda/include)
link_directories(/usr/local/cuda/lib64)

# tensorrt
set(TRT_OSS_ROOT /usr/local/libraries/TensorRT-8.2.4.2)
include_directories(${TRT_OSS_ROOT}/include)
link_directories(${TRT_OSS_ROOT}/lib)

# opencv
find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS})

add_executable(yolact ${PROJECT_SOURCE_DIR}/yolact.cpp)
target_link_libraries(yolact nvinfer)
target_link_libraries(yolact cudart)
target_link_libraries(yolact ${OpenCV_LIBS} nvparsers cublas nvinfer_plugin cudnn)

# add_definitions(-O2 -pthread)

