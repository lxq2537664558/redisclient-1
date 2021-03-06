#include "RedisPool.h"


#define PREPARE_REDIS_CONTEXT_EXCEPTION(message)  std::string(__FUNCTION__)+ \
									"|"+ (redisContext_->errstr == NULL ? "" : redisContext_->errstr)+ \
									"|" + std::string(message)

#define PREPARE_REDIS_EXCEPTION(message)  std::string(__FUNCTION__)+ \
									"|"+ (redisContext_->errstr == NULL ? "" : redisContext_->errstr)+ \
									"|" +(reply->str == NULL ? "" : reply->str)+ \
									"|" + std::string(message)
									

RedisConnection::RedisConnection(RedisPool* redisPool)
	: redisContext_(NULL),
	  lastActiveTime_(time(NULL)),
	  redisPool_(redisPool)
{

}
RedisConnection::~RedisConnection()
{
	if (redisContext_) 
	{
		redisFree(redisContext_);
		redisContext_ = NULL;
	}
}

int RedisConnection::connect()
{
	struct timeval timeout = {0, 1000000}; // 1s
	redisContext_ = redisConnectWithTimeout(redisPool_->getServerIP(), redisPool_->getServerPort(), timeout);
	if (!redisContext_ || redisContext_->err) 
	{
		RedisException e(PREPARE_REDIS_CONTEXT_EXCEPTION("connect failed!"));
		if (redisContext_) 
		{
			redisFree(redisContext_);
			redisContext_ = NULL;
		} 
		throw e;
	}

	redisReply* reply = static_cast<redisReply*>(redisCommand(redisContext_, "SELECT %d", redisPool_->getDBNo()));
	if (!checkReply(reply)) 
	{
		RedisException e(PREPARE_REDIS_EXCEPTION());
		if (reply)
			freeReplyObject(reply);
	    throw e;
	}
	
	return 0;
}

bool RedisConnection::checkReply(const redisReply* reply)
{
	if(reply == NULL) 
		return false;

	switch(reply->type)
	{
	case REDIS_REPLY_STRING:
		return true;
	case REDIS_REPLY_ARRAY:
		return true;
	case REDIS_REPLY_INTEGER:
		return true;
	case REDIS_REPLY_NIL:
		return false;
	case REDIS_REPLY_STATUS:
		return (strcasecmp(reply->str, "OK") == 0) ? true : false;
	case REDIS_REPLY_ERROR:
		return false;
	default:
		return false;
	}

	return false;
}

bool RedisConnection::ping() 
{
	redisReply* reply = static_cast<redisReply*>(redisCommand(redisContext_, "PING"));
	if (reply == NULL)
		return false;
		
	freeReplyObject(reply);
	return true;
}

bool RedisConnection::exists(std::string key)
{
	int result;
	redisReply* reply = static_cast<redisReply*>(redisCommand(redisContext_, "EXISTS %s", key.c_str()));
	if (!checkReply(reply)) 
	{
		RedisException e(PREPARE_REDIS_EXCEPTION());
		if (reply)
			freeReplyObject(reply);
	    throw e;
	}

	result = reply->integer;
	freeReplyObject(reply);
	return result == 1;
}

bool RedisConnection::set(std::string key, std::string &value)
{
    redisReply* reply = static_cast<redisReply*>(redisCommand(redisContext_, "SET %s %s", key.c_str(), value.c_str()));
    if (!checkReply(reply)) 
    {
    	RedisException e(PREPARE_REDIS_EXCEPTION());
		if (reply)
			freeReplyObject(reply);
		throw e;
    }

	freeReplyObject(reply);
    return true;
}

std::string RedisConnection::get(std::string key)
{
	redisReply* reply = static_cast<redisReply*>(redisCommand(redisContext_, "GET %s", key.c_str()));
	if (!checkReply(reply)) 
	{
		RedisException e(PREPARE_REDIS_EXCEPTION());
		if (reply)
			freeReplyObject(reply);
		throw e;
	}

	std::string result;
	if (reply->type == REDIS_REPLY_STRING) 
		result.append(reply->str, reply->len);
	
	freeReplyObject(reply);
	return result;
}

int RedisConnection::hset(std::string key, std::string field, std::string value)
{
	redisReply* reply = static_cast<redisReply*>(redisCommand(redisContext_, \
						"HSET %s %s %s", key.c_str(), field.c_str(), value.c_str()));
	if (!checkReply(reply)) 
	{
		RedisException e(PREPARE_REDIS_EXCEPTION());
		if (reply)
			freeReplyObject(reply);
		throw e;
	}

	freeReplyObject(reply);
	return reply->integer;
}

std::string RedisConnection::hget(std::string key, std::string field)
{
	std::string result;
	redisReply* reply = static_cast<redisReply*>(redisCommand(redisContext_, \
						"HGET %s %s", key.c_str(), field.c_str()));
	if (!checkReply(reply)) 
	{
		RedisException e(PREPARE_REDIS_EXCEPTION());
		if (reply)
			freeReplyObject(reply);
	    throw e;
	}

	result.append(reply->str, reply->len);
	freeReplyObject(reply);
	return result;
}

bool RedisConnection::hgetall(std::string key, std::map<std::string, std::string>& result)
{
	redisReply* reply = static_cast<redisReply*>(redisCommand(redisContext_, \
						"HGETALL %s", key.c_str()));
	if (!checkReply(reply)) 
	{
		RedisException e(PREPARE_REDIS_EXCEPTION());
		if (reply)
			freeReplyObject(reply);
	    throw e;
	}

	if ( (reply->type == REDIS_REPLY_ARRAY) && (reply->elements % 2 == 0) ) 
	{
		for (size_t i = 0; i < reply->elements; i += 2) 
		{
			std::string field(reply->element[i]->str, reply->element[i]->len);
			std::string value(reply->element[i+1]->str, reply->element[i+1]->len);
			result.insert(make_pair(field, value));
		}
	}

	freeReplyObject(reply);
	return true;
}

RedisPool::RedisPool(const std::string ip, 
					uint16_t port, 
					int minConn,
					int maxConn,
					int dbNo,
					const std::string nameArg)
	: hostip_(ip),
	  hostport_(port),
	  minConn_(minConn),
	  maxConn_(maxConn),
	  dbNo_(dbNo),
	  name_(nameArg),
	  mutex_(),
	  notEmpty_(mutex_),
	  connections_(),
	  quit_(false)
{
	
}
RedisPool::~RedisPool()
{
	MutexLockGuard lock(mutex_);

	quit_ = true;
	cronThread->join();
	
	for (std::list<RedisConnection*>::iterator it = connections_.begin(); 
			it != connections_.end(); it++) 
	{
		delete *it;
	}

	connections_.clear();
	minConn_= 0;
}

int RedisPool::init()
{
	for (int i = 0; i < minConn_; i++) 
	{
		RedisConnection* conn = new RedisConnection(this);
		try
		{
			conn->connect(); 
		}
		catch(RedisException& ex)
		{
			if (conn) delete conn;
		}
		
		if (conn != NULL)
			connections_.push_back(conn);
	}

	cronThread = new std::thread(std::bind(&RedisPool::serverCron, this));

	return 0;
}

// move out the disabled connections
void RedisPool::serverCron()
{
	while (!quit_)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10000));
		MutexLockGuard lock(mutex_);
		
		std::list<RedisConnection*>::iterator it = connections_.begin();
		for (; it != connections_.end(); ) 
		{
			if ((*it)->ping() == false)
			{
				delete *it;
				connections_.remove(*it++);
			}
			else
			{
				it++;
			}
		}
	}
}

RedisConnection* RedisPool::getConnection()
{
	MutexLockGuard lock(mutex_);

	while (connections_.empty()) 
	{
		if (minConn_ >= maxConn_) 
		{
			notEmpty_.wait();
		} 
		else 
		{
			RedisConnection* conn = new RedisConnection(this);
			try
			{
				conn->connect();
			}
			catch(RedisException& ex)
			{
				if (conn) delete conn;
				throw;
			}
			connections_.push_back(conn);
			minConn_++;
		}
	}

	RedisConnection* pConn = connections_.front();
	connections_.pop_front();

	return pConn;
}

void RedisPool::freeConnection(RedisConnection* conn)
{
	MutexLockGuard lock(mutex_);

	std::list<RedisConnection*>::iterator it = connections_.begin();
	for (; it != connections_.end(); it++) 
	{
		if (*it == conn) 
			break;
	}

	if (it == connections_.end()) 
	{
		connections_.push_back(conn);
	}

	notEmpty_.notify();
}


