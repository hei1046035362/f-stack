#ifdef 	__TGG_FDTIMER_H
#define __TGG_FDTIMER_H
#include "heap_timer.h"
#include <string>

namespace NS_MICRO_THREAD
{


class CTimerTggCliFd : public CTimerNotify
{
public:

    virtual void timer_notify() { return;};

    CTimerNotify() : _time_expired(180) {};

    virtual ~CTimerNotify(){};

private:
	int fd;
	///< 客户端要用cid做校验，fd被回收的速度太快了，容易把新建立的连接断掉
	std::string _cid;
};

class CTimerTggBwFd : public CTimerNotify
{
public:

    virtual void timer_notify() { return;};

    CTimerNotify() : _time_expired(300) {};

    virtual ~CTimerNotify(){};
private:
	// BW就用fd就行了，本身比较稳定
	int fd;
};

}

#endif // __TGG_FDTIMER_H