# 指定目标系统
set(CMAKE_SYSTEM_NAME Linux)

# 指定目标平台
set(CMAKE_SYSTEM_PROCESSOR mips)

# 指定交叉编译工具链的根路径
set(CROSS_CHAIN_PATH /opt/mips-gcc540-glibc222-64bit-r3.3.0)

# 设置编译环境根目录
set(CMAKE_SYSROOT "${CROSS_CHAIN_PATH}/mips-linux-gnu/libc")

# 指定C编译器
set(CMAKE_C_COMPILER "${CROSS_CHAIN_PATH}/bin/mips-linux-gnu-gcc")
# 指定C++编译器
set(CMAKE_CXX_COMPILER "${CROSS_CHAIN_PATH}/bin/mips-linux-gnu-g++")

# 指定编译器选项
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -std=gnu11 -march=mips32r2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -std=gnu++11 -march=mips32r2")

# 指定链接器选项
set(CMAKE_LD_FLAGS "${CMAKE_LD_FLAGS} stdc++ -Wl,-gc-sections")
