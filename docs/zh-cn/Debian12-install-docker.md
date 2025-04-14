## Debian12安装Docker

如果您已经安装了Docker，本步骤可以跳过。

```bash
apt-get update
apt-get -y install apt-transport-https software-properties-common ca-certificates curl gnupg lsb-release
curl -fsSL https://mirrors.aliyun.com/docker-ce/linux/debian/gpg | apt-key add -
add-apt-repository "deb [arch=amd64] https://mirrors.tuna.tsinghua.edu.cn/docker-ce/linux/debian $(lsb_release -cs) stable"
apt-get update
apt-get -y install docker-ce 
systemctl enable docker
```