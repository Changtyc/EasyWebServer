#include <arpa/inet.h>
#include <cassert>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "heap_timer.h"
#include "http_conn.h"
#include "locker.h"
#include "log.h"
#include "sql_connection_pool.h"
#include "threadpool.h"

#define MAX_FD 65536           // 最大文件描述符
#define MAX_EVENT_NUMBER 10000 // 最大事件数
#define TIMESLOT 5             // 最小超时单位

// 这三个函数在http_conn.cpp中定义，改变链接属性
extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd, int fd);
extern int setnonblocking(int fd);

// 设置定时器相关参数
static int pipefd[2];
// static sort_timer_lst timer_lst; // 定时器双向链表
static HeapTimer timer_lst; // 定时器双向链表

static int epollfd = 0;

// 信号处理函数
void sig_handler(int sig) {
    // 为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

// 设置信号函数
void addsig(int sig, void(handler)(int), bool restart = true) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    // 信号处理函数中仅仅发送信号值，不做对应逻辑处理
    sa.sa_handler = handler;
    // 设置中断后恢复标志
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 定时器到达处理任务，重新定时以不断触发SIGALRM信号
void timer_handler() {
    timer_lst.tick();
    alarm(TIMESLOT);
}

// 定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data *user_data) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;

    // 输出日志
    LOG_INFO("close fd %d", user_data->sockfd);
    // printf("close fd %d \n", user_data->sockfd);
    Log::get_instance()->flush();
}

// 连接个数过多，返回错误信息，并断开连接
void show_error(int connfd, const char *info) {
    // printf("%s", info);
    LOG_ERROR("accept numbers are too big!, %s", info);
    Log::get_instance()->flush();
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[]) {
    // 异步日志
    Log::get_instance()->init("ServerLog", 8192, 800000, 500);

    // 设置的端口
    if (argc <= 1) {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }
    int port = atoi(argv[1]);

    // 忽略管道的差错信号，避免程序意外退出
    addsig(SIGPIPE, SIG_IGN);

    // 创建数据库连接池
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", "dbname", "dbPasswd", "mydatabase", 3306, 8);

    // 创建线程池
    threadpool<http_conn> *pool = NULL;
    try {
        pool = new threadpool<http_conn>(connPool);
    } catch (...) {
        return 1;
    }

    // 直接开辟所有文件描述符的连接
    http_conn *users = new http_conn[MAX_FD];
    assert(users);

    // 初始化数据库读取表
    users->initmysql_result(connPool);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);
    // 设置为非阻塞
    setnonblocking(listenfd);

    int flag = 1;
    // 强制使用被处于TIME_WAIT状态的连接占用的socket地址
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    int ret = 0;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    // 创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);

    /*
    监听listenfd上是不能注册EPOLLONESHOT事件的；
    但是对于socket的读、写事件，应注册为EPOLLONESHOT来保证一个socket连接
    在任一时刻都只被一个线程处理
    */
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    // 设置管道写端为非阻塞
    setnonblocking(pipefd[1]);

    // 设置管道读端为ET非阻塞，非一次性
    addfd(epollfd, pipefd[0], false);

    // 添加程序结束的信号
    addsig(SIGALRM, sig_handler);
    // 添加闹钟信号
    addsig(SIGTERM, sig_handler);

    // 循环条件，优雅退出
    bool stop_server = false;

    client_data *users_timer = new client_data[MAX_FD];

    bool timeout = false;

    // 开始循环定时
    alarm(TIMESLOT);

    while (!stop_server) {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR) {
            LOG_ERROR("%s", "epoll failure");
            Log::get_instance()->flush();
            break;
        }

        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;

            // 处理新到的客户连接
            if (sockfd == listenfd) {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                while (1) {
                    // 非阻塞，因为是ET模式，所以需要while循环
                    int connfd =
                        accept(listenfd, (struct sockaddr *)&client_address,
                               &client_addrlength);
                    if (connfd < 0) {
                        // 此时已经没有连接了，或者是连接出错了
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        Log::get_instance()->flush();
                        break;
                    }
                    if (http_conn::m_user_count >= MAX_FD) {
                        show_error(connfd, "Internal server is busy");
                        break;
                    }
                    users[connfd].init(connfd, client_address);

                    // 初始化client_data数据
                    // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    util_timer *timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    // 设置定时数据
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
                continue;
            }
            // 客户端关闭连接，移除对应的定时器
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);

                if (timer) {
                    timer_lst.del_timer(timer);
                }
            }

            // 处理信号，即管道的读端
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1) {
                    continue;
                } else if (ret == 0) {
                    continue;
                } else {
                    for (int i = 0; i < ret; ++i) {
                        switch (signals[i]) {
                        case SIGALRM: {
                            // 定时器到了
                            timeout = true;
                            break;
                        }
                        case SIGTERM: {
                            // 退出程序
                            stop_server = true;
                            LOG_INFO("%s", "program exit!");
                            Log::get_instance()->flush();
                        }
                        default:
                            break;
                        }
                    }
                }
            }

            // 处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN) {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].read_once()) {
                    LOG_INFO("deal with the client(%s)",
                             inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    // 若监测到读事件，将该http事件放入请求队列
                    pool->append(users + sockfd);

                    // 若有数据传输，则将定时器往后延迟3个单位
                    // 并对新的定时器在链表上的位置进行调整
                    if (timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                } else {
                    // 关闭连接并移除定时器
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer) {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            // 处理客户连接上的发送数据
            else if (events[i].events & EPOLLOUT) {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write()) {
                    LOG_INFO("send data to the client(%s)",
                             inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    // 若有数据传输，则将定时器往后延迟3个单位
                    // 并对新的定时器在链表上的位置进行调整
                    if (timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                } else {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer) {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        // 处理定时器为非必须事件，收到信号并不是立马处理
        // 完成读写事件后，再进行处理
        if (timeout) {
            timer_handler();
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
