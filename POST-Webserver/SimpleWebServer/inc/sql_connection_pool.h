#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "locker.h"

using namespace std;

class connection_pool {
  public:
    MYSQL *GetConnection();              // 获取数据库连接
    bool ReleaseConnection(MYSQL *conn); // 释放连接
    int GetFreeConn();                   // 获取连接
    void DestroyPool();                  // 销毁所有连接

    // 单例模式
    static connection_pool *GetInstance();

    // 删除复制构造函数和赋值操作符
    connection_pool(const connection_pool &) = delete;
    connection_pool &operator=(const connection_pool &) = delete;

    // mysql连接池初始化
    void init(string url, string User, string PassWord, string DataBaseName,
              int Port, unsigned int MaxConn);

  private:
    connection_pool();
    ~connection_pool();

  private:
    unsigned int MaxConn;  // 最大连接数
    unsigned int CurConn;  // 当前已使用的连接数
    unsigned int FreeConn; // 当前空闲的连接数

  private:
    locker lock;            // 互斥锁
    list<MYSQL *> connList; // MYSQL连接池
    sem reserve; // 可用连接的信号量，其实是和FreeConn重复了

  private:
    string url;          // 主机地址
    string Port;         // 数据库端口号
    string User;         // 登陆数据库用户名
    string PassWord;     // 登陆数据库密码
    string DatabaseName; // 使用数据库名
};

// 不直接调用获取和释放连接的接口，将其封装起来，通过RAII机制进行获取和释放。
class connectionRAII {
  public:
    connectionRAII(MYSQL **con, connection_pool *connPool);
    ~connectionRAII();

  private:
    MYSQL *conRAII;
    connection_pool *poolRAII;
};

#endif
