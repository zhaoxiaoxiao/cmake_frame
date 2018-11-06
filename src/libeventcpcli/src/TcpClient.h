#ifndef __LIBEVENT_TCPCLI_CLIENT_H__
#define __LIBEVENT_TCPCLI_CLIENT_H__

#include <event.h>
//#include <event2/event.h>
#include <event2/bufferevent.h>  
#include <event2/buffer.h>  
//#include <event2/util.h> 

//#include "TcpClient.h"
namespace LIBEVENT_TCP_CLI
{

class TcpClient
{
public:
	//io�߳��±꣬Ψһ���ֱ�ʶ��ip��ַ���˿ڣ�˽����Ϣ����ʱʱ�䣬
	TcpClient(size_t ioIndex, uint64_t uniqueNum, const std::string& ipaddr, int port, void* priv, int outSecond = 30);

	~TcpClient();

	int connectServer(struct event_base* eBase);

	void disConnect();

	void sendMsg(const void* msg, size_t len);

	void* tpcClientPrivate()
	{
		return priv_;
	}

	int isKeepAlive();

	uint64_t tcpCliUniqueNum()
	{
		return uniqueNum_;
	
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	size_t inIoThreadIndex()
	{
		return ioIndex_;
	}

	void setRecvSecond();

	const char* tcpServerIp() const
	{
		return ipaddr_.c_str();
	}

	int tcpServerPort() const
	{
		return port_;
	}

	void setReadySendbufLen(size_t buflen)
	{
		outbufLen_ = buflen;
	}

	int tcpCliSockFd()
	{
		return sockfd_;
	
	
private:
	int port_;
	int sockfd_;
	int outSecond_;
	
	size_t ioIndex_;
	size_t outbufLen_;

	uint64_t uniqueNum_;
	uint64_t lastRecvSecond_;
	void* priv_;
	struct event_base *base_;
	struct bufferevent* bev_;

	std::string ipaddr_;
};

typedef std::shared_ptr<TcpClient> TcpClientPtr;
}
#endif