#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool() {
    this->CurConn = 0;
    this->FreeConn = 0;
}

connection_pool *connection_pool::GetInstance() {
    // 局部静态变量单例模式,懒汉模式
    static connection_pool connPool;
    return &connPool;
}

// 构造初始化，应只手动调用一次
void connection_pool::init(string url, string User, string PassWord,
                           string DBName, int Port, unsigned int MaxConn) {
    this->url = url;
    this->Port = Port;
    this->User = User;
    this->PassWord = PassWord;
    this->DatabaseName = DBName;

    lock.lock();
    FreeConn = 0;
    // 创建MaxConn条数据库连接
    for (int i = 0; i < MaxConn; i++) {
        MYSQL *con = NULL;
        con = mysql_init(con); // 获取或初始化MYSQL结构。

        if (con == NULL) {
            cout << "Error:" << mysql_error(con);
            exit(1);
        }
        // 数据库引擎建立连接
        con =
            mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(),
                               DBName.c_str(), Port, NULL, 0);

        if (con == NULL) {
            cout << "Error: " << mysql_error(con);
            exit(1);
        }
        connList.push_back(con);
        ++FreeConn;
    }

    reserve = sem(FreeConn);
    this->MaxConn = FreeConn;

    lock.unlock();
}

// 当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection() {
    if (0 == connList.size())
        return NULL;
    MYSQL *con = NULL;

    // 取出连接，信号量原子减1，为0则等待
    reserve.wait();

    lock.lock();

    con = connList.front();
    connList.pop_front();
    --FreeConn;
    ++CurConn;

    lock.unlock();
    return con;
}

// 释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con) {
    if (NULL == con)
        return false;

    lock.lock();

    connList.push_back(con);
    ++FreeConn;
    --CurConn;

    lock.unlock();

    reserve.post();
    return true;
}

// 销毁数据库连接池
void connection_pool::DestroyPool() {

    lock.lock();
    if (connList.size() > 0) {
        list<MYSQL *>::iterator it;
        for (it = connList.begin(); it != connList.end(); ++it) {
            MYSQL *con = *it;
            // 关闭数据库连接
            mysql_close(con);
        }
        CurConn = 0;
        FreeConn = 0;
        connList.clear();

        lock.unlock();
    }

    lock.unlock();
}

// 当前空闲的连接数
int connection_pool::GetFreeConn() { return this->FreeConn; }

connection_pool::~connection_pool() { DestroyPool(); }

// 以下是销毁的RAII机制
connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool) {
    *SQL = connPool->GetConnection();
    // 保存变量
    conRAII = *SQL;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII() {
    // 析构调用数据库连接回收函数
    poolRAII->ReleaseConnection(conRAII);
}