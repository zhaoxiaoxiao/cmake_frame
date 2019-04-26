
#include <sys/poll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/cdefs.h>

#include <unistd.h>

#include "Incommon.h"
#include "LibeventIo.h"

#pragma GCC diagnostic ignored "-Wold-style-cast"
namespace common
{
static int createEventfd()
{
    int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evtfd < 0)
    {
        PERROR("Failed in eventfd");
    }
    return evtfd;
}

static void wakeUpFdcb(int sockFd, short eventType, void *arg)
{
    LibeventIo *p = (LibeventIo *)arg;
    p->handleRead();
}

LibeventIo::LibeventIo()
    :wakeupFd(createEventfd())
    ,evbase(nullptr)
    ,lastSecond_(0)
    ,wake_event()
    ,orderDeque_()
    ,ioRedisClients_()
{
    PDEBUG("LibeventIo init");
}

LibeventIo::~LibeventIo()
{
    PERROR("~LibeventIo exit");
}

int LibeventIo::libeventIoReady()
{
    PDEBUG("libeventIoReady in");
    if(wakeupFd < 0)
    {
        PERROR("libeventIoReady wakeupFd_ error %d", wakeupFd);
        return -1;
    }

    evbase = event_base_new();
    if(evbase == nullptr)
    {
        PERROR("libeventIoReady event_base_new new error");
        return -1;
    }

    event_assign(&wake_event, evbase, wakeupFd, EV_READ|EV_PERSIST, wakeUpFdcb, this);
    event_add(&wake_event, NULL);

    event_base_dispatch(evbase);
    PERROR("event_base_dispatch return %p", evbase);

    event_del(&wake_event);
    close(wakeupFd);
    event_base_free(evbase);
    evbase = nullptr;
    return 0;
}

int LibeventIo::libeventIoOrder(const RedisCliOrderNodePtr& node)
{
    orderDeque_.orderNodeInsert(node);
    libeventIoWakeup();
    return 0;
}

int LibeventIo::libeventIoWakeup()
{
    uint64_t one = 2;
    ssize_t n = write(wakeupFd, &one, sizeof one);
    //INFO("wakeup n one %ld %ld %p", n, one, evbase);
    if (n != sizeof one)
    {
        PERROR("EventLoop::wakeup() writes %ld bytes instead of 8", n);
    }
    return 0;
}

int LibeventIo::libeventIoExit()
{
    uint64_t one = 1;
    ssize_t n = write(wakeupFd, &one, sizeof one);
    PDEBUG("LibeventIo n one %ld %ld %p", n, one, evbase);
    if (n != sizeof one)
    {
        PERROR("EventLoop::wakeup() writes %ld bytes instead of 8", n);
    }
    return 0;
}

void LibeventIo::ioDisRedisClient(const REDIS_ASYNC_CLIENT::RedisClientPtr& cli)
{
    for(size_t i = 0; i < ioRedisClients_.size(); i++)
    {
        if(ioRedisClients_[i].get() == cli.get())
        {
            ioRedisClients_[i].reset();
        }
    }
}

void LibeventIo::ioAddRedisClient(const REDIS_ASYNC_CLIENT::RedisClientPtr& cli)
{
    for(size_t i = 0; i < ioRedisClients_.size(); i++)
    {
        if(ioRedisClients_[i].get() == cli.get())
        {
            return;
        }
    }

    for(size_t i = 0; i < ioRedisClients_.size(); i++)
    {
        if(ioRedisClients_[i] == nullptr)
        {
            ioRedisClients_[i] = cli;
        }
    }

    ioRedisClients_.push_back(cli);
}

void LibeventIo::handleRead()
{
    uint64_t one = 1;
    ssize_t n = read(wakeupFd, &one, sizeof one);
    if (n != sizeof one)
    {
        PERROR("LibeventIo::handleRead() reads %ld bytes instead of 8", n);
        return;
    }
    PDEBUG("handleRead n one %ld %ld :: %p", n, one, evbase);

    while(1)
    {
        RedisCliOrderNodePtr node = orderDeque_.dealOrderNode();
        if(node)
        {
            if(node->cli_)
            {
                if(node->cmdOrd_ == nullptr)
                {
                    if(node->cli_->tcpCliState() == REDIS_CLIENT_STATE_INIT)
                    {
                        node->cli_->connectServer(evbase);
                        ioAddRedisClient(node->cli_);
                    }else{
                        node->cli_->disConnect();
                    }
                }else{
                    if(node->cli_->tcpCliState() == REDIS_CLIENT_STATE_INIT)
                    {
                        PERROR("TODO");
                    }else{
                        node->cli_->requestCmd(node->cmdOrd_);
                    }
                }
            }else{
                PERROR("TODO");
            }
        }else{
            break;
        }
    }
    uint64_t num = one % 2;
    if(num)
    {
        event_base_loopbreak(evbase);
        return;
    }

    if(lastSecond_ == 0)
    {
        lastSecond_ = secondSinceEpoch();
    }else{
        uint64_t nowSecond = secondSinceEpoch();
        if(nowSecond != lastSecond_)
        {
            for(size_t i = 0; i < ioRedisClients_.size(); i++)
            {
                if(ioRedisClients_[i])
                {
                    ioRedisClients_[i]->checkOutSecondCmd(nowSecond);
                }
            }
            lastSecond_ = nowSecond;
        }
    }
}

}

