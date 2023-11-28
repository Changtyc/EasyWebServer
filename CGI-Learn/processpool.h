#ifndef PROCESSPOOL_H
#define PROCESSPOOL_H

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/*描述一个子进程的类，m_pid是目标子进程的PID，m_pipefd是父进程和子进程通信用的管道*/
class process {
  public:
    process() : m_pid(-1) {}

  public:
    pid_t m_pid;
    int m_pipefd[2];
};

/*进程池类，将它定义为摸板类是为了代码复用。其模板参数是处理逻辑任务的类;
  模板参数必须实现两个函数
        process()
        init();
*/

template <typename T> class processpool {
  private:
    // 构造函数私有化
    processpool(int listenfd, int process_number = 8);

  public:
    // 单例模式，保证程序最多创建一个进程池
    static processpool<T> *create(int listenfd, int process_number = 8) {
        if (!m_instance) {
            m_instance = new processpool<T>(listenfd, process_number);
        }
        return m_instance;
    }
    ~processpool() { delete[] m_sub_process; }

    // 启动进程池
    void run();

  private:
    void setup_sig_pipe();
    void run_parent();
    void run_child();

  private:
    static const int MAX_PROCESS_NUMBER = 16; // 进程池允许的最大子进程数量
    static const int USER_PER_PROCESS =
        65536; // 每个子进程最多能处理的客户数量，文件描述符总数
    static const int MAX_EVENT_NUMBER = 10000; // epoll最多能处理的事件数
    int m_process_number;                      // 进程池中进程总数
    int m_idx;     // 子进程在池中的序号，从0开始，-1为父进程
    int m_epollfd; // 每个进程都有一个epoll内核事件表用m_epollfd标识，每个进程都是多路复用
    int m_listenfd;         // 监听socket
    int m_stop;             // 子进程通过m_stop来决定是否停止运行
    process *m_sub_process; // 保存所有子进程描述信息
    static processpool<T> *m_instance; // 进程池静态实例
};
template <typename T> processpool<T> *processpool<T>::m_instance = NULL;

static int sig_pipefd[2];

static int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

static void addfd(int epollfd, int fd) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

/*从epollfd标识的epol1内被事件表中剩除fd上的所有注册事件*/
static void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 信号处理函数，传递到管道
static void sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;
    send(sig_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

static void addsig(int sig, void(handler)(int), bool restart = true) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart) {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 进程池构造函数，父进程m_idx为-1
// listenfd 是监听sokcet，必须在创建进程前被创建
// process_number指定进程池中子进程数量
template <typename T>
processpool<T>::processpool(int listenfd, int process_number)
    : m_listenfd(listenfd), m_process_number(process_number), m_idx(-1),
      m_stop(false) {
    assert((process_number > 0) && (process_number <= MAX_PROCESS_NUMBER));

    m_sub_process = new process[process_number];
    assert(m_sub_process);

    // 使用管道实现了父进程与多个子进程之间的双向通信
    for (int i = 0; i < process_number; ++i) {
        int ret =
            socketpair(PF_UNIX, SOCK_STREAM, 0, m_sub_process[i].m_pipefd);
        assert(ret == 0);

        m_sub_process[i].m_pid = fork();
        assert(m_sub_process[i].m_pid >= 0);
        // 父子进程管道都只留一个口，可读可写
        if (m_sub_process[i].m_pid > 0) {
            // 父进程默认从m_pipefd[0]读写
            close(m_sub_process[i].m_pipefd[1]);
            continue;
        } else {
            // 子进程默认从m_pipefd[1]读写
            close(m_sub_process[i].m_pipefd[0]);
            // 每个子进程拥有自己的m_idx
            m_idx = i;
            break;
        }
    }
}

// 统一事件源
template <typename T> void processpool<T>::setup_sig_pipe() {

    // 创建epoll事件监听表和信号管道，5只是提示无实际意义。
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sig_pipefd);
    assert(ret != -1);

    setnonblocking(sig_pipefd[1]);
    // 信号处理函数->epoll
    addfd(m_epollfd, sig_pipefd[0]);

    // 设置信号处理函数
    addsig(SIGCHLD, sig_handler);
    addsig(SIGTERM, sig_handler);
    addsig(SIGINT, sig_handler);
    addsig(SIGPIPE, SIG_IGN);
}

/*父进程中m_idx值为-1，子进程中m_idx值大于等于0，我们据此判斯接下来要运行的是父进程代码还是子进程代码*/
template <typename T> void processpool<T>::run() {
    if (m_idx != -1) {
        run_child();
        return;
    }
    run_parent();
}

template <typename T> void processpool<T>::run_child() {
    // 创建epoll，统一事件源
    setup_sig_pipe();

    // 每个子进程都通过其在进程池中的序号值m_idx找到与父进程通信的管道
    int pipefd = m_sub_process[m_idx].m_pipefd[1];
    // 子进程需要监听管道文件描述符 pipefd,
    // 因为父进程将通过它来通知子进程accept新连接
    addfd(m_epollfd, pipefd);

    epoll_event events[MAX_EVENT_NUMBER];
    T *users = new T[USER_PER_PROCESS];
    assert(users);
    int number = 0;
    int ret = -1;

    while (!m_stop) {
        number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR)) {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;
            // 存在新的连接
            if ((sockfd == pipefd) && (events[i].events & EPOLLIN)) {
                int client = 0;
                // 从父子进程之间的管道读取数据，并保存在变量client中。
                // 如果成功，表示有新客户连接到来
                ret = recv(sockfd, (char *)&client, sizeof(client), 0);
                if (((ret < 0) && (errno != EAGAIN)) || ret == 0) {
                    continue;
                } else {
                    struct sockaddr_in client_address;
                    socklen_t client_addrlength = sizeof(client_address);
                    int connfd =
                        accept(m_listenfd, (struct sockaddr *)&client_address,
                               &client_addrlength);
                    if (connfd < 0) {
                        printf("errno is: %d\n", errno);
                        continue;
                    }
                    addfd(m_epollfd, connfd);
                    // 模板类T必须实现init方法，这里是连接类
                    users[connfd].init(m_epollfd, connfd, client_address);
                }
            }
            // 处理子进程接收到的信号
            else if ((sockfd == sig_pipefd[0]) &&
                     (events[i].events & EPOLLIN)) {
                int sig;
                char signals[1024];
                ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
                if (ret <= 0) {
                    continue;
                } else {
                    for (int i = 0; i < ret; ++i) {
                        switch (signals[i]) {
                        case SIGCHLD: {
                            pid_t pid;
                            int stat;
                            while ((pid = waitpid(-1, &stat, WNOHANG)) > 0) {
                                continue;
                            }
                            break;
                        }
                        case SIGTERM:
                        case SIGINT: {
                            m_stop = true;
                            break;
                        }
                        default: {
                            break;
                        }
                        }
                    }
                }
            }
            // 若是其他可读数据，必然是客户请求到来，调用处理逻辑处理对象的process方法处理即可
            else if (events[i].events & EPOLLIN) {
                users[sockfd].process();
            } else {
                continue;
            }
        }
    }

    delete[] users;
    users = NULL;
    close(pipefd);
    // close( m_listenfd ); // 哪个创建的listenfd，应该由哪个函数销毁
    // ，后面cgi服务器程序中销毁，而不是在子进程这里
    close(m_epollfd);
}

template <typename T> void processpool<T>::run_parent() {
    // 设置信号管道，统一信号源
    setup_sig_pipe();

    // 父进程监听m_listenfd
    addfd(m_epollfd, m_listenfd);

    epoll_event events[MAX_EVENT_NUMBER];
    int sub_process_counter = 0;
    int new_conn = 1;
    int number = 0;
    int ret = -1;

    while (!m_stop) {
        number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR)) {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;
            if (sockfd == m_listenfd) {
                // 若有新连接到来，就用Round Robin方式将其分配给一个子进程处理
                int j = sub_process_counter;
                do {
                    if (m_sub_process[j].m_pid != -1) {
                        break;
                    }
                    j = (j + 1) % m_process_number;
                } while (j != sub_process_counter);

                if (m_sub_process[j].m_pid == -1) {
                    m_stop = true;
                    break;
                }
                // 递增轮询
                sub_process_counter = (j + 1) % m_process_number;
                send(m_sub_process[j].m_pipefd[0], (char *)&new_conn,
                     sizeof(new_conn), 0);
                printf("send request to child %d\n", j);
            }
            // 处理父进程接收到的信号
            else if ((sockfd == sig_pipefd[0]) &&
                     (events[i].events & EPOLLIN)) {
                int sig;
                char signals[1024];
                ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
                if (ret <= 0) {
                    continue;
                } else {
                    for (int i = 0; i < ret; ++i) {
                        switch (signals[i]) {
                        case SIGCHLD: {
                            pid_t pid;
                            int stat;
                            // 如果进程池中第i个子进程退出了，则主进程关闭相应的通信管道
                            // 并设置相应的m_pid为-1，标记该子进程退出
                            while ((pid = waitpid(-1, &stat, WNOHANG)) > 0) {
                                for (int i = 0; i < m_process_number; ++i) {
                                    if (m_sub_process[i].m_pid == pid) {
                                        printf("child %d join, pid is %d\n", i,
                                               pid);
                                        close(m_sub_process[i].m_pipefd[0]);
                                        m_sub_process[i].m_pid = -1;
                                    }
                                }
                            }
                            // 若所有子进程都退出了，则父进程也退出
                            m_stop = true;
                            for (int i = 0; i < m_process_number; ++i) {
                                if (m_sub_process[i].m_pid != -1) {
                                    m_stop = false;
                                }
                            }
                            break;
                        }
                        case SIGTERM:
                        case SIGINT: {
                            // 若父进程收到终止信号，那么就杀死所有子进程
                            // 并等待它们全部结束。当然，通知子进程结束更好的方法是
                            // 向父子进程间的通信管道发送特殊数据。
                            printf("kill all the clild now\n");
                            for (int i = 0; i < m_process_number; ++i) {
                                int pid = m_sub_process[i].m_pid;
                                if (pid != -1) {
                                    kill(pid, SIGTERM);
                                }
                            }
                            break;
                        }
                        default: {
                            break;
                        }
                        }
                    }
                }
            } else {
                continue;
            }
        }
    }

    // close( m_listenfd ); //由创建者关闭这个文件描述符
    close(m_epollfd);
}

#endif
