#ifndef HEAP_TIMER_H
#define HEAP_TIMER_H

#include <time.h>
#include <queue>
#include <vector>
#include "log.h"

class util_timer;

// 用户数据结构
struct client_data {
    sockaddr_in address; // 客户端socket地址
    int sockfd;          // socket文件描述符
    util_timer *timer;   // 定时器
};

// 通用定时器类
class util_timer {
  public:
    util_timer() {}

  public:
    time_t expire; /*任务的超时时间，这里使用绝对时问*/
    void (*cb_func)(client_data *); // 任务回调函数
    client_data *user_data;
};

struct timer_cmp {
    bool operator()(const util_timer *f1, const util_timer *f2) {
        // 小顶堆
        return f1->expire > f2->expire;
    }
};

class HeapTimer {
  public:
    HeapTimer(){};
    ~HeapTimer() {
        // 释放所有定时器的堆内存
        while (!m_pq.empty()) {
            util_timer *curr = m_pq.top();
            m_pq.pop();
            delete curr;
        }
    };

    void add_timer(util_timer *timer) {
        // 直接插入即可
        m_pq.push(timer);
    }

    // 考虑定时器时间延长
    void adjust_timer(util_timer *timer) {
        del_timer(timer);
        add_timer(timer);
    };

    // 删除定时器
    void del_timer(util_timer *timer) {
        std::vector<util_timer *> vec;
        vec.reserve(m_pq.size() - 1);
        while (!m_pq.empty()) {
            // printf("%ld\n", m_pq.top()->expire);
            if (m_pq.top() != timer) {
                vec.push_back(m_pq.top());
            }
            m_pq.pop();
        }
        for (auto &t : vec) {
            m_pq.push(t);
        }
    }

    // 根据alarm，每隔一段时间tick一下以处理事件
    void tick() {
        // 输出日志
        LOG_INFO("%s", "timer tick");
        Log::get_instance()->flush();

        time_t cur = time(NULL);
        while (!m_pq.empty() && m_pq.top()->expire <= cur) {
            util_timer *tmp = m_pq.top();
            m_pq.pop();
            // 执行回调函数,并删除定时器
            tmp->cb_func(tmp->user_data);
            delete tmp;
        }
    }

  private:
    std::priority_queue<util_timer *, std::vector<util_timer *>, timer_cmp>
        m_pq;
};

#endif // HEAP_TIMER_H