# 内存泄漏检测：tcmalloc
## 1、安装依赖
sudo apt install -y google-perftools libgoogle-perftools-dev
sudo apt install google-perftools  # Ubuntu

## 2、添加代码
#include <gperftools/heap-profiler.h>

int main() {
    HeapProfilerStart("gwrcv.hprof");
    // 你的代码
    HeapProfilerStop();
    return 0;
}

## 3、编译链接tcmalloc
 -ltcmalloc -g -rdynamic

## 4、运行并生成堆分析
# HEAPPROFILE=/tmp/gwrcv.hprof 
HEAPCHECK=strict ./gwrcv

## 5、生成svg文件
# 使用pprof分析
pprof --gv ./gwrcv /tmp/gwrcv.0001.heap
# 或
google-pprof --svg ./gwrcv ./gwrcv_reactor.hprof.0001.heap >leak.svg

google-pprof --svg --alloc_space ./gwrcv "./gwrcv_reactor.hprof.0001.heap" --lines --edgefraction=1e-10 --nodefraction=1e-10 > ./gwrcv_alloc.svg