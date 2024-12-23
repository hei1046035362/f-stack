ifeq ($(FF_PATH),)
    FF_PATH=/usr/local
    $(warning FF_PATH environment variable not defined, default FF_PATH=/usr/local)
endif

ifneq ($(shell pkg-config --exists libdpdk && echo 0),0)
$(error "no installation of DPDK found, maybe you shuld export environment variable `PKG_CONFIG_PATH`")
endif

PKGCONF ?= pkg-config

DEBUG= -g
BINARY = gwrcv gwprc gwredis
FF_LIBS+= $(shell $(PKGCONF) --static --libs libdpdk)
FF_LIBS+= -L${FF_PATH}/lib -Wl,--whole-archive,-lfstack,--no-whole-archive -L$(FF_PATH)/adapter/micro_thread -lmt
FF_LIBS+= -Wl,--no-whole-archive -lrt -lm -ldl -lcrypto -lpthread -lnuma -lhiredis -lstdc++

# Comment the following line if you are not using the gnu c compiler
#C_ARGS = -Wall -g -fPIC -D_DEBUG
C_ARGS = -Wall -g -fPIC -O0 -DMT_REAL_TIME -Wno-register
#.SUFFIXES: .o .cpp

ifeq ($(ARCH),32)
	C_ARGS +=  -march=pentium4 -m32 -DSUS_LINUX -pthread
else
	C_ARGS +=  -march=native -m64 -DSUS_LINUX -pthread
endif
# You might have to change this if your c compiler is not cc
CC = g++

# You shouldn't need to make any more changes below this line.
INCCOMM=-I./valgrind -I$(FF_PATH)/lib -I$(FF_PATH)/adapter/micro_thread -I$(FF_PATH)/adapter/micro_thread/tgg
SOURCE_FILES := $(wildcard cmd/*.c cmd/*.cpp comm/*.c comm/*.cpp tgg_comm/*.c tgg_comm/*.cpp ./*.c unittest/*.cpp)
#.c.o:
#	$(CC) -c $*.c

# all:	$(BINARY) 

### shell color config
RED = \\e[1m\\e[31m
DARKRED = \\e[31m
GREEN = \\e[1m\\e[32m
DARKGREEN = \\e[32m
BLUE = \\e[1m\\e[34m
DARKBLUE = \\e[34m
YELLOW = \\e[1m\\e[33m
DARKYELLOW = \\e[33m
MAGENTA = \\e[1m\\e[35m
DARKMAGENTA = \\e[35m
CYAN = \\e[1m\\e[36m
DARKCYAN = \\e[36m
RESET = \\e[m
CRESET =  ;echo -ne \\e[m; #test -s $@

%.o: %.cpp
	@echo -e Compiling $(GREEN)$<$(RESET) ...$(RED)
	@$(CC) $(C_ARGS) -c -o $@ $< $(INCCOMM) $(CRESET)

%.o: %.c
	@echo -e Compiling $(GREEN)$<$(RESET) ...$(RED)
	@$(CC) $(C_ARGS) -c -o $@ $< $(INCCOMM) $(CRESET)  

%.o: %.S
	@echo -e Compiling $(GREEN)$<$(RESET) ...$(RED)
	@$(CC) $(C_ARGS) -c -o $@ $< $(INCCOMM) $(CRESET) 

clean:
	@rm -f $(BINARY) *.a *.o gwrcv gwprc  tgg_comm/*.o cmd/*.o comm/*.o


MASTER_OBJ = dpdk_init.o master.o tgg_comm/tgg_common.o tgg_comm/tgg_bw_cache.o \
			tgg_comm/tgg_lock.o

PROCESS_OBJ = process.o dpdk_init.o bwserver.o cmd/CmdProcessor.o tgg_comm/tgg_bw_cache.o\
				tgg_comm/tgg_bwcomm.o tgg_comm/tgg_common.o tgg_comm/tgg_data_opt.o \
				tgg_comm/tgg_lock.o comm/Websocket.o comm/Encrypt.o comm/RedisClient.o \
				tgg_comm/WsConsumer.o comm/Serialize.o

REDIS_OBJ = unittest/rediscli_test.o tgg_comm/tgg_bw_cache.o tgg_comm/tgg_common.o \
			tgg_comm/tgg_data_opt.o dpdk_init.o tgg_comm/tgg_lock.o comm/Encrypt.o \
			comm/RedisClient.o tgg_comm/tgg_bwcomm.o

gwrcv: $(MASTER_OBJ)
	@echo -e Compile $(CYAN)$@$(RESET) ...$(RED)
	@$(CC) -O -gdwarf-2 -o $@ $^ -ldl $(FF_LIBS) $(CRESET)

gwprc: $(PROCESS_OBJ)
	@echo -e Compile $(CYAN)$@$(RESET) ...$(RED)
	@$(CC) -O -gdwarf-2 -o $@ $^ -ldl -lz -lssl $(FF_LIBS) $(CRESET)

gwredis: $(REDIS_OBJ)
	@echo -e Compile $(CYAN)$@$(RESET) ...$(RED)
	@$(CC) -O -gdwarf-2 -o $@ $^ -lz -lssl $(FF_LIBS) $(CRESET)


all: gwrcv gwprc gwredis