# easycallcenter365 依赖的FreeSWITCH模块

基于FreeSWITCH和大模型的智能电话客服 https://gitee.com/easycallcenter365/easycallcenter365
运行依赖的相关模块。主要有语音识别 mod_funasr 、流式语音合成 mod_aliyun_tts 等。

### 技术交流 && 商业咨询

   bug反馈或者咨询问题请在gitee/github上，新建 Issue，并贴上日志。

  ![联系方式](wetchat.png) 


### 模块 mod_funasr 如何使用
   
start asr:
```xml
<action application="start_asr" data="hello"/>
```

pause asr:
```xml
<action application="pause_asr" data="1"/>
```
resume asr:
```xml
<action application="pause_asr" data="0"/>
```
stop asr: 
```xml
<action application="stop_asr" />	
```

### 模块 mod_aliyun_tts 如何使用
```xml
<action application="speak" data="<engine>|<voice>|<text>|[timer_name]"/>
```
Example:

first request:
```xml
<action application="speak" data="aliyun_tts|aixia|上午好，这里是未来科技公司"/>
```

resume session based on the last request:
```xml
<action application="aliyuntts_resume" data="请问有什么可以帮您?"/>
```

once speak completed, the session will be closed automatically.
you need to use speak to re-open the session again if you want to speak again.

### 运行环境

   该项目目前仅在 debian-12.5 环境下编译测试通过。其他操作系统环境尚未测试。
   
### 是否有预编译的FreeSWITCH?
  
  是的，我们提供了预编译的FreeSWITCH-1.10.11，内置了 mod_funasr 和 mod_aliyun_tts 。
  它的运行环境是debain12.5，你需要配合docker运行它。
  docker镜像文件及FreeSWITCH下载地址: https://pan.baidu.com/s/1xFgMPCu0VKHKnG69QhyTlA 提取码: etv5  
  部署文档参考共享文件目录下的文件 freeswitch-Deploy-for-easycallcenter365.txt 。   

### 编译FreeSWITCH模块

* 下载FreeSWITCH-1.10.11源代码

  本项目支持的的FreeSWITCH版本是1.10.11，下载地址是： https://files.freeswitch.org/freeswitch-releases/ 。
  首先需要编译FreeSWITCH，参考官网文档。编译完成之后 make install 默认安装位置是 /usr/local/freeswitch/

* 下载 mod_funasr 和 mod_aliyun_tts 源代码
   
  git clone https://gitee.com/easycallcenter365/free-switch-modules-libs.git  

* 代码合并

  把 mod_funasr 及 mod_aliyun_tts 合并到FreeSWITCH源代码中，也就是 src\mod\asr_tts 目录下。
  libs/libhv/拷贝到FreeSWITCH源代码的相同目录下。
  
* 编译 mod_funasr 和 mod_aliyun_tts   

  首先需要下载 rapidjson , 地址是 https://github.com/Tencent/rapidjson 。
  接下来先编译 libhv，参考 libs/libhv/BUILD.md，使用cmake方式编译即可。
  这里把libhv纳入git管理的原因是因为我们对它的代码做了修改，如果使用libhv官方的版本，无法和我们项目兼容。
  libhv 编译完成后，把libhv.so 拷贝到 FreeSWITCH 的 lib 目录下。
  
  最后编译 mod_funasr 和 mod_aliyun_tts ，参考各自源码中的头部注释。
  
  编译完成后，把 mod_funasr.so、mod_aliyun_tts.so 拷贝到 FreeSWITCH 的 mod 目录下。
    
### 如何配置 mod_funasr 和 mod_aliyun_tts

* 配置 mod_funasr
  目前本项目支持的 funasr 版本是 funasr-online-0.1.9，更高版本未测试。
  找到FreeSWITCH配置文件目录下 autoload_configs/funasr.conf.xml 文件，
  修改 server_url 参数。
  
* 配置 mod_aliyun_tts  
  请先到阿里云后台申请tts试用。
  找到FreeSWITCH配置文件目录下 autoload_configs/aliyun_tts.conf.xml 文件，
  修改access_key_id、access_key_secret、app_key。
  