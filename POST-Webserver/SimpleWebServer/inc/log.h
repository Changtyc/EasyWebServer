#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"

using namespace std;

// 实现异步日志

class Log {
  public:
    // C++11以后,使用局部静态变量实现单例模式不用加锁
    static Log *get_instance() {
        static Log instance;
        return &instance;
    }

    // 异步写日志公有方法，调用私有方法async_write_log
    static void *flush_log_thread(void *args) {
        Log::get_instance()->async_write_log();
    }
    // 可选择的参数有日志文件名称、日志的缓冲区大小、单日志文件的最大行数以及阻塞队列的大小
    bool init(const char *file_name, int log_buf_size = 8192,
              int split_lines = 5000000, int max_queue_size = 0);

    // 将输出内容按照标准格式整理
    void write_log(int level, const char *format, ...);

    // 强制刷新缓冲区
    void flush(void);

  private:
    Log();
    virtual ~Log();

    // 异步线程的写函数
    void *async_write_log() {
        string single_log;
        // 从阻塞队列中取出一个日志string，写入文件，以空字符串终止
        while (m_log_queue->pop(single_log)) {
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
    }

  private:
    char dir_name[128]; // 路径名
    char log_name[128]; // log文件名
    int m_split_lines;  // 日志最大行数，决定文件名
    long long m_count;  // 日志行数记录
    int m_today; // 因为按天分类,记录当前时间是那一天，决定文件名
    FILE *m_fp;                       // 打开log的文件指针
    char *m_buf;                      // 要输出的内容
    int m_log_buf_size;               // 日志缓冲区大小
    block_queue<string> *m_log_queue; // 阻塞队列
    bool m_is_async;                  // 是否异步日志
    locker m_mutex;                   // 异步互斥锁
};

// 这四个宏定义在其他文件中使用，主要用于不同类型的日志输出
// 使用了##__VA_ARGS__ 可变参数宏
#define LOG_DEBUG(format, ...)                                                 \
    Log::get_instance()->write_log(0, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...)                                                  \
    Log::get_instance()->write_log(1, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...)                                                  \
    Log::get_instance()->write_log(2, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...)                                                 \
    Log::get_instance()->write_log(3, format, ##__VA_ARGS__)

#endif
