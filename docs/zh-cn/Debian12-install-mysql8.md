## Debian12安装mysql8

如果您已经安装了 `mysql8` ，本步骤可以跳过。本文档提供了通过 `docker` 安装 `mysql8`

```bash
mkdir /home/mysql/
mkdir /home/mysql/data/ /home/mysql/logs/  
cd /home/mysql/

# 设置docker仓库源
echo  "{  \
     \"registry-mirrors\": [\"https://docker.registry.cyou\",  \
    \"https://docker-cf.registry.cyou\",  \
    \"https://dockercf.jsdelivr.fyi\",  \
    \"https://docker.jsdelivr.fyi\",  \
    \"https://dockertest.jsdelivr.fyi\",  \
    \"https://mirror.aliyuncs.com\",  \
    \"https://dockerproxy.com\",  \
    \"https://mirror.baidubce.com\",  \
    \"https://docker.m.daocloud.io\",  \
    \"https://docker.nju.edu.cn\",  \
    \"https://docker.mirrors.sjtug.sjtu.edu.cn\",  \
    \"https://docker.mirrors.ustc.edu.cn\",  \
    \"https://mirror.iscas.ac.cn\",  \
    \"https://docker.rainbond.cc\"]  \
    }"   >  /etc/docker/daemon.json 

systemctl daemon-reload && systemctl restart docker
docker pull mysql:8.0.29
	
# 上传my.cnf配置文件
# 把 freeswitch-modules-libs/sql/mysql8/my.cnf 上传到 `/home/mysql/` 目录下。

chmod 644 my.cnf 

docker run -p 3306:3306 --name mysql -v $PWD/data:/var/lib/mysql -v $PWD/my.cnf:/etc/my.cnf -e MYSQL_ROOT_PASSWORD=easyCallcenter365Abc -d mysql:8.0.29

#进入容器
docker exec -it mysql /bin/bash

# 进入mysql控制台
mysql -uroot -peasyCallcenter365Abc -h 127.0.0.1

use mysql; update user set host='%' where user='root'; # 如遇错误不用管它
flush privileges;
GRANT ALL PRIVILEGES ON *.* TO 'root'@'%'WITH GRANT OPTION;
flush privileges;

ALTER USER 'root'@'%' IDENTIFIED BY 'easyCallcenter365Abc' PASSWORD EXPIRE NEVER;
ALTER USER 'root'@'%' IDENTIFIED WITH mysql_native_password BY 'easyCallcenter365Abc';
FLUSH PRIVILEGES;

ALTER USER 'root'@'%' IDENTIFIED WITH mysql_native_password BY 'easyCallcenter365Abc';
FLUSH PRIVILEGES;

```