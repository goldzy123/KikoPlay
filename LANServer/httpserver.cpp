#include "httpserver.h"
#include "qhttpengine/qobjecthandler.h"
#include "qhttpengine/handler.h"
#include "mediahandler.h"

#include "Common/network.h"
#include "Play/Playlist/playlist.h"
#include "Play/Danmu/common.h"
#include "Play/Danmu/Manager/danmumanager.h"
#include "Play/Danmu/Manager/pool.h"
#include "globalobjects.h"

#include <QCoreApplication>
#include <QMimeDatabase>
#include <QFileInfo>


HttpServer::HttpServer(QObject *parent) : QObject(parent)
{
    MediaHandler *handler=new MediaHandler(&mediaHash,this);
    const QString strApp(QCoreApplication::applicationDirPath()+"/web");
    QObject::connect(handler, &MediaHandler::requestMedia, this, &HttpServer::genLog);
#ifdef CONFIG_UNIX_DATA
    const QString strHome(QDir::homePath()+"/.config/kikoplay/web");
    const QString strSys("/usr/share/kikoplay/web");

    const QFileInfo fileinfoHome(strHome);
    const QFileInfo fileinfoSys(strSys);
    const QFileInfo fileinfoApp(strApp);

    if (fileinfoHome.exists() || fileinfoHome.isDir()) {
        handler->setDocumentRoot(strHome);
    } else if (fileinfoSys.exists() || fileinfoSys.isDir()) {
        handler->setDocumentRoot(strSys);
    } else {
        handler->setDocumentRoot(strApp);
    }
#else
    handler->setDocumentRoot(strApp);
#endif
    handler->addRedirect(QRegExp("^$"), "/index.html");

    QHttpEngine::QObjectHandler *apiHandler=new QHttpEngine::QObjectHandler(this);
    apiHandler->registerMethod("playlist", this, &HttpServer::api_Playlist);
    apiHandler->registerMethod("updateTime", this, &HttpServer::api_UpdateTime);
    apiHandler->registerMethod("danmu/v3/", this, &HttpServer::api_Danmu);
    apiHandler->registerMethod("subtitle", this, &HttpServer::api_Subtitle);
    apiHandler->registerMethod("danmu/full/", this, &HttpServer::api_DanmuFull);
    apiHandler->registerMethod("updateDelay", this, &HttpServer::api_UpdateDelay);
    apiHandler->registerMethod("updateTimeline", this, &HttpServer::api_UpdateTimeline);
    handler->addSubHandler(QRegExp("api/"), apiHandler);

    server = new QHttpEngine::Server(handler,this);
}

HttpServer::~HttpServer()
{
    server->close();
}

bool HttpServer::startServer(quint16 port)
{

   if(server->isListening())return true;
   bool r = server->listen(QHostAddress::AnyIPv4,port);
   genLog(r?"Server start":server->errorString());
   if(!r) server->close();
   return r;
}

void HttpServer::stopServer()
{
    server->close();
    genLog("Server close");
}

void HttpServer::genLog(const QString &logInfo)
{
    emit showLog(QString("%1%2").arg(QTime::currentTime().toString("[hh:mm:ss]"),logInfo));
}

void HttpServer::api_Playlist(QHttpEngine::Socket *socket)
{
    QMetaObject::invokeMethod(GlobalObjects::playlist,[this](){
        GlobalObjects::playlist->dumpJsonPlaylist(playlistDoc,mediaHash);
    },Qt::BlockingQueuedConnection);
    genLog(QString("[%1]Request:Playlist").arg(socket->peerAddress().toString()));

    QByteArray data = playlistDoc.toJson();
    QByteArray compressedBytes;
    Network::gzipCompress(data,compressedBytes);

    socket->setHeader("Content-Length", QByteArray::number(compressedBytes.length()));
    socket->setHeader("Content-Type", "application/json");
    socket->setHeader("Content-Encoding", "gzip");
    socket->writeHeaders();
    socket->write(compressedBytes);
    socket->close();
}

void HttpServer::api_UpdateTime(QHttpEngine::Socket *socket)
{
    bool syncPlayTime=GlobalObjects::appSetting->value("Server/SyncPlayTime",true).toBool();
    if(syncPlayTime)
    {
        QJsonDocument document;
        if (socket->readJson(document))
        {
            genLog(QString("[%1]Request:UpdateTime").arg(socket->peerAddress().toString()));
            QVariantMap data = document.object().toVariantMap();
            QString mediaPath=mediaHash.value(data.value("mediaId").toString());
            int playTime=data.value("playTime").toInt();
            int playTimeState=data.value("playTimeState").toInt();
            QMetaObject::invokeMethod(GlobalObjects::playlist,[mediaPath,playTime,playTimeState](){
                GlobalObjects::playlist->updatePlayTime(mediaPath,playTime,playTimeState);
            },Qt::QueuedConnection);
        }
    }
    socket->close();
}

void HttpServer::api_Danmu(QHttpEngine::Socket *socket)
{ 
    QString poolId=socket->queryString().value("id");
    bool update=(socket->queryString().value("update").toLower()=="true");
    Pool *pool=GlobalObjects::danmuManager->getPool(poolId);
    genLog(QString("[%1]Request:Danmu %2%3").arg(socket->peerAddress().toString(),
                                                   pool?pool->epTitle():"",
                                                   update?", update=true":""));
    QJsonArray danmuArray;
    if(pool)
    {
        if(update)
        {
            QList<QSharedPointer<DanmuComment> > incList;
            pool->update(-1,&incList);
            danmuArray=Pool::exportJson(incList);
        }
        else
        {
            danmuArray=pool->exportJson();
        }
    }
    QJsonObject resposeObj
    {
        {"code", 0},
        {"data", danmuArray},
        {"update",update}
    };
    QByteArray data = QJsonDocument(resposeObj).toJson();
    QByteArray compressedBytes;
    Network::gzipCompress(data,compressedBytes);
    socket->setHeader("Content-Length", QByteArray::number(compressedBytes.length()));
    socket->setHeader("Content-Type", "application/json");
    socket->setHeader("Content-Encoding", "gzip");
    socket->writeHeaders();
    socket->write(compressedBytes);
    socket->close();
}

void HttpServer::api_DanmuFull(QHttpEngine::Socket *socket)
{
    QString poolId=socket->queryString().value("id");
    bool update=(socket->queryString().value("update").toLower()=="true");
    Pool *pool=GlobalObjects::danmuManager->getPool(poolId);
    genLog(QString("[%1]Request:Danmu(Full) %2%3").arg(socket->peerAddress().toString(),
                                                   pool?pool->epTitle():"",
                                                   update?", update=true":""));
    QJsonObject resposeObj;
    if(pool)
    {
        if(update)
        {
            QList<QSharedPointer<DanmuComment> > incList;
            pool->update(-1,&incList);
            resposeObj=
            {
                {"comment", Pool::exportJson(incList, true)},
                {"update", true}
            };
        }
        else
        {
            resposeObj=pool->exportFullJson();
            resposeObj.insert("update", false);
        }
    }
    QByteArray data = QJsonDocument(resposeObj).toJson();
    QByteArray compressedBytes;
    Network::gzipCompress(data,compressedBytes);
    socket->setHeader("Content-Length", QByteArray::number(compressedBytes.length()));
    socket->setHeader("Content-Type", "application/json");
    socket->setHeader("Content-Encoding", "gzip");
    socket->writeHeaders();
    socket->write(compressedBytes);
    socket->close();
}

void HttpServer::api_UpdateDelay(QHttpEngine::Socket *socket)
{
    QJsonDocument document;
    if (socket->readJson(document))
    {
        QVariantMap data = document.object().toVariantMap();
        QString poolId=data.value("danmuPool").toString();
        int delay=data.value("delay").toInt();  //ms
        int sourceId=data.value("source").toInt();
        genLog(QString("[%1]Request:UpdateDelay, SourceId: %2").arg(socket->peerAddress().toString(),QString::number(sourceId)));
        Pool *pool=GlobalObjects::danmuManager->getPool(poolId,false);
        if(pool) pool->setDelay(sourceId, delay);
    }
    socket->close();
}

void HttpServer::api_UpdateTimeline(QHttpEngine::Socket *socket)
{
    QJsonDocument document;
    if (socket->readJson(document))
    {
        QVariantMap data = document.object().toVariantMap();
        QString poolId=data.value("danmuPool").toString();
        QString timelineStr=data.value("timeline").toString();
        int sourceId=data.value("source").toInt();
        genLog(QString("[%1]Request:UpdateTimeline, SourceId: %2").arg(socket->peerAddress().toString(),QString::number(sourceId)));
        Pool *pool=GlobalObjects::danmuManager->getPool(poolId,false);
        DanmuSourceInfo srcInfo;
        srcInfo.setTimeline(timelineStr);
        if(pool) pool->setTimeline(sourceId, srcInfo.timelineInfo);
    }
    socket->close();
}

void HttpServer::api_Subtitle(QHttpEngine::Socket *socket)
{
    QString mediaId=socket->queryString().value("id");
    QString mediaPath=mediaHash.value(mediaId);
    QFileInfo fi(mediaPath);
    QString dir=fi.absolutePath(),name=fi.baseName();
    static QStringList supportedSubFormats={"","ass","ssa","srt"};
    genLog(QString("[%1]Request:Subtitle - %2").arg(socket->peerAddress().toString(),name));
    int formatIndex=0;
    for(int i=1;i<4;++i)
    {
        QFileInfo subInfo(dir,name+"."+supportedSubFormats[i]);
        if(subInfo.exists())
        {
            formatIndex=i;
            break;
        }
    }
    QJsonObject resposeObj
    {
        {"type", supportedSubFormats[formatIndex]}
    };
    QByteArray data = QJsonDocument(resposeObj).toJson();
    socket->setHeader("Content-Length", QByteArray::number(data.length()));
    socket->setHeader("Content-Type", "application/json");
    socket->writeHeaders();
    socket->write(data);
    socket->close();
}
