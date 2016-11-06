#include "robomongo/core/domain/MongoServer.h"

#include "robomongo/core/domain/MongoDatabase.h"
#include "robomongo/core/settings/ConnectionSettings.h"
#include "robomongo/core/settings/SshSettings.h"
#include "robomongo/core/settings/SettingsManager.h"
#include "robomongo/core/settings/ReplicaSetSettings.h"
#include "robomongo/core/mongodb/MongoWorker.h"
#include "robomongo/core/mongodb/SshTunnelWorker.h"
#include "robomongo/core/AppRegistry.h"
#include "robomongo/core/domain/App.h"
#include "robomongo/core/EventBus.h"
#include "robomongo/core/utils/QtUtils.h"

namespace Robomongo {
    R_REGISTER_EVENT(MongoServerLoadingDatabasesEvent)

    MongoServer::MongoServer(int handle, ConnectionSettings *settings, ConnectionType connectionType) : QObject(),
        _version(0.0f),
        _connectionType(connectionType),
        _client(NULL),
        _isConnected(false),
        _settings(settings),
        _handle(handle),
        _bus(AppRegistry::instance().bus()),
        _app(AppRegistry::instance().app()) { }

    bool MongoServer::isConnected() const {
        return _isConnected;
    }

    ConnectionSettings *MongoServer::connectionRecord() const {
        return _settings;
    }

    MongoServer::~MongoServer() {
        clearDatabases();

        if (_client != NULL) {
            _client->stopAndDelete();
        }

        // MongoWorker "_client" does not deleted here, because it is now owned by
        // another thread (call to moveToThread() made in MongoWorker constructor).
        // It will be deleted by this thread by means of "deleteLater()", which
        // is also specified in MongoWorker constructor.
    }

    /**
     * @brief Try to connect to MongoDB server.
     * @throws MongoException, if fails
     */
    void MongoServer::tryConnect() 
    {
        _bus->send(_client, new EstablishConnectionRequest(this, _connectionType));
    }

    void MongoServer::tryRefresh() 
    {
        _bus->send(_client, new EstablishConnectionRequest(this, ConnectionRefresh));
    }

    void MongoServer::tryRefreshReplicaSet()
    {
        _bus->send(_client, new RefreshReplicaSetRequest(this));
    }
    
    QStringList MongoServer::getDatabasesNames() const {
        QStringList result;
        for (QList<MongoDatabase *>::const_iterator it = _databases.begin(); it != _databases.end(); ++it) {
            MongoDatabase *datab = *it;
            result.append(QtUtils::toQString(datab->name()));
        }
        return result;
    }

    void MongoServer::createDatabase(const std::string &dbName) {
        _bus->send(_client, new CreateDatabaseRequest(this, dbName));
    }

    MongoDatabase *MongoServer::findDatabaseByName(const std::string &dbName) const {
        for (QList<MongoDatabase *>::const_iterator it = _databases.begin(); it != _databases.end(); ++it) {
            MongoDatabase *datab = *it;
            if (datab->name() == dbName) {
                return datab;
            }
        }
        return NULL;
    }

    void MongoServer::dropDatabase(const std::string &dbName) {
        _bus->send(_client, new DropDatabaseRequest(this, dbName));
    }

    void MongoServer::insertDocuments(const std::vector<mongo::BSONObj> &objCont,
                                      const MongoNamespace &ns) {
        for (std::vector<mongo::BSONObj>::const_iterator it = objCont.begin(); it != objCont.end(); it++) {
            insertDocument(*it, ns);
        }
    }

    void MongoServer::insertDocument(const mongo::BSONObj &obj, const MongoNamespace &ns) {
        _bus->send(_client, new InsertDocumentRequest(this, obj, ns));
    }

    void MongoServer::saveDocuments(const std::vector<mongo::BSONObj> &objCont, const MongoNamespace &ns) {
        for (std::vector<mongo::BSONObj>::const_iterator it = objCont.begin(); it != objCont.end(); it++) {
            saveDocument(*it, ns);
        }
    }

    void MongoServer::saveDocument(const mongo::BSONObj &obj, const MongoNamespace &ns) {
        _bus->send(_client, new InsertDocumentRequest(this, obj, ns, true));
    }

    void MongoServer::removeDocuments(mongo::Query query, const MongoNamespace &ns, bool justOne) {
        _bus->send(_client, new RemoveDocumentRequest(this, query, ns, justOne));
    }

    void MongoServer::loadDatabases() {
        _bus->publish(new MongoServerLoadingDatabasesEvent(this));
        _bus->send(_client, new LoadDatabaseNamesRequest(this));
    }

    void MongoServer::clearDatabases() {
        qDeleteAll(_databases);
        _databases.clear();
    }

    void MongoServer::addDatabase(MongoDatabase *database) {
        _databases.append(database);
    }

    void MongoServer::handle(EstablishConnectionResponse *event) {
        if (event->isError()) {
            _isConnected = false;

            std::stringstream ss("Unknown error");
            auto eventErrorReason = event->_errorReason;
            if (EstablishConnectionResponse::ErrorReason::MongoSslConnection == eventErrorReason)
            {
                auto reason = ConnectionFailedEvent::SslConnection;
                ss.clear();
                ss << "Cannot connect to the MongoDB at " << connectionRecord()->getFullAddress()
                    << ".\n\nError:\n" << "SSL connection failure: " << event->error().errorMessage();
                _app->fireConnectionFailedEvent(_handle, _connectionType, ss.str(), reason);
            }
            else
            {
                auto reason = (EstablishConnectionResponse::ErrorReason::MongoAuth == eventErrorReason) ?
                    ConnectionFailedEvent::MongoAuth : ConnectionFailedEvent::MongoConnection;
                ss.clear();
                ss << "Cannot connect to the MongoDB at " << connectionRecord()->getFullAddress()
                    << ".\n\nError:\n" << event->error().errorMessage();
                _app->fireConnectionFailedEvent(_handle, _connectionType, ss.str(), reason);
            }

            // When connection cannot be established, we should cleanup this instance of MongoServer if it wasn't
            // shown in UI (i.e. it is not a Secondary connection that is used for shells tab)
            if (_connectionType == ConnectionPrimary || _connectionType == ConnectionTest)
            {
                _app->closeServer(this);
            }

            return;
        }

        // Save various information after successful connection
        const ConnectionInfo &info = event->info();
        _version = info._version;
        _storageEngineType = info._storageEngineType;
        _isConnected = true;
        // todo: move to func
        // Save Replica Set Status  
        //_repSetName = event->getRepSetName();
        _repPrimary = event->replicaSet().primary;                   // todo: what happens to this member if not replica set?
        _repMembersAndHealths = event->replicaSet().membersAndHealths;
        if (_settings->isReplicaSet()) {
            _settings->setServerHost(_repPrimary.host());
            _settings->setServerPort(_repPrimary.port());
            _settings->replicaSetSettings()->setSetName(event->replicaSet().setName);
            std::vector<std::string> members;
            for (auto const& memberAndHealth : event->replicaSet().membersAndHealths) {
                members.push_back(memberAndHealth.first);
            }
            _settings->replicaSetSettings()->setMembers(members);
            // Save replica set settings
            AppRegistry::instance().settingsManager()->save();
        }

        // ConnectionRefresh is used just to update connection view (_version, _storageEngineType, _repPrimary etc..)
        // So we return here after updating(refreshing) information related to connection view.
        if (ConnectionRefresh == event->_connectionType)
            return;

        _bus->publish(new ConnectionEstablishedEvent(this, _connectionType));

        // Do nothing if this is not a "primary" connection
        if (ConnectionPrimary != _connectionType)
            return;

        clearDatabases();
        for (std::vector<std::string>::const_iterator it = info._databases.begin(); it != info._databases.end(); ++it) {
            const std::string &name = *it;
            MongoDatabase *db  = new MongoDatabase(this, name);
            addDatabase(db);
        }
    }

    void MongoServer::handle(RefreshReplicaSetResponse *event)
    {
        if (event->isError()) {
            _bus->publish(new ReplicaSetUpdated(this, event->error()));
            return;
        }

        _repPrimary = event->replicaSet.primary;                   // todo: what happens to this member if not replica set?
        _repMembersAndHealths = event->replicaSet.membersAndHealths;
        if (_settings->isReplicaSet()) {
            _settings->setServerHost(_repPrimary.host());
            _settings->setServerPort(_repPrimary.port());
            _settings->replicaSetSettings()->setSetName(event->replicaSet.setName);
            std::vector<std::string> members;
            for (auto const& memberAndHealth : event->replicaSet.membersAndHealths) {
                members.push_back(memberAndHealth.first);
            }
            _settings->replicaSetSettings()->setMembers(members);
            // Save replica set settings
            AppRegistry::instance().settingsManager()->save();
        }

        _bus->publish(new ReplicaSetUpdated(this));
    }

    void MongoServer::handle(LoadDatabaseNamesResponse *event) {
        if (event->isError()) {
            _bus->publish(new DatabaseListLoadedEvent(this, event->error()));
            return;
        }

        clearDatabases();
        for (std::vector<std::string>::iterator it = event->databaseNames.begin(); it != event->databaseNames.end(); ++it) {
            const std::string &name = *it;
            MongoDatabase *db  = new MongoDatabase(this, name);
            addDatabase(db);
        }

        _bus->publish(new DatabaseListLoadedEvent(this, _databases));
    }

    void MongoServer::handle(InsertDocumentResponse *event) {
        _bus->publish(new InsertDocumentResponse(event->sender(), event->error()));
    }

    void MongoServer::handle(RemoveDocumentResponse *event) {
        _bus->publish(new RemoveDocumentResponse(event->sender(), event->error()));
    }

    void MongoServer::runWorkerThread() {
        _client = new MongoWorker(_settings->clone(),
            AppRegistry::instance().settingsManager()->loadMongoRcJs(),
            AppRegistry::instance().settingsManager()->batchSize(),
            AppRegistry::instance().settingsManager()->mongoTimeoutSec(),
            AppRegistry::instance().settingsManager()->shellTimeoutSec());
    }

    void MongoServer::handle(CreateDatabaseResponse *event) {
        genericResponseHandler(event, "Failed to create database.");
    }

    void MongoServer::handle(DropDatabaseResponse *event) {
        genericResponseHandler(event, "Failed to drop database.");
    }

    void MongoServer::genericResponseHandler(Event *event, const std::string &userFriendlyMessage) {
        if (!event->isError())
            return;

        _bus->publish(new OperationFailedEvent(this, event->error().errorMessage(), userFriendlyMessage));
    }
}
