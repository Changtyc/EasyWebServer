#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include "locker.h"
#include "sql_connection_pool.h"

// 线程池的模板参数类，用以封装对http连接的处理
class http_conn {
  public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    // 这里实现了GET 和 POST
    enum METHOD {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    enum HTTP_CODE {
        NO_REQUEST, // 请求不完整，需要继续读取请求报文数据,跳转主线程继续监测读事件
        GET_REQUEST, // 获得了完整的HTTP请求,调用do_request完成请求资源映射
        BAD_REQUEST, // HTTP请求报文有语法错误或请求资源为目录,跳转process_write完成响应报文
        NO_RESOURCE, // 请求资源不存在,跳转process_write完成响应报文
        FORBIDDEN_REQUEST, // 请求资源禁止访问，没有读取权限,跳转process_write完成响应报文
        FILE_REQUEST, // 请求资源可以正常访问,跳转process_write完成响应报文
        INTERNAL_ERROR,   // 服务器内部错误
        CLOSED_CONNECTION // 客户端已经关闭连接
    };
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

  public:
    http_conn() {}
    ~http_conn() {}

  public:
    void init(int sockfd, const sockaddr_in &addr);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();
    bool write();
    sockaddr_in *get_address() { return &m_address; }
    // 初始化数据库连接池的所有表项
    void initmysql_result(connection_pool *connPool);

  private:
    void init();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request();
    char *get_line() { return m_read_buf + m_start_line; };
    LINE_STATUS parse_line();
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

  public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;

  private:
    int m_sockfd;
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE];
    int m_read_idx;
    int m_checked_idx;
    int m_start_line;
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;
    CHECK_STATE m_check_state;
    METHOD m_method;
    char m_real_file[FILENAME_LEN]; // 实际的文件地址
    char *m_url;
    char *m_version;
    char *m_host;
    int m_content_length;
    bool m_linger;
    char *m_file_address;
    struct stat m_file_stat;
    struct iovec m_iv[2];
    int m_iv_count;

    // 以下是POST是需求变量
    int cgi;        // 是否启用的POST
    char *m_string; // 存储请求体数据,账号和密码
    int bytes_to_send;
    int bytes_have_send;
};

#endif
