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

### 从源码编译FreeSWITCH模块

参考 [docs/zh-cn/FreeSWITCH-install-docs.md](docs/zh-cn/FreeSWITCH-install-docs.md) 。

### 是否有预编译的FreeSWITCH?
  
  是的，我们提供了预编译的FreeSWITCH-1.10.11，内置了 mod_funasr 和 mod_aliyun_tts 。
  它的运行环境是debain12.5，你需要配合docker运行它。
  docker镜像文件及FreeSWITCH下载地址: https://pan.baidu.com/s/1xFgMPCu0VKHKnG69QhyTlA 提取码: etv5  
  部署文档参考共享文件目录下的文件 freeswitch-Deploy-for-easycallcenter365.txt 。  
  
  


  