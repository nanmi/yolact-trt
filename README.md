# YOLACT-TensorRT in C++

our repo base on pytorch version of yolact [https://github.com/dbolya/yolact.git](https://github.com/dbolya/yolact.git)

## Step 0: Install some dependency package

Install opencv with ```sudo apt-get install libopencv-dev```.


## Step 1: Export onnx form pytorch yolact

reference: https://medium.com/@nanmi/%E5%A6%82%E4%BD%95%E7%94%A8tensorrt%E9%83%A8%E7%BD%B2yolact-3690e0708a85


## Step 2: Generate yolact engine for tensorrt

Please follow the [TensorRT trtexec tools](https://docs.nvidia.com/deeplearning/tensorrt/developer-guide/index.html#trtexec-ovr) to genetate TensorRT engine.


## Step 3: Build our project and inference yolact engine

build the demo:

```shell
mkdir build
cd build
cmake ..
make
```

Then run the demo:

```shell
./yolact ../yolac_fp32.engine -i ../000000000016.jpg
```

or

```shell
./yolact <path/to/your/engine_file> -i <path/to/image>
```

## About License
For the 3rd-party module and Deepstream, you need to follow their license

For the part I wrote, you can do anything you want
