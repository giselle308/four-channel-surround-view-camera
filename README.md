# 四路环视相机

从原工程独立整理出的海康 USB 工业相机驱动包，只保留相机取流、BGR 转换、配置及运行依赖。

## 内容

- `src/camera/`：相机初始化、取流和独立预览程序。
- `src/common/`：相机代码直接引用的日志与内存对齐头文件。
- `config/config.yaml`：分辨率、像素格式、帧率、曝光、增益、触发与白平衡配置。
- `config/camera_info.yaml`：当前相机的内参与畸变标定参数，供后续环视算法使用；独立预览程序暂不读取它。
- `include/`、`lib/`：海康 MVS SDK 头文件和 x86_64 Linux 运行库。
- `CMakeLists.txt`、`run.sh`：独立构建与启动入口。

## 系统依赖

需要 C++20 编译器、CMake、OpenCV 4、yaml-cpp 和 spdlog。Ubuntu 可安装发行版开发包：

```bash
sudo apt install build-essential cmake libopencv-dev libyaml-cpp-dev libspdlog-dev
```

## 构建与运行

```bash
cd 四路环视相机
chmod +x run.sh
./run.sh
```

也可以手动构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target camera_app -j
./build/camera_app
```

按 `Esc` 退出预览。相机参数修改 `config/config.yaml` 后重新启动即可，无需重新编译。

## 当前驱动范围

“四路环视相机”是本次拆包后的目录名称。复制出的原驱动当前仍是单设备实例：枚举 USB 相机后打开第一个设备；配置里的 `serial_number` 尚未用于选设备。若要四台相机同时采集，需要在此包上增加按序列号选择和四实例调度逻辑。

