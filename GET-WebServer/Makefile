src = $(wildcard ./src/*.cpp)
obj = $(patsubst ./src/%.cpp, ./obj/%.o, $(src))

inc_path = ./inc

myArgu = -Wall -g

ALL: server.out

# 检查并创建obj文件夹
check_obj_dir:
	@mkdir -p ./obj

$(obj): ./obj/%.o: ./src/%.cpp | check_obj_dir
	g++ -c $< -o $@ $(myArgu) -I $(inc_path)

server.out: $(obj)
	g++ $^ -o $@ $(myArgu)

clean:
	-rm -rf ./obj server.out

.PHONY: clean ALL check_obj_dir
