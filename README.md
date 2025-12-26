## 用户+密码+角色
editor,123,EDITOR
reviewer,123,REVIEWER
alice,123,AUTHOR
admin,123,ADMIN
reviewer1,123,REVIEWER
bob,123,AUTHOR

## 测试指令
### 一、打开一个终端用于服务器
cd operatingsys
rm -rf build_server
cmake -S server -B build_server
cmake --build build_server -j
./build_server/server

### 二、打开多个终端用于client
在某一个终端执行如下指令：
cd operatingsys
rm -rf build_client
mkdir -p build_client
cd build_client
cmake ../client
cmake --build . -j
./client_cli

在其他所有终端执行如下指令：
cd build_client
./client_cli

#### 接下来在不同的client终端下执行2.1-2.5
##### 2.1打开终端一：登陆AUTHOR并上传文章
connect 127.0.0.1 9090
login alice 123
upload paper_001
这是我的论文
END

status paper_001
papers
revise paper_001
这是修订稿（SUBMITTED 状态应覆盖 v1.txt，不新建版本）
END

##### 2.2打开终端二：登陆EDITOR并分配审稿人
connect 127.0.0.1 9090
login editor 123
queue
assign paper_001 reviewer
assign paper_001 reviewer1
status paper_001


##### 2.3打开终端三：登陆reviewer和reviewer1分别给出审稿建议
connect 127.0.0.1 9090
login reviewer 123
tasks
download paper_001
reviews_give paper_001
这篇论文我给 8/10，建议小修。
END
logout

login reviewer1 123
tasks
download paper_001
reviews_give paper_001
这篇论文我给 7/10，建议改一下。
END
logout

##### 2.4回到终端二：继续使用EDITOR，查看意见并终审
status paper_001
reviews paper_001
decide paper_001 REJECT


##### 2.5回到终端一：继续使用AUTHOR，发现文章被拒，重新上传
status paper_001
revise paper_001
这是修订稿（SUBMITTED 状态应新建 v2.txt，新建版本）
END

##### 2.6重复2.2-2.5，直到论文被accept

##### 2.7管理员快照测试
login admin 123

mkdir /paper_system/snap_test
ls /paper_system
write /paper_system/snap_test/a.txt
v1-content
END
read /paper_system/snap_test/a.txt
backup_create snap_test_1
backup_list

write /paper_system/snap_test/a.txt
v2-content
END
read /paper_system/snap_test/a.txt
backup_restore snap_test_1
read /paper_system/snap_test/a.txt
