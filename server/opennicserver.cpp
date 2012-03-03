/*
 * Copyright (c) 2012 Mike Sharkey <michael_sharkey@firstclass.com>
 *
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Mike Sharkey wrote this file. As long as you retain this notice you
 * can do whatever you want with this stuff. If we meet some day, and you think
 * this stuff is worth it, you can buy me a beer in return.
 */
#include "opennicserver.h"
#include "opennicsystem.h"
#include "opennicresolverpoolitem.h"

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QEventLoop>
#include <QByteArray>
#include <QSettings>
#include <QMultiMap>
#include <QMap>
#include <QHostAddress>
#include <QTcpSocket>

#define DEFAULT_FAST_TIMER						10			/* seconds */
#define DEFAULT_REFRESH_TIMER_PERIOD			1			/* minutes */
#define DEFAULT_RESOLVER_CACHE_SIZE				3
#define DEFAULT_BOOTSTRAP_CACHE_SIZE			3
#define DEFAULT_CLIENT_TIMEOUT					3			/* seconds */
#define DEFAULT_TCP_LISTEN_PORT				    19803		/* localhost port for communication with GUI */
#define MAX_LOG_LINES							100			/* max lines to keep in log cache */

#define inherited QObject

OpenNICServer::OpenNICServer(QObject *parent)
: inherited(parent)
, mRefreshTimerPeriod(0)
, mResolverCacheSize(0)
, mEnabled(true)
, mRefreshTimer(-1)
, mFastTimer(-1)
, mResolversInitialized(false)
, mTcpListenPort(DEFAULT_TCP_LISTEN_PORT)
, mUpdatingDNS(false)
{
	readSettings();
	initializeServer();
	mFastTimer = startTimer(1000*DEFAULT_FAST_TIMER);
}

OpenNICServer::~OpenNICServer()
{
}

int OpenNICServer::refreshPeriod()
{
	return mRefreshTimerPeriod;
}

/**
  * @brief Set refresh period ni minutes
  */
void OpenNICServer::setRefreshPeriod(int period)
{
	if ( mRefreshTimerPeriod != period && period >= 0 )
	{
		mRefreshTimerPeriod = period;
		if ( mRefreshTimer >= 0 )
		{
			killTimer(mRefreshTimer);
			mRefreshTimer=-1;
		}
		log(tr("** DNS REFRESH IN ")+QString::number(mRefreshTimerPeriod)+tr(" MINUTES **"));
		mRefreshTimer = startTimer((mRefreshTimerPeriod*60)*1000);
	}
}

int OpenNICServer::resolverCacheSize()
{
	return mResolverCacheSize;
}

void OpenNICServer::setResolverCacheSize(int size)
{
	if ( mResolverCacheSize != size && size >= 0 )
	{
		mResolverCacheSize = size;
		log(tr("** ACTIVE CACHE SET TO ")+QString::number(mResolverCacheSize)+tr(" RESOLVERS **"));
		updateDNS(mResolverCacheSize);
	}
}

/**
  * @brief log a message
  */
void OpenNICServer::log(QString msg)
{
	QString str = QDateTime::currentDateTime().toString("yyMMddhhmmss")+"|"+msg;
	mLog << str;
	fprintf(stderr,"%s\n",str.toAscii().data());
	while(mLog.count() > MAX_LOG_LINES)
	{
		mLog.takeAt(0);
	}
}

/**
  * @brief prune the log 'file'
  */
void OpenNICServer::pruneLog()
{
	while (mLog.count() > MAX_LOG_LINES)
	{
		mLog.takeFirst();
	}
}

/**
  * @brief purge the log
  */
void OpenNICServer::logPurge()
{
	mLog.clear();
}

/**
  * @brief Fetch the settings.
  */
void OpenNICServer::readSettings()
{
	QSettings settings("OpenNIC", "OpenNICService");
	mTcpListenPort			= settings.value("tcp_listen_port",			DEFAULT_TCP_LISTEN_PORT).toInt();
	setRefreshPeriod(settings.value("refresh_timer_period",	DEFAULT_REFRESH_TIMER_PERIOD).toInt());
	setResolverCacheSize(settings.value("resolver_cache_size",DEFAULT_RESOLVER_CACHE_SIZE).toInt());
}

/**
  * @brief Store the settings.
  */
void OpenNICServer::writeSettings()
{
	QSettings settings("OpenNIC", "OpenNICService");
	settings.setValue("tcp_listen_port",			mTcpListenPort);
	settings.setValue("refresh_timer_period",		refreshPeriod());
	settings.setValue("resolver_cache_size",		resolverCacheSize());
}

/**
  * @brief Get here when a task tray applet has connected.
  */
void OpenNICServer::newConnection()
{
	QTcpSocket* client;
	while ( (client = mServer.nextPendingConnection()) != NULL )
	{
		QObject::connect(client,SIGNAL(readyRead()),this,SLOT(readyRead()));
		mSessions.append(client);
		log(tr("** client session created **"));
	}
}

/**
  * @brief get here on data available from client
  */
void OpenNICServer::readyRead()
{
	mAsyncMessage.clear();
	for(int n=0; n < mSessions.count(); n++)
	{
		QTcpSocket* session = mSessions[n];
		if ( session->isOpen() && session->isValid() )
		{
			QMap<QString,QVariant> clientPacket;
			QDataStream stream(session);
			log("got "+QString::number(stream.device()->bytesAvailable())+" bytes from client");
			stream >> clientPacket;
			QMapIterator<QString, QVariant>i(clientPacket);
			while (i.hasNext())
			{
				i.next();
				QString key = i.key();
				QVariant value = i.value();
				if ( key == "resolver_cache_size" )
				{
					if (resolverCacheSize() != value.toInt())
					{
						mAsyncMessage = tr("Settings Applied");
					}
					setResolverCacheSize(value.toInt());
				}
				else if ( key == "refresh_timer_period" )
				{
					if (refreshPeriod() != value.toInt())
					{
						mAsyncMessage = tr("Settings Applied");
					}
					setRefreshPeriod(value.toInt());
				}
				else if ( key == "bootstrap_t1_list" )
				{
					if ( OpenNICSystem::saveBootstrapT1List(value.toStringList()) )
					{
						mAsyncMessage = tr("Bootstrap T1 List Saved");
					}
					else
					{
						mAsyncMessage = tr("There was a problem saving the T1 bootstrap list");
					}
				}
				else if ( key == "bootstrap_domains" )
				{
					if ( OpenNICSystem::saveTestDomains(value.toStringList()) )
					{
						mAsyncMessage = tr("Domain List Saved");
					}
					else
					{
						mAsyncMessage = tr("There was a problem saving the domains list");
					}
				}
				else if ( key == "update_dns" )
				{
					updateDNS(resolverCacheSize());
				}
			}

		}
	}
	writeSettings();
	if (!mAsyncMessage.isEmpty())
	{
		announcePackets();
	}
}

/**
  * @brief Set up the local server for the task tray app to attach to.
  * @return the number of resolvers
  */
int OpenNICServer::initializeServer()
{
	if (!mServer.isListening() )
	{
		QHostAddress localhost(QHostAddress::LocalHost);
		mServer.setMaxPendingConnections(10);
		if ( mServer.listen(localhost,/* mTcpListenPort */ DEFAULT_TCP_LISTEN_PORT) )
		{
			QObject::connect(&mServer,SIGNAL(newConnection()),this,SLOT(newConnection()));
			log(tr("listening on port ")+QString::number(mTcpListenPort));
		}
		else
		{
			fprintf(stderr,"%s\n",mServer.errorString().trimmed().toAscii().data());
		}
	}
	return 0;
}

QString OpenNICServer::copyright()
{
	return "OpenNICServer V"+QString(VERSION_STRING)+ tr( " (c) 2012 Mike Sharkey <michael_sharkey@firstclass.com>" );
}

QString OpenNICServer::license()
{
	return QString( copyright() +
						tr( "\"THE BEER-WARE LICENSE\" (Revision 42):\n"
						"Mike Sharkey wrote this thing. As long as you retain this notice you\n"
						"can do whatever you want with this stuff. If we meet some day, and you think\n"
						"this stuff is worth it, you can buy me a beer in return.\n" )
					   );
}

/**
  * @brief purge dead (closed) sessions.
  */
void OpenNICServer::purgeDeadSesssions()
{
	/* get here regularly, purge dead sessions... */
	for(int n=0; n < mSessions.count(); n++)
	{
		QTcpSocket* session = mSessions.at(n);
		if ( !session->isOpen() || !session->isValid() )
		{
			log("** CLIENT SESSION DISPOSED **");
			mSessions.takeAt(n);
			delete session;
		}
	}
}

/**
  * @brief Make a server packet
  * @return a map of key/value pairs
  */
QByteArray& OpenNICServer::makeServerPacket(QByteArray& bytes)
{

	QDataStream stream(&bytes,QIODevice::ReadWrite);
	QMap<QString,QVariant> packet;
	packet.insert("tcp_listen_port",			mTcpListenPort);
	packet.insert("refresh_timer_period",		refreshPeriod());
	packet.insert("resolver_cache_size",		resolverCacheSize());
	packet.insert("resolver_pool",				mResolverPool.toStringList());
	packet.insert("resolver_cache",				mResolverCache.toStringList());
	packet.insert("bootstrap_t1_list",			OpenNICSystem::getBootstrapT1List());
	packet.insert("bootstrap_domains",			OpenNICSystem::getTestDomains().toStringList());
	packet.insert("system_text",				OpenNICSystem::getSystemResolverList());
	packet.insert("journal_text",				mLog);
	packet.insert("async_message",				mAsyncMessage);
	stream << packet;
	return bytes;
}

/**
  * @brief announce packets to sessions
  */
void OpenNICServer::announcePackets()
{
	if ( mSessions.count() )
	{
		QByteArray packet;
		makeServerPacket(packet);
		for(int n=0; n < mSessions.count(); n++)
		{
			QTcpSocket* session = mSessions[n];
			if ( session->isOpen() && session->isValid() )
			{
				QDataStream stream(session);
				stream << packet;
				session->flush();
				//log("sent "+QString::number(packet.length())+" bytes.");
			}
		}
		logPurge();
		mAsyncMessage.clear();
	}
}

/**
  * @brief get here to initiate cold bootstrap
  */
void OpenNICServer::coldBoot()
{
	log("** COLD BOOT **");
	log(copyright());
	log(license());
	readSettings();
	bootstrapResolvers();
	if ( mResolversInitialized )
	{
		initializeServer();
	}
}

/**
  * @brief Perform the update function. Fetch DNS candidates, test for which to apply, and apply them.
  * @return the number of resolvers
  */
int OpenNICServer::bootstrapResolvers()
{
	int n, rc;
	mResolversInitialized=false;
	/** get the bootstrap resolvers... */
	mResolverPool.clear();
	QStringList bootstrapList = OpenNICSystem::getBootstrapT1List();
	log(tr("Found ")+QString::number(bootstrapList.count())+tr(" T1 resolvers"));
	for(n=0; n < bootstrapList.count(); n++)
	{
		QString resolver = bootstrapList.at(n).trimmed();
		if ( !resolver.isEmpty() )
		{
			OpenNICResolverPoolItem item(QHostAddress(resolver),"T1");
			mResolverPool.insort(item);
			log(item.toString());
		}
	}
	/** apply the bootstrap resolvers */
	log(tr("Randomizing T1 list..."));
	mResolverPool.randomize();
	int nBootstrapResolvers = resolverCacheSize() <= mResolverPool.count() ? resolverCacheSize() : mResolverPool.count();
	log(tr("Applying ")+QString::number(nBootstrapResolvers)+tr(" T1 resolvers..."));
	for(n=0; n < nBootstrapResolvers; n++)
	{
		OpenNICResolverPoolItem item = mResolverPool.at(n);
		OpenNICSystem::insertSystemResolver(item.hostAddress(),n+1);
		log(" > "+item.toString());
	}
	rc=n;
	/** get the T2 resolvers */
	log(tr("Fetching T2 resolvers..."));
	QStringList t2List = OpenNICSystem::getBootstrapT2List();
	for(n=0; n < t2List.count(); n++)
	{
		QString resolver = t2List.at(n).trimmed();
		if ( !resolver.isEmpty() )
		{
			OpenNICResolverPoolItem item(QHostAddress(resolver),"T2");
			mResolverPool.insort(item);
			mResolversInitialized=true;
		}
	}
	log(tr("Found ")+QString::number(t2List.count())+tr(" T2 resolvers"));
	log("mResolversInitialized="+QString(mResolversInitialized?"TRUE":"FALSE"));
	return rc;
}

/**
  * @brief determine if the active resolvers should be replaces with those proposed.
  * @return true to replace active resolvers with those proposed, else false
  */
bool OpenNICServer::shouldReplaceWithProposed(OpenNICResolverPool& proposed)
{
	if ( proposed.count() >= 2 && proposed.count() == mResolverCache.count() )
	{
		int diffCount=0; /* number of differences */
		for(int n=0; n < proposed.count(); n++)
		{
			OpenNICResolverPoolItem& item = proposed.at(n);
			if (!mResolverCache.contains(item))
			{
				++diffCount;
			}
		}
		return diffCount >= mResolverCache.count()/2; /* true if at least %50 different */
	}
	else if (proposed.count() == 1 && mResolverCache.count() == 1)
	{
		return proposed.at(0) == mResolverCache.at(0);
	}
	return true; /* when in doubt, replace */
}

/**
  * @brief replace the active resolvers with those proposed
  */
void OpenNICServer::replaceActiveResolvers(OpenNICResolverPool& proposed)
{
	mResolverCache.clear();
	proposed.sort();
	log("Applying new resolver cache of ("+QString::number(proposed.count())+") items...");
	for(int n=0; n < proposed.count(); n++)
	{
		OpenNICResolverPoolItem& item = proposed.at(n);
		OpenNICSystem::insertSystemResolver(item.hostAddress(),n+1);
		mResolverCache.append(item);
		log(" > "+item.toString());
	}
}

/**
  * @brief Perform the update function. Fetch DNS candidates, test for which to apply, and apply them.
  * @return the number of resolvers
  */
int OpenNICServer::updateDNS(int resolverCount)
{
	int rc=0;
	if ( !mUpdatingDNS )
	{
		log("** UPDATE DNS **");
		OpenNICResolverPool proposed;
		mUpdatingDNS=true;
		log("Sorting resolver pool.");
		mResolverPool.sort();
		log("Proposing ("+QString::number(resolverCount)+") candidates.");
		for(int n=0; n < mResolverPool.count() && n < resolverCount; n++)
		{
			OpenNICResolverPoolItem& item = mResolverPool.at(n);
			proposed.append(item);
		}
		/** see if what we are proposing is much different than what we have cach'ed already... */
		if ( shouldReplaceWithProposed(proposed) )
		{
			replaceActiveResolvers(proposed);
			rc=mResolverCache.count();
		}
		mUpdatingDNS=false;
	}
	return rc;
}

/**
  * @brief test the integrity of the resolver cache. look for dead resolvers.
  */
bool OpenNICServer::testResolverCache()
{
	for(int n=0; n < mResolverCache.count(); n++ )
	{
		OpenNICResolverPoolItem& item = mResolverCache.at(n);
		if ( !item.alive() )
		{
			log("** ACTIVE RESOLVER "+item.hostAddress().toString()+"' NOT RESPONDING **");
			return false;
		}
	}
	return true;
}

/**
  * @brief get here once in a while to see if we need to refresh any resolvers.
  */
void OpenNICServer::refreshResolvers(bool force)
{
	if ( !mResolversInitialized )							/* have we started any resolvers yet? */
	{
		coldBoot();											/* start from scratch */
	}
	if (force || mResolverCache.count()==0 || mResolverCache.count() < resolverCacheSize() )
	{
		updateDNS(resolverCacheSize());
	}
	else if (mResolverCache.count() && !testResolverCache())		/* how are our currently active resolvers doing?.. */
	{
		updateDNS(resolverCacheSize());						/* ...not so good, get new ones. */
	}
}

/**
  * run regular functions
  */
void OpenNICServer::runOnce()
{
	readSettings();
	refreshResolvers();										/* try to be smart */
	if (mSessions.count())
	{
		purgeDeadSesssions();									/* free up closed gui sessions */
		announcePackets();										/* tell gui sessions what they need to know */
		pruneLog();												/* don't let the log get out of hand */
	}
}

/**
  * @brief get here on timed events.
  */
void OpenNICServer::timerEvent(QTimerEvent* e)
{
	if ( e->timerId() == mFastTimer )
	{
		runOnce();											/* don't let the log get out of hand */
	}
	else if ( e->timerId() == mRefreshTimer )				/* get here once in a while, a slow timer... */
	{
		refreshResolvers(true);								/* force a resolver cache refresh */
	}
	else
	{
		inherited::timerEvent(e);
	}
}




