# LicheeRV-Nano-Build

# download source

```
git clone https://github.com/sipeed/LicheeRV-Nano-Build --depth=1
cd LicheeRV-Nano-Build
git clone https://github.com/sophgo/host-tools --depth=1
```

## host environment

you can use container:

```
cd host/ubuntu
docker build -t licheervnano-build-ubuntu .
docker run --name licheervnano-build-ubuntu licheervnano-build-ubuntu
docker export licheervnano-build-ubuntu | sqfstar licheervnano-build-ubuntu.sqfs
singularity shell -e licheervnano-build-ubuntu.sqfs
```

# build it

```
source build/cvisetup.sh
# C906:
defconfig sg2002_licheervnano_sd
# A53:
# defconfig sg2002_licheea53nano_sd
build_all
```

# build fail

on some system, qt5svg or qt5base will build failed on first build, please retry command:

```
build_all
```

# how to modify image after build:

```
# first partition
touch wifi.sta
mcopy -i install/xxx/xxx.img@@1s wifi.sta ::/

# second partition
./host/mount_ext4.sh install/xxx/xxx.img mountpoint
cd mountpoint
touch xxx
```

# logo

```
./host/make_logo.sh input.jpeg logo.jpeg
mcopy -i install/xxx/xxx.img@@1s logo.jpeg ::/
```

如果只是把 `logo` 写进已经生成好的某个 SD 镜像，继续用上面这两条命令即可。

如果希望以后每次重新编译/打包生成的整个 `.img` 都默认带这张 `logo`，把生成好的文件覆盖到：

```
cp logo.jpeg build/tools/common/bootlogo/logo.jpg
```

之后重新执行打包流程。现在 `sd` 镜像生成脚本会优先使用 `build/tools/common/bootlogo/logo.jpg` 作为 `.img` 第一分区里的 `logo.jpeg`。

对于大分辨率图案，需要确保分辨率在屏幕分辨率范围内。默认短边在前。例如横排图片，分辨率为800x480，则需要在make_logo.sh中使用:
```
./host/make_logo.sh input.jpeg logo.jpeg 480 800 1
```
其中1表示顺时针旋转后再缩放，类似的，2是逆时针。

Linux 启动后如果启用了 framebuffer，系统会继续复用同一张 `/boot/logo.jpeg` 作为 `/dev/fb0` 的首帧，因此后续只需要维护这一份图片。

在当前平台上，`cvifb` 实测显存字节序为 `BGRA`：

```
byte0 = B
byte1 = G
byte2 = R
byte3 = A
```

因此如果用户态程序直接写 `/dev/fb0`，应按 `BGRA` 写入，并把 alpha 固定为 `0xff`。如果写全 0，会因为 alpha 为 0 而显示底色，不会得到纯黑屏。
