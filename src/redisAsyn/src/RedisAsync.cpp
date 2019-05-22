
#include <stdarg.h>
#include <string.h>

#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#include <async.h>

#include "Atomic.h"
#include "Incommon.h"
#include "Thread.h"
#include "ThreadPool.h"
#include "OrderInfo.h"
#include "LibeventIo.h"
#include "RedisClientMgr.h"
#include "../RedisAsync.h"

namespace CLUSTER_REDIS_ASYNC
{
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//global variable define
static std::unique_ptr<common::ThreadPool> eventIoPoolPtr(nullptr);
static std::unique_ptr<common::ThreadPool> callBackPoolPtr(nullptr);
static std::unique_ptr<common::Thread> timerThreadPtr(nullptr);

static std::vector<common::LibeventIoPtr> libeventIoPtrVect;
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//init paramter
static common::AtomicUInt32 initIoIndex;
static common::AtomicInt32 asyncCmdNum;
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static REDIS_ASYNC_CLIENT::VectRedisClientMgrPtr vectRedisCliMgr;
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

RedisAsync::RedisAsync()
    :callBackNum_(0)
    ,ioThreadNum_(0)
    ,keepSecond_(0)
    ,connOutSecond_(0)
    ,isExit_(0)
    ,timerExit_(0)
    ,nowSecond_(0)
    ,initMutex_()
    ,stateCb_(nullptr)
    ,exceCb_(nullptr)
{
    PDEBUG("RedisAsync init");
}

RedisAsync::~RedisAsync()
{
    PERROR("~RedisAsync exit");
}

void RedisAsync::redisAsyncExit()
{
    isExit_ = 1;
    
    do{
        if(asyncCmdNum.get() == 0)
        {
            break;
        }
        PERROR("This still async cmd num %d", asyncCmdNum.get());
        usleep(1000);
    }while(1);
    
    if(callBackPoolPtr)
    {
        callBackPoolPtr->stop();
    }

    timerExit_ = 1;
    if(timerThreadPtr)
    {
        timerThreadPtr->join();
    }

    for(int i = 0; i < ioThreadNum_; i++)
    {
        size_t index = static_cast<size_t>(i);
        if(libeventIoPtrVect[index])
        {
            libeventIoPtrVect[index]->libeventIoExit();
        }
    }

    if(eventIoPoolPtr)
    {
        eventIoPoolPtr->stop();
    }
}

int RedisAsync::redisAsyncInit(int threadNum, int callbackNum, int connOutSecond, int keepSecond, const StateChangeCb& statecb, const ExceptionCallBack& excecb)
{
    std::lock_guard<std::mutex> lock(initMutex_);
    if(callBackNum_ && ioThreadNum_ && keepSecond_ && connOutSecond_)
    {
        PERROR("set already");
        return INIT_SUCCESS_CODE;
    }

    if(threadNum < 1 || callbackNum < 1 || connOutSecond < 1 || keepSecond < 1)
    {
        PERROR("threadNum %d callbackNum %d connOutSecond %d keepSecond %d", threadNum, callbackNum, connOutSecond, keepSecond);
        return INIT_PARAMETER_ERROR;
    }

    eventIoPoolPtr.reset(new common::ThreadPool("eventIo"));
    if(eventIoPoolPtr == nullptr)
    {
        PERROR("redisAsyncInit IO thread pool new error");
        return INIT_SYSTEM_ERROR;
    }
    eventIoPoolPtr->start(threadNum);

    callBackPoolPtr.reset(new common::ThreadPool("callbk"));
    if(callBackPoolPtr == nullptr)
    {
        PERROR("redisAsyncInit CALLBACK thread pool new error");
        return INIT_SYSTEM_ERROR;
    }
    callBackPoolPtr->start(callbackNum);

    timerThreadPtr.reset(new common::Thread(std::bind(&CLUSTER_REDIS_ASYNC::RedisAsync::timerThreadRun, this), "timer"));
    if(timerThreadPtr == nullptr)
    {
        PERROR("redisAsyncInit TIMER thread new error");
        return INIT_SYSTEM_ERROR;
    }
    timerThreadPtr->start();

    ioThreadNum_ = threadNum;
    callBackNum_ = callbackNum;
    connOutSecond_ = connOutSecond;
    keepSecond_ = keepSecond;
    stateCb_ = statecb;
    exceCb_ = excecb;
    nowSecond_ = secondSinceEpoch();

    for(int i = 0; i < threadNum; i++)
    {
        common::LibeventIoPtr tmpEventIo(new common::LibeventIo());
        if(tmpEventIo == nullptr)
        {
            return INIT_SYSTEM_ERROR;
        }
        libeventIoPtrVect.push_back(tmpEventIo);
    }

    for(int i = 0; i < ioThreadNum_; i++)
    {
        eventIoPoolPtr->run(std::bind(&CLUSTER_REDIS_ASYNC::RedisAsync::libeventIoThread, this, i));
    }

    return INIT_SUCCESS_CODE;
}

void RedisAsync::timerThreadRun()
{
    while(1)
    {
        if(timerExit_)
        {
            break;
        }
        sleep(1);
        if(timerExit_)
        {
            break;
        }

        nowSecond_ = secondSinceEpoch();
        size_t subIndex = 0;
        for(int i = 0; i < ioThreadNum_; i++)
        {
            if(libeventIoPtrVect[subIndex])
            {
                libeventIoPtrVect[subIndex]->libeventIoWakeup(nowSecond_);
            }
            subIndex++;
        }

        do{
            common::RedisCliOrderNodePtr node = common::TimerOrderDeque::instance().dealTimerOrder(nowSecond_);
            if(node)
            {
                node->outSecond_ = 0;
                libeventIoPtrVect[node->cli_->redisMgrfd()]->libeventIoOrder(node, nowSecond_);
            
}else{
                break;
            }
        }while(true);
    }
}

void RedisAsync::libeventIoThread(int index)
{
    int ret = 0;
    size_t subIndex = static_cast<size_t>(index);
    PDEBUG("io thread index %d entry", index);
    while(1)
    {
        if(libeventIoPtrVect[subIndex])
        {
            ret = libeventIoPtrVect[subIndex]->libeventIoReady();
            PERROR("LibeventIoPtrVect[%d]->asyncCurlReady ret %d", index, ret);
        }else{
            PERROR("LibeventTcpCli %d new io Thread object error", index);
        }
        if(isExit_)
        {
            break;
        }

        libeventIoPtrVect[subIndex].reset(new common::LibeventIo());
    }
    PDEBUG("io thread index %d out", index);

    return;
}

void RedisAsync::cmdReplyCallPool()
{
    PDEBUG("cb thread poll callback");
    common::OrderNodePtr node = common::CmdResultQueue::instance().takeCmdResult();
    if(node == nullptr)
    {
        PERROR("cb thread poll get node nullptr");
        return;
    }
    if(node && node->resultCb_)
    {
        asyncCmdNum.decrement();
        PTRACE("asyncCmdNum %d", asyncCmdNum.get());
        node->resultCb_(node->cmdRet_, node->cmdPri_, node->cmdResult_);
    }
}

void RedisAsync::asyncRequestCmdAdd()
{
    asyncCmdNum.increment();
    PTRACE("asyncCmdNum %d", asyncCmdNum.get());
}

void RedisAsync::asyncCmdResultCallBack()
{
    PDEBUG("IO Asynchronous ready cb");
    callBackPoolPtr->run(std::bind(&CLUSTER_REDIS_ASYNC::RedisAsync::cmdReplyCallPool, this));
}

void RedisAsync::redisSvrMgrInvalid(size_t asyFd)
{
    if(vectRedisCliMgr[asyFd]->nodeCli_ && vectRedisCliMgr[asyFd]->nodeCli_->redisClient_)
    {
        libeventIoPtrVect[vectRedisCliMgr[asyFd]->nodeCli_->redisClient_->redisCliIoIndex()]->ioDisRedisClient(vectRedisCliMgr[asyFd]->nodeCli_->redisClient_);
    }
    
    if(vectRedisCliMgr[asyFd]->mSlaveCli_)
    {
        if(vectRedisCliMgr[asyFd]->mSlaveCli_->master_ && vectRedisCliMgr[asyFd]->mSlaveCli_->master_->redisClient_)
        {
            libeventIoPtrVect[vectRedisCliMgr[asyFd]->mSlaveCli_->master_->redisClient_->redisCliIoIndex()]->ioDisRedisClient(vectRedisCliMgr[asyFd]->mSlaveCli_->master_->redisClient_);
        }

        for(REDIS_ASYNC_CLIENT::VectRedisSvrCliPtr::iterator iter = vectRedisCliMgr[asyFd]->mSlaveCli_->slave_.begin(); iter != vectRedisCliMgr[asyFd]->mSlaveCli_->slave_.end(); iter++)
        {
            libeventIoPtrVect[(*iter)->redisClient_->redisCliIoIndex()]->ioDisRedisClient((*iter)->redisClient_);
        }
    }
    
    if(vectRedisCliMgr[asyFd]->cluterCli_)
    {
        for(REDIS_ASYNC_CLIENT::VectRedisClusterNodePtr::iterator iter = vectRedisCliMgr[asyFd]->cluterCli_->clusterVectInfo_.begin(); iter != vectRedisCliMgr[asyFd]->cluterCli_->clusterVectInfo_.end(); iter++)
        {
            if((*iter)->clusterNode_)
            {
                if((*iter)->clusterNode_->master_ && (*iter)->clusterNode_->master_->redisClient_)
                {
                    libeventIoPtrVect[(*iter)->clusterNode_->master_->redisClient_->redisCliIoIndex()]->ioDisRedisClient((*iter)->clusterNode_->master_->redisClient_);
                }

                for(REDIS_ASYNC_CLIENT::VectRedisSvrCliPtr::iterator it = (*iter)->clusterNode_->slave_.begin(); it != (*iter)->clusterNode_->slave_.end(); it++)
                {
                    libeventIoPtrVect[(*it)->redisClient_->redisCliIoIndex()]->ioDisRedisClient((*it)->redisClient_);
                }
            }
        }
    }
    
    callBackPoolPtr->run(std::bind(&CLUSTER_REDIS_ASYNC::RedisAsync::asyncStateCallBack, this, asyFd, REDIS_SVR_INVALID_STATE, vectRedisCliMgr[asyFd]->inSvrInfoStr_));
    PTRACE("vectRedisCliMgr[asyFd].reset() %ld begin", vectRedisCliMgr[asyFd]->nodeCli_->redisClient_.use_count());
    vectRedisCliMgr[asyFd].reset();
    PTRACE("vectRedisCliMgr[asyFd].reset() end");
}


/*
9be7650002bdbfd6b099c07969c6969d120b2b6c 192.169.6.211:6790@16790 master - 0 1557940444069 5 connected 10923-16383
af805d65d9ee40af96695a213b300afd2f5e24d6 192.169.6.234:6791@16791 slave a4892e9a69d9e2858a9d0f45a915c40734b8a4aa 0 1557940443067 3 connected
1adf947915d6f3f4bc9943831734d34b7e1fc610 192.169.6.234:6790@16790 myself,slave 19b1e55b4b30bb9c48b504121f36bd253fe7d30f 0 1557940442000 1 connected
37e495750f36270d3a55a41a126f940316c54c8c 192.169.6.211:6791@16791 slave 9be7650002bdbfd6b099c07969c6969d120b2b6c 0 1557940446071 6 connected
a4892e9a69d9e2858a9d0f45a915c40734b8a4aa 192.169.6.233:6790@16790 master - 0 1557940441000 3 connected 5461-10922
19b1e55b4b30bb9c48b504121f36bd253fe7d30f 192.169.6.233:6791@16791 master - 0 1557940445070 7 connected 0-5460
*/
void RedisAsync::clusterInitCallBack(int ret, void* priv, const StdVectorStringPtr& resultMsg)
{
    PDEBUG("ret %d priv %p", ret , priv);
    for(StdVectorStringPtr::const_iterator iter = resultMsg.begin(); iter != resultMsg.end(); iter++)
    {
        PDEBUG("\n%s", (*iter)->c_str());
    }
    
    if(ret == 0)
    {
        REDIS_ASYNC_CLIENT::RedisClientMgr *pClient = static_cast<REDIS_ASYNC_CLIENT::RedisClientMgr *>(priv);
        std::vector<std::string> vLines;
        split(*resultMsg[0], "\n", vLines);
        for (size_t i = 0; i < vLines.size(); ++i)
        {
            std::vector<std::string> nodeInfo;
            split(vLines[i], " ", nodeInfo);
            if (nodeInfo.size() < 8)
            {
                pClient->cluterCli_.reset();//TODO
                return;
            }

            bool addMasConnCmd = true, addSalConnCmd = true;
            if(strstr(nodeInfo[2].c_str(), "master"))
            {
                if (nodeInfo.size() < 9)
                {
                    pClient->cluterCli_.reset();//TODO
                    return;
                }

                std::string::size_type pos = nodeInfo[1].find(':');
                if (pos == std::string::npos)
                {
                    pClient->cluterCli_.reset();//TODO
                    return;
                }

                std::string portStr = nodeInfo[1].substr(pos + 1);
                int tmpPort  = atoi(portStr.c_str());
                std::string tmpIpStr = nodeInfo[1].substr(0, pos);
                PDEBUG("master %s::%d", tmpIpStr.c_str(), tmpPort);

                size_t tmpIoIndex = 0;
                REDIS_ASYNC_CLIENT::RedisClientPtr tmpCli(nullptr);
                REDIS_ASYNC_CLIENT::RedisSvrCliPtr tmpScrCli(nullptr);
                REDIS_ASYNC_CLIENT::RedisSvrInfoPtr tmpsvr(new REDIS_ASYNC_CLIENT::RedisSvrInfo(tmpIpStr, tmpPort));
                if(tmpsvr == nullptr)
                {
                    pClient->cluterCli_.reset();//TODO
                    return;
                }

                if((*pClient->nodeCli_->svrInfo_) == (*tmpsvr))
                {
                    addMasConnCmd = false;
                    tmpCli = pClient->nodeCli_->redisClient_;
                    tmpScrCli = pClient->nodeCli_;
                }else
                {
                    tmpIoIndex = initIoIndex.incrementAndGet();
                    tmpIoIndex = tmpIoIndex % ioThreadNum_;
                    tmpCli.reset(new REDIS_ASYNC_CLIENT::RedisClient(tmpIoIndex, pClient->mgrFd_, tmpsvr, keepSecond_, connOutSecond_));
                    if(tmpCli == nullptr)
                    {
                        pClient->cluterCli_.reset();//TODO
                        return;
                    }

                    tmpScrCli.reset(new REDIS_ASYNC_CLIENT::RedisSvrCli());
                    if(tmpScrCli == nullptr)
                    {
                        pClient->cluterCli_.reset();//TODO
                        return;
                    }
                    tmpScrCli->svrInfo_ = tmpsvr;
                    tmpScrCli->redisClient_ = tmpCli;
                }

                REDIS_ASYNC_CLIENT::RedisMasterSlavePtr tmpMS(new REDIS_ASYNC_CLIENT::RedisMasterSlave());
                if(tmpMS == nullptr)
                {
                    pClient->cluterCli_.reset();//TODO
                    return;
                }
                tmpMS->master_ = tmpScrCli;

                REDIS_ASYNC_CLIENT::RedisClusterNodePtr tmpClusNode(new REDIS_ASYNC_CLIENT::RedisClusterNode());
                if(tmpClusNode == nullptr)
                {
                    pClient->cluterCli_.reset();//TODO
                    return;
                }
                tmpClusNode->clusterNode_ = tmpMS;
                tmpClusNode->nodeIds_.assign(nodeInfo[0]);

                for(size_t index = 8; index < nodeInfo.size(); index++)
                {
                    REDIS_ASYNC_CLIENT::SlotsInfoPtr tmpSlots(new REDIS_ASYNC_CLIENT::SlotsInfo());
                    if(tmpSlots == nullptr)
                    {
                        pClient->cluterCli_.reset();//TODO
                        return;
                    }
                    pos = nodeInfo[index].find('-');
                    if (pos == std::string::npos)
                    {
                        tmpSlots->slotStart_ = static_cast<uint16_t>(atoi(nodeInfo[index].c_str()));
                        tmpSlots->slotEnd_ = tmpSlots->slotStart_;
                    }
                    else
                    {
                        const std::string slotEndStr = nodeInfo[index].substr(pos + 1);
                        tmpSlots->slotEnd_ = static_cast<uint16_t>(atoi(slotEndStr.c_str()));
                        tmpSlots->slotStart_ = static_cast<uint16_t>(atoi(nodeInfo[index].substr(0, pos).c_str()));
                    }
                    tmpClusNode->vectSlotInfo_.push_back(tmpSlots);
                }

                if(pClient->cluterCli_ == nullptr)
                {
                    pClient->cluterCli_.reset(new REDIS_ASYNC_CLIENT::RedisClusterInfo());
                    if(pClient->cluterCli_ == nullptr)
                    {
                        //TODO
                        return;
                    }
                }
                pClient->cluterCli_->clusterVectInfo_.push_back(tmpClusNode);

                for(size_t j = 0; j < tmpClusNode->vectSlotInfo_.size(); ++j)
                {
                    for(uint16_t jj = tmpClusNode->vectSlotInfo_[j]->slotStart_; jj <= tmpClusNode->vectSlotInfo_[j]->slotEnd_; jj++)
                    {
                        pClient->cluterCli_->clusterSlotMap_.insert(REDIS_ASYNC_CLIENT::SlotCliInfoMap::value_type(jj, tmpCli));
                    }
                }
                
                if(addMasConnCmd)
                {
                    common::RedisCliOrderNodePtr node(new common::RedisCliOrderNode(tmpMS->master_->redisClient_));
                    if(node == nullptr)
                    {
                        pClient->cluterCli_.reset();//TODO
                        return;
                    }
                    libeventIoPtrVect[tmpMS->master_->redisClient_->redisCliIoIndex()]->libeventIoOrder(node, nowSecond_);
                }

                //从节点
                for (size_t j = 0; j < vLines.size(); ++j)
                {
                    std::vector<std::string> tmpSalveInfo;
                    split(vLines[j], " ", tmpSalveInfo);
                    if (tmpSalveInfo.size() < 8)
                    {
                        pClient->cluterCli_.reset();//TODO
                        return;
                    }

                    if(nodeInfo[0].compare(tmpSalveInfo[3]) == 0)
                    {
                        pos = tmpSalveInfo[1].find(':');
                        if (pos == std::string::npos)
                        {
                            pClient->cluterCli_.reset();//TODO
                            return;
                        }

                        portStr = tmpSalveInfo[1].substr(pos + 1);
                        tmpPort  = atoi(portStr.c_str());
                        tmpIpStr = tmpSalveInfo[1].substr(0, pos);
                        PDEBUG("salve %s::%d", tmpIpStr.c_str(), tmpPort);

                        tmpsvr.reset(new REDIS_ASYNC_CLIENT::RedisSvrInfo(tmpIpStr, tmpPort));
                        if(tmpsvr == nullptr)
                        {
                            pClient->cluterCli_.reset();//TODO
                            return;
                        }

                        if((*pClient->nodeCli_->svrInfo_) == (*tmpsvr))
                        {
                            addSalConnCmd = false;
                            tmpCli = pClient->nodeCli_->redisClient_;
                            tmpScrCli = pClient->nodeCli_;
                        }else{
                            tmpIoIndex = initIoIndex.incrementAndGet();
                            tmpIoIndex = tmpIoIndex % ioThreadNum_;
                            tmpCli.reset(new REDIS_ASYNC_CLIENT::RedisClient(tmpIoIndex, pClient->mgrFd_, tmpsvr, keepSecond_, connOutSecond_));
                            if(tmpCli == nullptr)
                            {
                                pClient->cluterCli_.reset();//TODO
                                return;
                            }

                            tmpScrCli.reset(new REDIS_ASYNC_CLIENT::RedisSvrCli());
                            if(tmpScrCli == nullptr)
                            {
                                pClient->cluterCli_.reset();//TODO
                                return;
                            }
                            tmpScrCli->svrInfo_ = tmpsvr;
                            tmpScrCli->redisClient_ = tmpCli;
                        }

                        tmpMS->slave_.push_back(tmpScrCli);

                        if(addSalConnCmd)
                        {
                            common::RedisCliOrderNodePtr node(new common::RedisCliOrderNode(tmpCli));
                            if(node == nullptr)
                            {
                                pClient->cluterCli_.reset();//TODO
                                return;
                            }
                            libeventIoPtrVect[tmpMS->master_->redisClient_->redisCliIoIndex()]->libeventIoOrder(node, nowSecond_);
                        }
                    }
               }
                
            }else{
                continue;
            }
        }
        
    }else{
        PERROR("ret %d priv %p", ret , priv);

        for(StdVectorStringPtr::const_iterator iter = resultMsg.begin(); iter != resultMsg.end(); iter++)
        {
            PERROR("\n%s", (*iter)->c_str());
        }
    }

    return;
}

void RedisAsync::masterSalveInitCb(int ret, void* priv, const StdVectorStringPtr& resultMsg)
{
    PDEBUG("ret %d priv %p", ret , priv);
    for(StdVectorStringPtr::const_iterator iter = resultMsg.begin(); iter != resultMsg.end(); iter++)
    {
        PDEBUG("%s", (*iter)->c_str());
    }
    return;
}

int RedisAsync::addSigleRedisInfo(const std::string& ipPortInfo)
{
    int asyFd = 0;
    std::lock_guard<std::mutex> lock(initMutex_);
    if((callBackNum_ == 0) || (ioThreadNum_ == 0) || (keepSecond_ == 0) || (connOutSecond_ == 0))
    {
        PERROR("threadNum %d callbackNum %d connOutSecond %d keepSecond %d", ioThreadNum_, callBackNum_, connOutSecond_, keepSecond_);
        return INIT_NO_INIT_ERROR;
    }

    REDIS_ASYNC_CLIENT::RedisClientMgrPtr client(new REDIS_ASYNC_CLIENT::RedisClientMgr());
    if(client == nullptr)
    {
        return INIT_SYSTEM_ERROR;
    }

    client->inSvrInfoStr_.assign(ipPortInfo);
    client->svrType_ = REDIS_SINGLE_SERVER;
    client->mgrFd_ = static_cast<int>(vectRedisCliMgr.size());
    asyFd = static_cast<int>(vectRedisCliMgr.size());
    vectRedisCliMgr.push_back(client);

    int ret = redisInitConnect();
    if(ret < 0)
    {
        return ret;
    }
    return asyFd;
}

int RedisAsync::addMasterSlaveInfo(const std::string& ipPortInfo)
{
    int asyFd = 0;
    std::lock_guard<std::mutex> lock(initMutex_);
    if((callBackNum_ == 0) || (ioThreadNum_ == 0) || (keepSecond_ == 0) || (connOutSecond_ == 0))
    {
        PERROR("threadNum %d callbackNum %d connOutSecond %d keepSecond %d", ioThreadNum_, callBackNum_, connOutSecond_, keepSecond_);
        return INIT_NO_INIT_ERROR;
    }

    REDIS_ASYNC_CLIENT::RedisClientMgrPtr client(new REDIS_ASYNC_CLIENT::RedisClientMgr());
    if(client == nullptr)
    {
        return INIT_SYSTEM_ERROR;
    }

    client->inSvrInfoStr_.assign(ipPortInfo);
    client->svrType_ = REDIS_MASTER_SLAVE_SERVER;
    client->mgrFd_ = static_cast<int>(vectRedisCliMgr.size());
    asyFd = static_cast<int>(vectRedisCliMgr.size());
    vectRedisCliMgr.push_back(client);

    int ret = redisInitConnect();
    if(ret < 0)
    {
        return ret;
    }
    return asyFd;
}

int RedisAsync::addClusterInfo(const std::string& ipPortInfo)
{
    int asyFd = 0;
    std::lock_guard<std::mutex> lock(initMutex_);
    if((callBackNum_ == 0) || (ioThreadNum_ == 0) || (keepSecond_ == 0) || (connOutSecond_ == 0))
    {
        PERROR("threadNum %d callbackNum %d connOutSecond %d keepSecond %d", ioThreadNum_, callBackNum_, connOutSecond_, keepSecond_);
        return INIT_NO_INIT_ERROR;
    }

    REDIS_ASYNC_CLIENT::RedisClientMgrPtr client(new REDIS_ASYNC_CLIENT::RedisClientMgr());
    if(client == nullptr)
    {
        return INIT_SYSTEM_ERROR;
    }

    client->inSvrInfoStr_.assign(ipPortInfo);
    client->svrType_ = REDIS_CLUSTER_SERVER;
    client->mgrFd_ = static_cast<int>(vectRedisCliMgr.size());
    asyFd = static_cast<int>(vectRedisCliMgr.size());
    vectRedisCliMgr.push_back(client);

    int ret = redisInitConnect();
    if(ret < 0)
    {
        return ret;
    }
    return asyFd;
}

int RedisAsync::redisInitConnect()
{
    size_t len = vectRedisCliMgr.size() - 1;
    //这个取最后一个新插入的节点初始化连接。有点奇怪。主要为了解决传参的问题，如果传参倒是好解决。但是为了尽可能少暴露模块内的头文件及定义出去
    REDIS_ASYNC_CLIENT::RedisClientMgrPtr cli = vectRedisCliMgr[len];
    const char *pBegin = cli->inSvrInfoStr_.c_str(), *pComma = NULL, *pColon = NULL;

    do{
        pComma = utilFristConstchar(pBegin, ',');

        pColon = utilFristConstchar(pBegin, ':');
        if(pColon == NULL)
        {
            return INIT_PARAMETER_ERROR;//ip port格式不正确
        }

        len = pColon - pBegin;
        if(len > MAX_IP_ADDR_LEN)
        {
            return INIT_PARAMETER_ERROR;//ip port格式不正确
        }
        std::string ipAddr(pBegin, len);
        pColon++;
        if(pComma)
        {
            len = pComma - pColon;
        }else{
            len = strlen(pColon);
        }
        if(len > MAX_PORT_NUM_LEN)
        {
            return INIT_PARAMETER_ERROR;//ip port格式不正确
        }
        int port = atoi(pColon);

        REDIS_ASYNC_CLIENT::RedisSvrInfoPtr tmpSvrInfo(new REDIS_ASYNC_CLIENT::RedisSvrInfo(port, ipAddr));
        if(tmpSvrInfo == nullptr)
        {
            return INIT_SYSTEM_ERROR;//内存分配失败
        }
        cli->initSvrInfo_.push_back(tmpSvrInfo);
        if(pComma)
        {
            pBegin = pComma + 1;
        }
    }while(pComma);

    size_t tmpIoIndex = initIoIndex.incrementAndGet();
    tmpIoIndex = tmpIoIndex % ioThreadNum_;
    REDIS_ASYNC_CLIENT::RedisClientPtr tmpRedisCli(new REDIS_ASYNC_CLIENT::RedisClient(tmpIoIndex, cli->mgrFd_, cli->initSvrInfo_[cli->initSvrIndex_], keepSecond_, connOutSecond_));
    if(tmpRedisCli == nullptr)
    {
        return INIT_SYSTEM_ERROR;
    }

    REDIS_ASYNC_CLIENT::RedisSvrCliPtr tmpRedisSvrCli(new REDIS_ASYNC_CLIENT::RedisSvrCli(cli->initSvrInfo_[cli->initSvrIndex_], tmpRedisCli));
    if(tmpRedisSvrCli == nullptr)
    {
        return INIT_SYSTEM_ERROR;
    }
    cli->nodeCli_ = tmpRedisSvrCli;

    common::RedisCliOrderNodePtr node(new common::RedisCliOrderNode(tmpRedisCli));
    if(node == nullptr)
    {
        return INIT_SYSTEM_ERROR;
    }
    libeventIoPtrVect[tmpIoIndex]->libeventIoOrder(node, nowSecond_);
    return INIT_SUCCESS_CODE;
}

/*
 * [redisSvrOnConnect] 这一个方法挺复杂的，当有redis服务连接成功或者断开连接，会回调此函数。要区分各种情况。所以这个函数看起来会比较冗余复杂，注意区分各种情况即可
 * @author xiaoxiao 2019-05-14
 * @param
 * @param
 * @param
 *
 * @return
 */
void RedisAsync::redisSvrOnConnect(size_t asyFd, int state, const std::string& ipaddr, int port)
{
    if((vectRedisCliMgr.size() <= asyFd) || (vectRedisCliMgr[asyFd] == nullptr))
    {
        PERROR("vectRedisCliMgr.size() %ld asyFd %ld", vectRedisCliMgr.size(), asyFd);
        return;
    }
    PDEBUG("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
    switch(vectRedisCliMgr[asyFd]->svrType_)
    {
        //vectRedisCliMgr[asyFd]->svrType_ 服务类型
        case REDIS_UNKNOWN_SERVER:
            {
                PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                //TODO
                return;
            }
        //vectRedisCliMgr[asyFd]->svrType_ 服务类型
        case REDIS_SINGLE_SERVER:
            {
                switch(vectRedisCliMgr[asyFd]->initState_)
                {
                    //vectRedisCliMgr[asyFd]->initState_ 服务端状态
                    case REDIS_SVR_INIT_STATE:
                        {
                            switch(state)
                            {
                                //function state 连接状态
                                case CONNECT_REDISVR_SUCCESS:
                                    {
                                        vectRedisCliMgr[asyFd]->initState_ = REDIS_SVR_RUNING_STATE;
                                        callBackPoolPtr->run(std::bind(&CLUSTER_REDIS_ASYNC::RedisAsync::asyncStateCallBack, this, asyFd, REDIS_SVR_RUNING_STATE, vectRedisCliMgr[asyFd]->inSvrInfoStr_));
                                        return;
                                    }
                                //function state 连接状态
                                case CONNECT_REDISVR_RESET:
                                    {
                                        common::RedisCliOrderNodePtr node(new common::RedisCliOrderNode(vectRedisCliMgr[asyFd]->nodeCli_->redisClient_));
                                        if(node == nullptr)
                                        {
                                            char exceMsgBuf[1024] = {0};
                                            sprintf(exceMsgBuf, "%s::%d", ipaddr.c_str(), port);
                                            callBackPoolPtr->run(std::bind(&CLUSTER_REDIS_ASYNC::RedisAsync::asyncExceCallBack, this, asyFd, EXCE_MALLOC_NULL, exceMsgBuf));
                                            //TODO
                                            return;
                                        }
                                        libeventIoPtrVect[vectRedisCliMgr[asyFd]->nodeCli_->redisClient_->redisCliIoIndex()]->libeventIoOrder(node, nowSecond_);
                                        return;
                                    }
                                //function state 连接状态
                                case REDISVR_CONNECT_DISCONN:
                                    {
                                        PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                                        return;
                                    }
                                //function state 连接状态
                                default:
                                    {
                                        PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                                        return;
                                    }
                            }
                            return;
                        }
                    //vectRedisCliMgr[asyFd]->initState_ 服务端状态
                    case REDIS_SVR_RUNING_STATE:
                        {
                            switch(state)
                            {
                                //function state 连接状态
                                case CONNECT_REDISVR_SUCCESS:
                                    {
                                        PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                                        return;
                                    }
                                //function state 连接状态
                                case CONNECT_REDISVR_RESET:
                                    {
                                        PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                                        return;
                                    }
                                //function state 连接状态
                                case REDISVR_CONNECT_DISCONN:
                                    {
                                        vectRedisCliMgr[asyFd]->initState_ = REDIS_SVR_ERROR_STATE;
                                        callBackPoolPtr->run(std::bind(&CLUSTER_REDIS_ASYNC::RedisAsync::asyncStateCallBack, this, asyFd, REDIS_SVR_ERROR_STATE, vectRedisCliMgr[asyFd]->inSvrInfoStr_));
                                        common::RedisCliOrderNodePtr node(new common::RedisCliOrderNode(vectRedisCliMgr[asyFd]->nodeCli_->redisClient_));
                                        if(node == nullptr)
                                        {
                                            char exceMsgBuf[1024] = {0};
                                            sprintf(exceMsgBuf, "%s::%d", ipaddr.c_str(), port);
                                            callBackPoolPtr->run(std::bind(&CLUSTER_REDIS_ASYNC::RedisAsync::asyncExceCallBack, this, asyFd, EXCE_MALLOC_NULL, exceMsgBuf));
                                            //TODO
                                            return;
                                        }
                                        libeventIoPtrVect[vectRedisCliMgr[asyFd]->nodeCli_->redisClient_->redisCliIoIndex()]->libeventIoOrder(node, nowSecond_);
                                        return;
                                    }
                                //function state 连接状态
                                default:
                                    {
                                        PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                                        return;
                                    }
                            }
                            return;
                        }
                    case REDIS_EXCEPTION_STATE:
                        {
                            PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                            return;
                        }
                    //vectRedisCliMgr[asyFd]->initState_ 服务端状态
                    case REDIS_SVR_ERROR_STATE:
                        {
                            switch(state)
                            {
                                //function state 连接状态
                                case CONNECT_REDISVR_SUCCESS:
                                    {
                                        vectRedisCliMgr[asyFd]->initState_ = REDIS_SVR_RUNING_STATE;
                                        callBackPoolPtr->run(std::bind(&CLUSTER_REDIS_ASYNC::RedisAsync::asyncStateCallBack, this, asyFd, REDIS_SVR_RUNING_STATE, vectRedisCliMgr[asyFd]->inSvrInfoStr_));
                                        return;
                                    }
                                //function state 连接状态
                                case CONNECT_REDISVR_RESET:
                                    {
                                        //TODO 重连时间要拉长
                                        common::RedisCliOrderNodePtr node(new common::RedisCliOrderNode(vectRedisCliMgr[asyFd]->nodeCli_->redisClient_));
                                        if(node == nullptr)
                                        {
                                            char exceMsgBuf[1024] = {0};
                                            sprintf(exceMsgBuf, "%s::%d", ipaddr.c_str(), port);
                                            callBackPoolPtr->run(std::bind(&CLUSTER_REDIS_ASYNC::RedisAsync::asyncExceCallBack, this, asyFd, EXCE_MALLOC_NULL, exceMsgBuf));
                                            //TODO
                                            return;
                                        }
                                        libeventIoPtrVect[vectRedisCliMgr[asyFd]->nodeCli_->redisClient_->redisCliIoIndex()]->libeventIoOrder(node, nowSecond_);
                                        return;
                                    }
                                //function state 连接状态
                                case REDISVR_CONNECT_DISCONN:
                                    {
                                        PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                                        return;
                                    }
                                //function state 连接状态
                                default:
                                    {
                                        PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                                        return;
                                    }
                            }
                            return;
                        }
                    //vectRedisCliMgr[asyFd]->initState_ 服务端状态
                    default:
                        {
                            PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                            return;
                        }
                }
                return;
            }
        //vectRedisCliMgr[asyFd]->svrType_ 服务类型
        case REDIS_MASTER_SLAVE_SERVER:
            {
                switch(vectRedisCliMgr[asyFd]->initState_)
                {
                    //vectRedisCliMgr[asyFd]->initState_ 服务端状态
                    case REDIS_SVR_INIT_STATE:
                        {
                            switch(state)
                            {
                                //function state 连接状态
                                case CONNECT_REDISVR_SUCCESS:
                                    {
                                        PDEBUG("INFO replication");
                                        std::string clusterCmd = "INFO replication";
                                        common::OrderNodePtr cmdnode(new common::OrderNode(clusterCmd, std::bind(&CLUSTER_REDIS_ASYNC::RedisAsync::masterSalveInitCb, this, std::placeholders::_1,std::placeholders::_2, std::placeholders::_3)));
                                        if(cmdnode == nullptr)
                                        {
                                            //TODO
                                            return;
                                        }
                                        cmdnode->outSecond_ = DEFAULT_CMD_OUTSECOND;
                                        cmdnode->cmdOutSecond_ = nowSecond_ + DEFAULT_CMD_OUTSECOND;
                                        common::RedisCliOrderNodePtr node(new common::RedisCliOrderNode(vectRedisCliMgr[asyFd]->nodeCli_->redisClient_, cmdnode, cmdnode->cmdOutSecond_));
                                        if(node == nullptr)
                                        {
                                            //TODO
                                            return;
                                        }
                                        libeventIoPtrVect[vectRedisCliMgr[asyFd]->nodeCli_->redisClient_->redisCliIoIndex()]->libeventIoOrder(node, nowSecond_);
                                        return;
                                    }
                                //function state 连接状态
                                case CONNECT_REDISVR_RESET:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                case REDISVR_CONNECT_DISCONN:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                default:
                                    {
                                        PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                                        return;
                                    }
                            }
                            return;
                        }
                    //vectRedisCliMgr[asyFd]->initState_ 服务端状态
                    case REDIS_SVR_RUNING_STATE:
                        {
                            switch(state)
                            {
                                //function state 连接状态
                                case CONNECT_REDISVR_SUCCESS:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                case CONNECT_REDISVR_RESET:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                case REDISVR_CONNECT_DISCONN:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                default:
                                    {
                                        PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                                        return;
                                    }
                            }
                            return;
                        }
                    //vectRedisCliMgr[asyFd]->initState_ 服务端状态
                    case REDIS_EXCEPTION_STATE:
                        {
                            switch(state)
                            {
                                //function state 连接状态
                                case CONNECT_REDISVR_SUCCESS:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                case CONNECT_REDISVR_RESET:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                case REDISVR_CONNECT_DISCONN:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                default:
                                    {
                                        PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                                        return;
                                    }
                            }
                            return;
                        }
                    //vectRedisCliMgr[asyFd]->initState_ 服务端状态
                    case REDIS_SVR_ERROR_STATE:
                        {
                            switch(state)
                            {
                                //function state 连接状态
                                case CONNECT_REDISVR_SUCCESS:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                case CONNECT_REDISVR_RESET:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                case REDISVR_CONNECT_DISCONN:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                default:
                                    {
                                        PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                                        return;
                                    }
                            }
                            return;
                        }
                    //vectRedisCliMgr[asyFd]->initState_ 服务端状态
                    default:
                        {
                            PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                            return;
                        }
                }
                return;
            }
        //vectRedisCliMgr[asyFd]->svrType_ 服务类型
        case REDIS_CLUSTER_SERVER:
            {
                switch(vectRedisCliMgr[asyFd]->initState_)
                {
                    //vectRedisCliMgr[asyFd]->initState_ 服务端状态
                    case REDIS_SVR_INIT_STATE:
                        {
                            switch(state)
                            {
                                //function state 连接状态
                                case CONNECT_REDISVR_SUCCESS:
                                    {
                                        if((vectRedisCliMgr[asyFd]->cluterCli_ == nullptr) || (vectRedisCliMgr[asyFd]->cluterCli_->clusterVectInfo_.size() == 0))
                                        {
                                            std::string clusterCmd = "CLUSTER NODES";
                                            common::OrderNodePtr cmdnode(new common::OrderNode(clusterCmd, std::bind(&CLUSTER_REDIS_ASYNC::RedisAsync::clusterInitCallBack, this, std::placeholders::_1,std::placeholders::_2, std::placeholders::_3), vectRedisCliMgr[asyFd].get()));
                                            if(cmdnode == nullptr)
                                            {
                                                callBackPoolPtr->run(std::bind(&CLUSTER_REDIS_ASYNC::RedisAsync::asyncExceCallBack, this, asyFd, EXCE_MALLOC_NULL, vectRedisCliMgr[asyFd]->inSvrInfoStr_));
                                                callBackPoolPtr->run(std::bind(&CLUSTER_REDIS_ASYNC::RedisAsync::redisSvrMgrInvalid, this, asyFd));
                                                return;
                                            }
                                            cmdnode->outSecond_ = DEFAULT_CMD_OUTSECOND;
                                            cmdnode->cmdOutSecond_ = nowSecond_ + DEFAULT_CMD_OUTSECOND;
                                            common::RedisCliOrderNodePtr node(new common::RedisCliOrderNode(vectRedisCliMgr[asyFd]->nodeCli_->redisClient_, cmdnode, cmdnode->cmdOutSecond_));
                                            if(node == nullptr)
                                            {
                                                callBackPoolPtr->run(std::bind(&CLUSTER_REDIS_ASYNC::RedisAsync::asyncExceCallBack, this, asyFd, EXCE_MALLOC_NULL, vectRedisCliMgr[asyFd]->inSvrInfoStr_));
                                                callBackPoolPtr->run(std::bind(&CLUSTER_REDIS_ASYNC::RedisAsync::redisSvrMgrInvalid, this, asyFd));
                                                return;
                                            }
                                            libeventIoPtrVect[vectRedisCliMgr[asyFd]->nodeCli_->redisClient_->redisCliIoIndex()]->libeventIoOrder(node, nowSecond_);
                                        }else{
                                            size_t tmpConns = vectRedisCliMgr[asyFd]->masterConn_;
                                            for(size_t i = 0; i < vectRedisCliMgr[asyFd]->cluterCli_->clusterVectInfo_.size(); i++)
                                            {
                                                if((vectRedisCliMgr[asyFd]->cluterCli_->clusterVectInfo_[i]->clusterNode_->master_->svrInfo_->port_ == port)
                                                        && (vectRedisCliMgr[asyFd]->cluterCli_->clusterVectInfo_[i]->clusterNode_->master_->svrInfo_->ipAddr_.compare(ipaddr) == 0))
                                                {
                                                    vectRedisCliMgr[asyFd]->masterConn_++;
                                                    break;
                                                }
                                            }
                                            if(tmpConns == vectRedisCliMgr[asyFd]->masterConn_)
                                            {
                                                vectRedisCliMgr[asyFd]->slaveConn_++;
                                            }
                                            if(vectRedisCliMgr[asyFd]->cluterCli_->clusterVectInfo_.size() == vectRedisCliMgr[asyFd]->masterConn_)
                                            {
                                                vectRedisCliMgr[asyFd]->initState_ = REDIS_SVR_RUNING_STATE;
                                                callBackPoolPtr->run(std::bind(&CLUSTER_REDIS_ASYNC::RedisAsync::asyncStateCallBack, this, asyFd, REDIS_SVR_RUNING_STATE, vectRedisCliMgr[asyFd]->inSvrInfoStr_));
                                            }
                                        }
                                        return;
                                    }
                                //function state 连接状态
                                case CONNECT_REDISVR_RESET:
                                    {
                                        //TODO 重连时间要拉长
                                        common::RedisCliOrderNodePtr node(new common::RedisCliOrderNode(vectRedisCliMgr[asyFd]->nodeCli_->redisClient_));
                                        if(node == nullptr)
                                        {
                                            char exceMsgBuf[1024] = {0};
                                            sprintf(exceMsgBuf, "%s::%d", ipaddr.c_str(), port);
                                            callBackPoolPtr->run(std::bind(&CLUSTER_REDIS_ASYNC::RedisAsync::asyncExceCallBack, this, asyFd, EXCE_MALLOC_NULL, exceMsgBuf));
                                            //TODO
                                            return;
                                        }
                                        libeventIoPtrVect[vectRedisCliMgr[asyFd]->nodeCli_->redisClient_->redisCliIoIndex()]->libeventIoOrder(node, nowSecond_);
                                        return;
                                    }
                                //function state 连接状态
                                case REDISVR_CONNECT_DISCONN:
                                    {
                                        PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                                        return;
                                    }
                                //function state 连接状态
                                default:
                                    {
                                        PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                                        return;
                                    }
                            }
                            return;
                        }
                    //vectRedisCliMgr[asyFd]->initState_ 服务端状态
                    case REDIS_SVR_RUNING_STATE:
                        {
                            switch(state)
                            {
                                //function state 连接状态
                                case CONNECT_REDISVR_SUCCESS:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                case CONNECT_REDISVR_RESET:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                case REDISVR_CONNECT_DISCONN:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                default:
                                    {
                                        PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                                        return;
                                    }
                            }
                            return;
                        }
                    //vectRedisCliMgr[asyFd]->initState_ 服务端状态
                    case REDIS_EXCEPTION_STATE:
                        {
                            switch(state)
                            {
                                //function state 连接状态
                                case CONNECT_REDISVR_SUCCESS:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                case CONNECT_REDISVR_RESET:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                case REDISVR_CONNECT_DISCONN:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                default:
                                    {
                                        PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                                        return;
                                    }
                            }
                            return;
                        }
                    //vectRedisCliMgr[asyFd]->initState_ 服务端状态
                    case REDIS_SVR_ERROR_STATE:
                        {
                            switch(state)
                            {
                                //function state 连接状态
                                case CONNECT_REDISVR_SUCCESS:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                case CONNECT_REDISVR_RESET:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                case REDISVR_CONNECT_DISCONN:
                                    {
                                        return;
                                    }
                                //function state 连接状态
                                default:
                                    {
                                        PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                                        return;
                                    }
                            }
                            return;
                        }
                    //vectRedisCliMgr[asyFd]->initState_ 服务端状态
                    default:
                        {
                            PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                            return;
                        }
                }
                break;
            }
        //vectRedisCliMgr[asyFd]->svrType_ 服务类型
        default:
            {
                PERROR("asyFd %ld svrType_ %d inSvrInfoStr_ %s state %d svr info %s::%d", asyFd, vectRedisCliMgr[asyFd]->svrType_, vectRedisCliMgr[asyFd]->inSvrInfoStr_.c_str(), state, ipaddr.c_str(), port);
                return;
            }
    }
    return;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int RedisAsync::redisAsyncCommand(int asyFd, const CmdResultCallback& cb, int outSecond, void *priv, const std::string& key, const std::string& cmdStr)
{
    
    REDIS_ASYNC_CLIENT::RedisClientPtr redisClient(nullptr);
    if(asyFd < 0 || key.empty())
    {
        return CMD_PARAMETER_ERROR_CODE;
    }

    size_t mgrFd_ = static_cast<size_t>(asyFd);
    if(mgrFd_ >= vectRedisCliMgr.size())
    {
        return CMD_PARAMETER_ERROR_CODE;
    }

    if(vectRedisCliMgr[mgrFd_] == nullptr)
    {
        return CMD_SVR_INVALID_CODE;
    }

    if((vectRedisCliMgr[mgrFd_]->initState_ == REDIS_SVR_INIT_STATE) || (vectRedisCliMgr[mgrFd_]->initState_ == REDIS_SVR_ERROR_STATE))
    {
        return CMD_SVR_NO_INIT_CODE;
    }

    if(vectRedisCliMgr[mgrFd_]->svrType_ == REDIS_ASYNC_CLUSTER_RUNING_STATE)
    {
        uint16_t slot = getKeySlotIndex(key);
        REDIS_ASYNC_CLIENT::SlotCliInfoMap::iterator iter = vectRedisCliMgr[mgrFd_]->cluterCli_->clusterSlotMap_.find(slot);
        if(iter == vectRedisCliMgr[mgrFd_]->cluterCli_->clusterSlotMap_.end())
        {
            return CMD_SLOTS_CALCUL_ERROR_CODE;
        }
        redisClient = iter->second;
    }else{
        redisClient = vectRedisCliMgr[mgrFd_]->nodeCli_->redisClient_;
    }

    common::OrderNodePtr cmdOrder(new common::OrderNode(cmdStr, cb, priv));
    if(cmdOrder == nullptr)
    {
        return CMD_SYSTEM_MALLOC_CODE;
    }
    cmdOrder->outSecond_ = outSecond;
    cmdOrder->cmdOutSecond_ = nowSecond_ + outSecond;
    common::RedisCliOrderNodePtr node(new common::RedisCliOrderNode(redisClient, cmdOrder, cmdOrder->cmdOutSecond_));
    if(node == nullptr)
    {
        return CMD_SYSTEM_MALLOC_CODE;
    }
    libeventIoPtrVect[redisClient->redisCliIoIndex()]->libeventIoOrder(node, nowSecond_);
    return 0;
}

}

