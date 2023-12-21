src = $(wildcard ./src/**/*.cpp ./src/*.cpp)
obj = $(patsubst ./src/%.cpp, ./obj/%.o, $(src))

inc_path = ./inc

myArgu = -Wall -g

# 需要链接的库
LIBS = -lpthread -lmysqlclient

ALL: check_obj_dir server

# 检查并创建obj文件夹
check_obj_dir:
	@mkdir -p $(dir $(obj))

$(obj): ./obj/%.o: ./src/%.cpp | check_obj_dir
	g++ -c $< -o $@ $(myArgu) -I $(inc_path)

server: $(obj)
	g++ $^ -o $@ $(myArgu) $(LIBS)

clean:
	-rm -rf ./obj server

.PHONY: clean ALL check_obj_dir
