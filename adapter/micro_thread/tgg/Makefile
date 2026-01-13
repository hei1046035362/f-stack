ifeq ($(FF_PATH),)
    FF_PATH=/usr/local
    $(warning FF_PATH environment variable not defined, default FF_PATH=/usr/local)
endif

ifneq ($(shell pkg-config --exists libdpdk && echo 0),0)
$(error "no installation of DPDK found, maybe you shuld export environment variable `PKG_CONFIG_PATH`")
endif

SUBDIRS := $(wildcard tools/.)

PKGCONF ?= pkg-config
INSTALL_PATH = /usr/local/tgg_gateway
TGG_CONF = $(INSTALL_PATH)/conf/tgg_conf.ini
IP_W_FILTER_CONF = $(INSTALL_PATH)/conf/ip_filter/whitelist.txt
IP_B_FILTER_CONF = $(INSTALL_PATH)/conf/ip_filter/blacklist.txt
IP_E_FILTER_CONF = $(INSTALL_PATH)/conf/ip_filter/excludelist.txt
DEBUG= -g
BINARY = gwrcv gwcliprc gwbwprc gwredis gwregister
FF_LIBS+= $(shell $(PKGCONF) --static --libs libdpdk)
FF_LIBS+= -L${FF_PATH}/lib -Wl,--whole-archive,-lfstack,--no-whole-archive \
		-L${FF_PATH}/app/libco/lib
FF_LIBS+= -Wl,--no-whole-archive -lrt -lm -ldl -lcrypto -lpthread -lnuma -lstdc++ -z noexecstack

# Comment the following line if you are not using the gnu c compiler
#C_ARGS = -Wall -g -fPIC -D_DEBUG
C_ARGS = -Wall -g -fPIC -O0 -DMT_REAL_TIME -Wno-register -Wa,--noexecstack
#.SUFFIXES: .o .cpp

ifeq ($(ARCH),32)
	C_ARGS +=  -march=pentium4 -m32 -DSUS_LINUX -pthread
else
	C_ARGS +=  -march=native -m64 -DSUS_LINUX -pthread
endif
# You might have to change this if your c compiler is not cc
CC = g++

# You shouldn't need to make any more changes below this line.
INCCOMM=-I./valgrind -I$(FF_PATH)/lib -I$(FF_PATH)/adapter/micro_thread -I$(FF_PATH)/adapter/micro_thread/tgg -I${FF_PATH}/app/libco/ \
		-I${FF_PATH}/app/rapidjson/include
SOURCE_FILES := $(wildcard cmd/*.c cmd/*.cpp comm/*.c comm/*.cpp tgg_comm/*.c tgg_comm/*.cpp reactor/reactor.c reactor/timeout_wheel.c ./*.c)
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
CRESET =  ; EXIT_CODE=$$?; echo -ne \\e[m; exit $$EXIT_CODE #test -s $@

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
	@rm -f $(BINARY) *.a *.o gwrcv gwprc  tgg_comm/*.o cmd/*.o comm/*.o reactor/*.o
	@for dir in $(SUBDIRS); do \
		if [ -f $$dir/Makefile ]; then \
			echo "Cleaning in $$dir"; \
			$(MAKE) -C $$dir clean || exit 1; \
		fi \
	done


# MASTER_OBJ = dpdk_init.o master.o tgg_comm/tgg_common.o tgg_comm/tgg_bwcomm.o tgg_comm/tgg_bw_cache.o comm/log.o\
# 			comm/Websocket.o comm/Encrypt.o tgg_comm/WsConsumer.o tgg_comm/tgg_transport.o\
# 			tgg_comm/tgg_lock.o comm/RWIni.o tgg_comm/tgg_conf.o comm/common.o tgg_comm/tgg_data_opt.o \
# 			tgg_comm/tgg_master_timers.o tgg_comm/tgg_ip_filter.o comm/picohttpparser.o

MASTER_OBJ = dpdk_init.o master_reactor.o reactor/reactor.o reactor/timeout_wheel.o \
			tgg_comm/tgg_common.o tgg_comm/tgg_bwcomm.o tgg_comm/tgg_bw_cache.o comm/log.o\
			comm/Websocket.o comm/Encrypt.o tgg_comm/WsConsumer.o tgg_comm/tgg_transport.o\
			tgg_comm/tgg_lock.o comm/RWIni.o tgg_comm/tgg_conf.o comm/common.o tgg_comm/tgg_data_opt.o \
			tgg_comm/tgg_master_timers.o tgg_comm/tgg_ip_filter.o comm/picohttpparser.o comm/MurmurHash3.o


CLIPRC_OBJ = cliprc.o dpdk_init.o tgg_comm/tgg_bw_cache.o comm/log.o\
				tgg_comm/tgg_bwcomm.o tgg_comm/tgg_common.o tgg_comm/tgg_data_opt.o \
				tgg_comm/tgg_lock.o comm/Websocket.o comm/Encrypt.o \
				tgg_comm/WsConsumer.o tgg_comm/tgg_conf.o comm/common.o \
				comm/RWIni.o tgg_comm/tgg_cliprc.o tgg_comm/tgg_transport.o \
				comm/picohttpparser.o comm/MurmurHash3.o


BWSERVER_OBJ = bwprc.o dpdk_init.o cmd/CmdProcessor.o tgg_comm/tgg_bw_cache.o comm/log.o\
				tgg_comm/tgg_bwcomm.o tgg_comm/tgg_common.o tgg_comm/tgg_data_opt.o \
				tgg_comm/tgg_lock.o comm/Encrypt.o cmd/TggCmdProcessor.o \
				comm/Serialize.o tgg_comm/tgg_conf.o tgg_comm/tgg_bwserver.o \
				comm/RWIni.o tgg_comm/tgg_transport.o comm/common.o comm/Websocket.o \
				comm/picohttpparser.o comm/MurmurHash3.o

REGISTER_OBJ = register.o dpdk_init.o tgg_comm/tgg_bw_cache.o comm/log.o\
				tgg_comm/tgg_bwcomm.o tgg_comm/tgg_common.o tgg_comm/tgg_data_opt.o \
				tgg_comm/tgg_lock.o \
				tgg_comm/tgg_conf.o tgg_comm/tgg_register.o \
				comm/RWIni.o comm/common.o comm/picohttpparser.o comm/MurmurHash3.o

# 日志是否写入文件，如果要输出到控制台，屏蔽下面这一行
C_ARGS += -DLOG_TO_FILE

gwrcv: $(MASTER_OBJ)
	@echo -e Compile $(CYAN)$@$(RESET) ...$(RED)
	@$(CC) -O -gdwarf-2 -o $@ $^ -ldl -lz -lssl $(FF_LIBS) -L$(FF_PATH)/adapter/micro_thread -lmt $(CRESET)

# gwrcv: $(MASTER_REACTOR_OBJ)
# 	@echo -e Compile $(CYAN)$@$(RESET) ...$(RED)
# 	@$(CC) -O -gdwarf-2 -o $@ $^ -ldl -lz -lssl $(FF_LIBS) -L$(FF_PATH)/adapter/micro_thread -lmt $(CRESET)

gwcliprc: $(CLIPRC_OBJ)
	@echo -e Compile $(CYAN)$@$(RESET) ...$(RED)
	@$(CC) -O -gdwarf-2 -o $@ $^ -ldl -lz -lssl $(FF_LIBS) -L$(FF_PATH)/adapter/micro_thread -lmt $(CRESET)

gwbwprc: $(BWSERVER_OBJ)
	@echo -e Compile $(CYAN)$@$(RESET) ...$(RED)
	@$(CC) -O -gdwarf-2 -o $@ $^  -ldl -lz -lcolib -lssl $(FF_LIBS) $(CRESET)

gwregister: $(REGISTER_OBJ)
	@echo -e Compile $(CYAN)$@$(RESET) ...$(RED)
	@$(CC) -O -gdwarf-2 -o $@ $^  -ldl -lz -lcolib -lssl $(FF_LIBS) $(CRESET)

all: gwrcv gwcliprc gwbwprc gwregister build-subdirs

build-subdirs:
	@for dir in $(SUBDIRS); do \
		if [ -f $$dir/Makefile ]; then \
			echo "Building in $$dir"; \
			$(MAKE) -C $$dir all || exit 1; \
		fi \
	done

install:
	mkdir -p $(INSTALL_PATH)/bin
	cp -f gwrcv $(INSTALL_PATH)/bin/gwrcv
	cp -f gwcliprc $(INSTALL_PATH)/bin/gwcliprc
	cp -f gwbwprc $(INSTALL_PATH)/bin/gwbwprc
	cp -f gwregister $(INSTALL_PATH)/bin/gwregister
	mkdir -p $(INSTALL_PATH)/conf
	test -f '${TGG_CONF}' || cp -f ${FF_PATH}/adapter/micro_thread/tgg/config/tgg_conf.ini ${TGG_CONF}
	mkdir -p $(INSTALL_PATH)/conf/ip_filter
	test -f '${IP_W_FILTER_CONF}' || touch ${IP_W_FILTER_CONF}
	test -f '${IP_B_FILTER_CONF}' || touch ${IP_B_FILTER_CONF}
	test -f '${IP_E_FILTER_CONF}' || touch ${IP_E_FILTER_CONF}
service:
	# 拷贝服务和脚本
	cp -f scripts/service/tgg_gateway.service /etc/systemd/system/tgg_gateway.service
	cp -f scripts/service/gw-force-kill.sh $(INSTALL_PATH)/bin/gw-force-kill.sh
	cp -f scripts/service/gw-graceful-stop.sh $(INSTALL_PATH)/bin/gw-graceful-stop.sh
	cp -f scripts/service/gw-pre-start.sh $(INSTALL_PATH)/bin/gw-pre-start.sh
	cp -f scripts/service/gw-start.sh $(INSTALL_PATH)/bin/gw-start.sh
	cp -f scripts/dpdk_init.sh $(INSTALL_PATH)/bin/dpdk_init.sh
	chmod a+x $(INSTALL_PATH)/bin/*
	systemctl daemon-reload
	systemctl enable tgg_gateway
