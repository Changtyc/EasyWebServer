src = $(wildcard ./*.cpp)
obj = $(patsubst ./%.cpp, ./%.o, $(src))

inc_path = ./

myArgu = -Wall -g

ALL:server.out

$(obj):./%.o:./%.cpp
	g++ -c $< -o $@ $(myArgu) -I $(inc_path)

server.out:$(obj)
	g++ $^ -o $@ $(myArgu)

clean:
	-rm -rf $(obj) server.out

.PHONY: clean ALL