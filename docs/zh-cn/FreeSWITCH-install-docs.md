# Compile and Install FreeSWITCH

## 从源代码编译安装FreeSWITCH

```bash
cd /home/
wget https://files.freeswitch.org/releases/freeswitch/freeswitch-1.10.11.-release.zip 
apt-get install zip unzip  git
unzip -d /home/freeswitch  freeswitch-1.10.11.-release.zip
```

如果在linux使用wget下载失败，请在windows下使用下载工具下载，然后上传到linux的 /home 目录下。

## download FreeSWITCH mods for easyCallcenter365

```bash
   mkdir /home/easyCallcenter365
   cd /home/easyCallcenter365
   git clone https://gitee.com/easycallcenter365/freeswitch-modules-libs.git
```

## 合并代码

```bash
 cp -r /home/easyCallcenter365/freeswitch-modules-libs/src/*  /home/freeswitch/freeswitch-1.10.11.-release/src/
 cp -r /home/easyCallcenter365/freeswitch-modules-libs/libs/*  /home/freeswitch/freeswitch-1.10.11.-release/libs/
```    
 
    	
## 设置Token

The compilation requires downloading resource files from FreeSWITCH's official site with authentication. You may request your own Token or use mine. Run this command:

```bash
TOKEN=pat_shvgo5fY5igbiQdaJ3VUHppC
```

## 安装基本工具

```bash
apt-get update && apt-get install -yq gnupg2 wget lsb-release
```
	
## 设置FreeSWITCH的apt源

```bash
    wget --http-user=signalwire --http-password=$TOKEN -O /usr/share/keyrings/signalwire-freeswitch-repo.gpg https://freeswitch.signalwire.com/repo/deb/debian-release/signalwire-freeswitch-repo.gpg
    echo "machine freeswitch.signalwire.com login signalwire password $TOKEN" > /etc/apt/auth.conf
    chmod 600 /etc/apt/auth.conf
    echo "deb [signed-by=/usr/share/keyrings/signalwire-freeswitch-repo.gpg] https://freeswitch.signalwire.com/repo/deb/debian-release/ `lsb_release -sc` main" > /etc/apt/sources.list.d/freeswitch.list
    echo "deb-src [signed-by=/usr/share/keyrings/signalwire-freeswitch-repo.gpg] https://freeswitch.signalwire.com/repo/deb/debian-release/ `lsb_release -sc` main" >> /etc/apt/sources.list.d/freeswitch.list
    apt-get update
    apt-get build-dep freeswitch
```
		
## 编译FreeSWITCH

```bash
    cd  /home/freeswitch/freeswitch-1.10.11.-release
	# 开启 mariadb 和 curl 模块，
	vim modules.conf  
	# databases/mod_mariadb 去掉#号
	# databases/curl 去掉#号
    ./rebootstrap.sh -j
    ./configure  --prefix=/usr/local/freeswitchvideo  
    make -j 3
    make install
```

## 编译libhv

```bash
apt-get -y install cmake
mkdir build && cd build
cmake .. -DWITH_OPENSSL=ON
cmake --build .
cp lib/libhv.so  /usr/local/freeswitchvideo/lib/
```

## 编译 rapidJson

```bash
cd /home/freeswitch/
git clone https://github.com/Tencent/rapidjson.git
mkdir build && cd build
cmake ..
make install
```

## 编译 mod_funasr 和 mod_aliyun_tts

在本项目中，mod_funasr 用作语音识别功能, 通过它来集成 FunASR。 mod_aliyun_tts 用作语音合成, 通过它来集成阿里云tts.

```bash
# 编译 mod_funasr
cd /home/freeswitch/freeswitch-1.10.11.-release/src/mod/asr_tts/mod_funasr
g++  -shared -o mod_funasr.so -fPIC -g -O -ggdb -std=c++11 -Wall   mod_funasr.cpp  -I../../../../libs/libhv/build/include/hv/   -I../../../../libs/libteletone/src/   -I../../../../src/include/   -lpthread  -L/usr/local/freeswitchvideo/lib/  -lhv -lfreeswitch
cp mod_funasr.so  /usr/local/freeswitchvideo/lib/freeswitch/mod/

# 编译 mod_aliyun_tts
cd ../mod_aliyun_tts/
g++ -shared -o mod_aliyun_tts.so -fPIC -g -O -ggdb -std=c++11 -Wall   -I../../../../libs/libhv/include/hv/   -I../../../../libs/cpputils/ -I../../../../src/include/   -I../../../../libs/libteletone/src/   mod_aliyun_tts.cpp    -L/usr/local/freeswitchvideo/lib/  -lfreeswitch   -L/usr/local/lib/  -lhv    -lcurl  -lpthread   -lssl -lcrypto
cp mod_aliyun_tts.so /usr/local/freeswitchvideo/lib/freeswitch/mod/
```


## build apr and apr-util

```bash
mkdir /home/mrcp/ && cd  /home/mrcp/
apt-get install wget tar
wget https://www.unimrcp.org/project/component-view/unimrcp-deps-1-6-0-tar-gz/download -O unimrcp-deps-1.6.0.tar.gz
tar xvzf unimrcp-deps-1.6.0.tar.gz
cd unimrcp-deps-1.6.0

cd libs/apr
./configure --prefix=/usr/local/apr
make
make install 

cd ..
cd apr-util
./configure --prefix=/usr/local/apr-util --with-apr=/usr/local/apr
make
make install

cd ..
cd sofia-sip/
./configure --prefix=/usr/local/sofia-sip
make 
make install
```

## 编译 unimrcp

```bash
cd /home/mrcp/
git clone https://github.com/unispeech/unimrcp.git
cd unimrcp
./bootstrap
./configure --with-apr=/usr/local/apr --with-apr-util=/usr/local/apr-util  --with-sofia-sip=/usr/local/sofia-sip
make
make install
```

## 编译 mod_unimrcp

```bash
cd /home/mrcp/
git clone https://github.com/freeswitch/mod_unimrcp.git
cd mod_unimrcp
export PKG_CONFIG_PATH=/usr/local/freeswitchvideo/lib/pkgconfig:/usr/local/unimrcp/lib/pkgconfig
./bootstrap.sh
./configure
make
make install
# copy lib files to freeswitch lib directory
cp /usr/local/unimrcp/lib/libunimrcpclient.so.0 /usr/local/apr/lib/libapr-1.so.0  /usr/local/apr-util/lib/libaprutil-1.so.0  /usr/local/freeswitchvideo/lib/
```

## 替换FreeSWITCH配置文件

  从 freeswitch-modules-libs\FreeSWITCH-Config-Files\conf 拷贝配置文件，覆盖掉FreeSWITCH默认的配置文件。
  
```bash
cp -r /home/easyCallcenter365/freeswitch-modules-libs/FreeSWITCH-Config-Files/conf/*  /usr/local/freeswitchvideo/etc/freeswitch/
rm -rf /usr/local/freeswitchvideo/etc/freeswitch/sip_profiles/*ipv6*
```

## 设置FreeSWITCH的数据库连接

`docker` 的安装参考文档: [Debian12-install-docker.md](Debian12-install-docker.md) 。
`mysql8` 的安装参考文档: [Debian12-install-mysql8.md](Debian12-install-mysql8.md) 。

首先需要安装`mysql8`，创建一个名为 `freeswitch` 的数据库，然后导入sql文件： freeswitch-modules-libs\sql\freeswitch-1.10.11.sql 。
这里假定设置`mysql`数据库设置密码为: easyCallcenter365Abc

a. 修改hosts文件
   vim /etc/hosts ，增加一条记录：127.0.0.1    easycallcenter365

b. 修改 switch.conf.xml
vim /usr/local/freeswitchvideo/etc/freeswitch/autoload_configs/switch.conf.xml ， 在 `settings` 节点下增加配置：
<param name="core-db-dsn" value="mariadb://Server=easycallcenter365;Port=3306;Database=freeswitch;Uid=root;Pwd=easyCallcenter365Abc;" />

c. 修改 internal.xml
vim /usr/local/freeswitchvideo/etc/freeswitch/sip_profiles/internal.xml ， 在 `settings` 节点下增加配置：
<param name="odbc-dsn" value="mariadb://Server=easycallcenter365;Port=3306;Database=freeswitch;Uid=root;Pwd=easyCallcenter365Abc;" />

d. 修改 external.xml
vim /usr/local/freeswitchvideo/etc/freeswitch/sip_profiles/external.xml ， 在 `settings` 节点下增加配置：
<param name="odbc-dsn" value="mariadb://Server=easycallcenter365;Port=3306;Database=freeswitch;Uid=root;Pwd=easyCallcenter365Abc;" />

## 启动FreeSWITCH

所有参数设置好之后，尝试启动 `FreeSWITCH` 。
```bash
export LD_LIBRARY_PATH=/usr/local/freeswitchvideo/lib/
/usr/local/freeswitchvideo/bin/freeswitch -nonat -nosql
# 待 FreeSWITCH 启动后检查状态
sofia status
# 如果 sofia status 显示的记录为0，则说明数据库可能没有连接成功，请检查数据库配置

# 检查mod_funasr及mod_aliyun_tts模块是否正常加载
load mod_funasr      # 如果提示：Module mod_funasr Already Loaded! 说明加载成功
load mod_aliyun_tts  # 如果提示：Module mod_aliyun_tts Already Loaded! 说明加载成功
```

## 语音识别和合成配置

参考 [Set-up-ASR-TTS.md](Set-up-ASR-TTS.md)。






