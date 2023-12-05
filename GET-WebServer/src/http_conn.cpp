#include "http_conn.h"

// 定义HTTP响应的状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form =
    "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form =
    "You do not have permission to get file from this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form =
    "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form =
    "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char *doc_root = "/home/tyc/httpTest";

int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    // 始终维持一个socket连接在任一时刻都只被一个线程处理
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

// 关闭连接删减客户数量
void http_conn::close_conn(bool real_close) {
    if (real_close && (m_sockfd != -1)) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 初始化客户连接
void http_conn::init(int sockfd, const sockaddr_in &addr) {
    m_sockfd = sockfd;
    m_address = addr;

    // 获取socket的错误状态，并在调用后清除它
    int error = 0;
    socklen_t len = sizeof(error);
    getsockopt(m_sockfd, SOL_SOCKET, SO_ERROR, &error, &len);
    // 如下两行避免了TIME_WAIT 状态，解决在TIME_WAIT状态期间重新绑定端口的问题
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    // 对每个非监听文件描述符都注册 EPOLLONESHOT 事件
    addfd(m_epollfd, sockfd, true);
    m_user_count++;

    init();
}

void http_conn::init() {
    // 设置主状态机起始状态为 请求行
    m_check_state = CHECK_STATE_REQUESTLINE;
    // keep-alive 为false
    m_linger = false;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 从状态机，用于读取http报文一行的内容
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    /*
    m_checked_idx指向buffer（应用程序的读缓冲区）中当前正在分析的字节，m_read_idx指向buffer中客户数据的尾部的下一字节;
    buffer中第0~m_checked_idx字节都巳分析完毕，第m_read_idx~（m_read_idx-1）字节由下面的循环挨个分析
    */
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r') {
            if ((m_checked_idx + 1) == m_read_idx) {
                return LINE_OPEN;
            } else if (m_read_buf[m_checked_idx + 1] == '\n') {
                // 显式结尾
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }

            return LINE_BAD;
        } else if (temp == '\n') {
            if ((m_checked_idx > 1) &&
                (m_read_buf[m_checked_idx - 1] == '\r')) {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }

    return LINE_OPEN;
}

// 循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read() {
    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }

    int bytes_read = 0;
    while (true) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                          READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return false;
        } else if (bytes_read == 0) {
            return false;
        }

        m_read_idx += bytes_read;
    }
    return true;
}

// 解析HTTP请求行，获得请求方法、目标URL，以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text) {
    // strpbrk
    // 函数用于在字符串中查找第一个匹配参数字符串中任意字符的字符,即请求行中查找空格或制表符
    m_url = strpbrk(text, " \t");
    if (!m_url) {
        return BAD_REQUEST;
    }
    // 将m_url指向的当前位置的字符设置为 '\0',然后指针++
    *m_url++ = '\0';

    char *method = text;
    // 只设计了GET方法
    if (strcasecmp(method, "GET") == 0) {
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }

    // strspn函数用于计算字符串中连续包含在指定字符集合中的字符的长度
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version) {
        return BAD_REQUEST;
    }

    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }

    // 处理URL，如果以"http://"开头，则跳过，指向实际的路径部分
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    // 如果URL为空或不以 '/' 开头，返回BAD_REQUEST
    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析HTTP的请求头
http_conn::HTTP_CODE http_conn::parse_headers(char *text) {
    // 遇到空行，说明完整的HTTP请求
    if (text[0] == '\0') {
        if (m_method == HEAD) {
            return GET_REQUEST;
        }

        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }

        return GET_REQUEST;
    } else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    } else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    } else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else {
        printf("oop! unknow header %s\n", text);
    }

    return NO_REQUEST;
}

// 简易版本，并不真正解析请求体，只是判断是否完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text) {
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }

    return NO_REQUEST;
}

// 主状态机
http_conn::HTTP_CODE http_conn::process_read() {
    // 记录当前行的读取状态
    LINE_STATUS line_status = LINE_OK;
    // 记录HTTP请求的处理结果
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    /*
    两种可能:
    1.请求体，且之前无错误
    2.还在请求行、请求头，这时候需要读取一行
    */
    while (
        ((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) ||
        ((line_status = parse_line()) == LINE_OK)) {
        text = get_line();
        // 修改下一行的起始位置
        m_start_line = m_checked_idx;
        printf("got 1 http line: %s\n", text);

        switch (m_check_state) {
        case CHECK_STATE_REQUESTLINE: {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST) {
                return BAD_REQUEST;
            }
            break;
        }
        case CHECK_STATE_HEADER: {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST) {
                return BAD_REQUEST;
            } else if (ret == GET_REQUEST) {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT: {
            ret = parse_content(text);
            if (ret == GET_REQUEST) {
                return do_request();
            }
            line_status = LINE_OPEN;
            break;
        }
        default: {
            return INTERNAL_ERROR;
        }
        }
    }

    return NO_REQUEST;
}

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request() {
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    // 拼接URL
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    // 使用stat函数获取文件信息，包括文件的状态、大小等
    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }

    // 判断文件是否可读
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }

    // 判断请求的是否是目录
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    int fd = open(m_real_file, O_RDONLY);
    // 将文件映射到内存中，返回映射后的内存地址
    m_file_address =
        (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    // 返回FILE_REQUEST，表示请求的是一个文件，且处理成功
    return FILE_REQUEST;
}

// 对内存块射区执行munmap操作
void http_conn::unmap() {
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 写HTTP响应
bool http_conn::write() {
    int temp = 0;
    int bytes_have_send = 0;
    int bytes_to_send = m_write_idx;
    if (bytes_to_send == 0) {
        // 写完后重新改为读
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while (1) {
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp <= -1) {
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_to_send -= temp;
        bytes_have_send += temp;
        if (bytes_to_send <= bytes_have_send) {
            unmap();
            if (m_linger) {
                init();
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return true;
            } else {
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return false;
            }
        }
    }
}

// 往写缓冲写入待发送的数据
bool http_conn::add_response(const char *format, ...) {
    if (m_write_idx >= WRITE_BUFFER_SIZE) {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx,
                        WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

// 响应行
bool http_conn::add_status_line(int status, const char *title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// 响应头
bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len) {
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger() {
    return add_response("Connection: %s\r\n",
                        (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line() { return add_response("%s", "\r\n"); }

// 响应体
bool http_conn::add_content(const char *content) {
    return add_response("%s", content);
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret) {
    case INTERNAL_ERROR: {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form)) {
            return false;
        }
        break;
    }
    case BAD_REQUEST: {
        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form));
        if (!add_content(error_400_form)) {
            return false;
        }
        break;
    }
    case NO_RESOURCE: {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form)) {
            return false;
        }
        break;
    }
    case FORBIDDEN_REQUEST: {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form)) {
            return false;
        }
        break;
    }
    // 文件请求
    case FILE_REQUEST: {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0) {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        } else {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string)) {
                return false;
            }
        }
    }
    default: {
        return false;
    }
    }

    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process() {
    HTTP_CODE read_ret = process_read();

    // http没读完，重置
    if (read_ret == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    // 根据读取结果，写http响应报文
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
    }

    // 修改为写事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}
