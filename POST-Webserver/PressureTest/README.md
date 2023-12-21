

服务器压力测试
===============
Webbench是有名的网站压力测试工具，它是由[Lionbridge](http://www.lionbridge.com)公司开发。

> * 测试处在相同硬件上，不同服务的性能以及不同硬件上同一个服务的运行状况。
> * 展示服务器的两项内容：每秒钟响应请求数和每秒钟传输数据量。

- Webbench最多可以对网站模拟3w左右的并发请求。




测试规则
------------
* 测试示例

    ```C++
	./webbench -c 500 -t 10 http://192.168.141.128:12345/
    ```
* 参数

> * `-c` 表示客户端数
> * `-t` 表示时间





<div align=center><img src="https://github.com/twomonkeyclub/TinyWebServer/blob/master/root/testresult.png" height="201"/> </div>

