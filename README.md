# YOLACT-TensorRT in C++

our repo base on pytorch version of yolact [https://github.com/dbolya/yolact.git](https://github.com/dbolya/yolact.git)


## Step 1: Export onnx form pytorch yolact

Follow the trt [python demo README](../python/README.md) to convert and save the serialized engine file.

Check the 'model_trt.engine' file generated from Step 1, which will automatically saved at the current demo dir.


## Step 2: Generate yolact engine for tensorrt

Please follow the [TensorRT Installation Guide](https://docs.nvidia.com/deeplearning/tensorrt/install-guide/index.html) to install TensorRT.

Install opencv with ```sudo apt-get install libopencv-dev```.


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
./yolact ../model_trt.engine -i ../../../../assets/dog.jpg
```

or

```shell
./yolact <path/to/your/engine_file> -i <path/to/image>
```
