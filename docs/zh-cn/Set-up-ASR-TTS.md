# 如何配置语音识别和语音合成

## 配置语音识别

语音识别支持 websocket 和 mrcp 两种对接方式。
websocket对接方式下，开源版本仅提供了 funASR 的对接模块。
mrcp对接方式下，可以对接阿里云MRCPServer，具体请参考阿里云官方文档 《SDM(MRCP-SERVER)公共云镜像使用》。

配置 `funASR` 步骤如下：

* 配置mod_funasr

进入 `FreeSWITCH` 配置目录 /usr/local/freeswitchvideo/etc/freeswitch/autoload_configs ， 编辑文件:
vim funasr.conf.xml ，我们看到 `server_url` 参数为 `ws://127.0.0.1:2710` ， 这里保持默认即可。

* 配置funasr-server

我们提供了 `funasr-server` 的一键安装包，funASR-0.1.9 链接: https://pan.baidu.com/s/1Cg1xUcxrsLMaUv8CklFLug 提取码: 4tke 。
通过funASR官方文档进行安装，有比较大的安装失败概率。 下载安装包到本地，然后参考文档 "FunAsr-0.1.9-集群离线部署.txt" 进行安装。

## 配置语音合成

语音合成目前支持阿里云tts流式语音合成。登录到阿里云后台，搜索"智能语音交互"，开通tts试用申请。
 找到FreeSWITCH配置文件目录下 /usr/local/freeswitchvideo/etc/freeswitch/aliyun_tts.conf.xml 文件， 
 修改access_key_id、access_key_secret、app_key。 保存后进入FreeSWITCH控制台:  realod mod_aliyun_tts 即生效。
