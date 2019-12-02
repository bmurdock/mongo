/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <iosfwd>
#include <memory>
#include <ostream>

#include "mongo/client/fetcher.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/repl/collection_cloner.h"
#include "mongo/db/repl/data_replicator_external_state_mock.h"
#include "mongo/db/repl/initial_syncer.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery_mock.h"
#include "mongo/db/repl/reporter.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/sync_source_resolver.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/db/repl/sync_source_selector_mock.h"
#include "mongo/db/repl/task_executor_mock.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {

/**
 * Insertion operator for InitialSyncer::State. Formats initial syncer state for output stream.
 */
std::ostream& operator<<(std::ostream& os, const InitialSyncer::State& state) {
    switch (state) {
        case InitialSyncer::State::kPreStart:
            return os << "PreStart";
        case InitialSyncer::State::kRunning:
            return os << "Running";
        case InitialSyncer::State::kShuttingDown:
            return os << "ShuttingDown";
        case InitialSyncer::State::kComplete:
            return os << "Complete";
    }
    MONGO_UNREACHABLE;
}

}  // namespace repl
}  // namespace mongo


namespace {

using namespace mongo;
using namespace mongo::repl;

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using unittest::log;

using LockGuard = stdx::lock_guard<Latch>;
using NetworkGuard = executor::NetworkInterfaceMock::InNetworkGuard;
using UniqueLock = stdx::unique_lock<Latch>;

const BSONObj kListDatabasesFailPointData = BSON("cloner"
                                                 << "AllDatabaseCloner"
                                                 << "stage"
                                                 << "listDatabases");

BSONObj makeListDatabasesResponse(std::vector<std::string> databaseNames);

struct CollectionCloneInfo {
    std::shared_ptr<CollectionMockStats> stats = std::make_shared<CollectionMockStats>();
    CollectionBulkLoaderMock* loader = nullptr;
    Status status{ErrorCodes::NotYetInitialized, ""};
};

class InitialSyncerTest : public executor::ThreadPoolExecutorTest,
                          public SyncSourceSelector,
                          public ScopedGlobalServiceContextForTest {
public:
    InitialSyncerTest() : _threadClient(getGlobalServiceContext()) {}

    executor::ThreadPoolMock::Options makeThreadPoolMockOptions() const override;
    executor::ThreadPoolMock::Options makeClonerThreadPoolMockOptions() const;

    /**
     * clear/reset state
     */
    void reset() {
        _setMyLastOptime = [this](const OpTimeAndWallTime& opTimeAndWallTime,
                                  ReplicationCoordinator::DataConsistency consistency) {
            _myLastOpTime = opTimeAndWallTime.opTime;
            _myLastWallTime = opTimeAndWallTime.wallTime;
        };
        _myLastOpTime = OpTime();
        _myLastWallTime = Date_t();
        _syncSourceSelector = std::make_unique<SyncSourceSelectorMock>();
    }

    // SyncSourceSelector
    void clearSyncSourceBlacklist() override {
        _syncSourceSelector->clearSyncSourceBlacklist();
    }
    HostAndPort chooseNewSyncSource(const OpTime& ot) override {
        return _syncSourceSelector->chooseNewSyncSource(ot);
    }
    void blacklistSyncSource(const HostAndPort& host, Date_t until) override {
        _syncSourceSelector->blacklistSyncSource(host, until);
    }
    bool shouldChangeSyncSource(const HostAndPort& currentSource,
                                const rpc::ReplSetMetadata& replMetadata,
                                boost::optional<rpc::OplogQueryMetadata> oqMetadata) override {
        return _syncSourceSelector->shouldChangeSyncSource(currentSource, replMetadata, oqMetadata);
    }

    void scheduleNetworkResponse(std::string cmdName, const BSONObj& obj) {
        NetworkInterfaceMock* net = getNet();
        if (!net->hasReadyRequests()) {
            log() << "The network doesn't have a request to process for this response: " << obj;
        }
        verifyNextRequestCommandName(cmdName);
        scheduleNetworkResponse(net->getNextReadyRequest(), obj);
    }

    void scheduleNetworkResponse(NetworkInterfaceMock::NetworkOperationIterator noi,
                                 const BSONObj& obj) {
        NetworkInterfaceMock* net = getNet();
        Milliseconds millis(0);
        RemoteCommandResponse response(obj, millis);
        log() << "Sending response for network request:";
        log() << "     req: " << noi->getRequest().dbname << "." << noi->getRequest().cmdObj;
        log() << "     resp:" << response;

        net->scheduleResponse(noi, net->now(), response);
    }

    void scheduleNetworkResponse(std::string cmdName, Status errorStatus) {
        NetworkInterfaceMock* net = getNet();
        if (!getNet()->hasReadyRequests()) {
            log() << "The network doesn't have a request to process for the error: " << errorStatus;
        }
        verifyNextRequestCommandName(cmdName);
        net->scheduleResponse(net->getNextReadyRequest(), net->now(), errorStatus);
    }

    void processNetworkResponse(std::string cmdName, const BSONObj& obj) {
        scheduleNetworkResponse(cmdName, obj);
        finishProcessingNetworkResponse();
    }

    void processNetworkResponse(std::string cmdName, Status errorStatus) {
        scheduleNetworkResponse(cmdName, errorStatus);
        finishProcessingNetworkResponse();
    }

    /**
     * Schedules and processes a successful response to the network request sent by InitialSyncer's
     * last oplog entry fetcher. Also validates the find command arguments in the request.
     */
    void processSuccessfulLastOplogEntryFetcherResponse(std::vector<BSONObj> docs);

    /**
     * Schedules and processes a successful response to the network request sent by InitialSyncer's
     * feature compatibility version fetcher. Includes the 'docs' provided in the response.
     */
    void processSuccessfulFCVFetcherResponse(std::vector<BSONObj> docs);

    /**
     * Schedules and processes a successful response to the network request sent by InitialSyncer's
     * feature compatibility version fetcher. Always includes a valid fCV=last-stable document in
     * the response.
     */
    void processSuccessfulFCVFetcherResponseLastStable();

    void finishProcessingNetworkResponse() {
        getNet()->runReadyNetworkOperations();
        if (getNet()->hasReadyRequests()) {
            log() << "The network has unexpected requests to process, next req:";
            const NetworkInterfaceMock::NetworkOperation& req = *getNet()->getNextReadyRequest();
            log() << req.getDiagnosticString();
        }
        ASSERT_FALSE(getNet()->hasReadyRequests());
    }

    InitialSyncer& getInitialSyncer() {
        return *_initialSyncer;
    }

    DataReplicatorExternalStateMock* getExternalState() {
        return _externalState;
    }

    StorageInterface& getStorage() {
        return *_storageInterface;
    }

protected:
    struct StorageInterfaceResults {
        bool createOplogCalled = false;
        bool truncateCalled = false;
        bool insertedOplogEntries = false;
        int oplogEntriesInserted = 0;
        bool droppedUserDBs = false;
        std::vector<std::string> droppedCollections;
        int documentsInsertedCount = 0;
    };

    // protects _storageInterfaceWorkDone.
    Mutex _storageInterfaceWorkDoneMutex =
        MONGO_MAKE_LATCH("InitialSyncerTest::_storageInterfaceWorkDoneMutex");
    StorageInterfaceResults _storageInterfaceWorkDone;

    void setUp() override {
        executor::ThreadPoolExecutorTest::setUp();
        _storageInterface = std::make_unique<StorageInterfaceMock>();
        _storageInterface->createOplogFn = [this](OperationContext* opCtx,
                                                  const NamespaceString& nss) {
            LockGuard lock(_storageInterfaceWorkDoneMutex);
            _storageInterfaceWorkDone.createOplogCalled = true;
            return Status::OK();
        };
        _storageInterface->truncateCollFn = [this](OperationContext* opCtx,
                                                   const NamespaceString& nss) {
            LockGuard lock(_storageInterfaceWorkDoneMutex);
            _storageInterfaceWorkDone.truncateCalled = true;
            return Status::OK();
        };
        _storageInterface->insertDocumentFn = [this](OperationContext* opCtx,
                                                     const NamespaceStringOrUUID& nsOrUUID,
                                                     const TimestampedBSONObj& doc,
                                                     long long term) {
            LockGuard lock(_storageInterfaceWorkDoneMutex);
            ++_storageInterfaceWorkDone.documentsInsertedCount;
            return Status::OK();
        };
        _storageInterface->insertDocumentsFn = [this](OperationContext* opCtx,
                                                      const NamespaceStringOrUUID& nsOrUUID,
                                                      const std::vector<InsertStatement>& ops) {
            LockGuard lock(_storageInterfaceWorkDoneMutex);
            _storageInterfaceWorkDone.insertedOplogEntries = true;
            ++_storageInterfaceWorkDone.oplogEntriesInserted;
            return Status::OK();
        };
        _storageInterface->dropCollFn = [this](OperationContext* opCtx,
                                               const NamespaceString& nss) {
            LockGuard lock(_storageInterfaceWorkDoneMutex);
            _storageInterfaceWorkDone.droppedCollections.push_back(nss.ns());
            return Status::OK();
        };
        _storageInterface->dropUserDBsFn = [this](OperationContext* opCtx) {
            LockGuard lock(_storageInterfaceWorkDoneMutex);
            _storageInterfaceWorkDone.droppedUserDBs = true;
            return Status::OK();
        };
        _storageInterface->createCollectionForBulkFn =
            [this](const NamespaceString& nss,
                   const CollectionOptions& options,
                   const BSONObj idIndexSpec,
                   const std::vector<BSONObj>& secondaryIndexSpecs)
            -> StatusWith<std::unique_ptr<CollectionBulkLoaderMock>> {
            // Get collection info from map.
            const auto collInfo = &_collections[nss];
            if (collInfo->stats->initCalled) {
                log() << "reusing collection during test which may cause problems, ns:" << nss;
            }
            auto localLoader = std::make_unique<CollectionBulkLoaderMock>(collInfo->stats);
            auto status = localLoader->init(secondaryIndexSpecs);
            if (!status.isOK())
                return status;
            collInfo->loader = localLoader.get();

            return std::move(localLoader);
        };

        ThreadPool::Options dbThreadPoolOptions;
        dbThreadPoolOptions.poolName = "dbthread";
        dbThreadPoolOptions.minThreads = 1U;
        dbThreadPoolOptions.maxThreads = 1U;
        dbThreadPoolOptions.onCreateThread = [](const std::string& threadName) {
            Client::initThread(threadName.c_str());
        };
        _dbWorkThreadPool = std::make_unique<ThreadPool>(dbThreadPoolOptions);
        _dbWorkThreadPool->startup();

        _target = HostAndPort{"localhost:12346"};
        _mockServer = std::make_unique<MockRemoteDBServer>(_target.toString());
        // Usually we're just skipping the cloners in this test, so we provide an empty list
        // of databases.
        _mockServer->setCommandReply("listDatabases", makeListDatabasesResponse({}));
        _options1.uuid = UUID::gen();

        reset();

        launchExecutorThread();

        _replicationProcess = std::make_unique<ReplicationProcess>(
            _storageInterface.get(),
            std::make_unique<ReplicationConsistencyMarkersMock>(),
            std::make_unique<ReplicationRecoveryMock>());

        _executorProxy = std::make_unique<TaskExecutorMock>(&getExecutor());

        _myLastOpTime = OpTime({3, 0}, 1);

        InitialSyncerOptions options;
        options.initialSyncRetryWait = Milliseconds(1);
        options.getMyLastOptime = [this]() { return _myLastOpTime; };
        options.setMyLastOptime = [this](const OpTimeAndWallTime& opTimeAndWallTime,
                                         ReplicationCoordinator::DataConsistency consistency) {
            _setMyLastOptime(opTimeAndWallTime, consistency);
        };
        options.resetOptimes = [this]() { _myLastOpTime = OpTime(); };
        options.syncSourceSelector = this;

        _options = options;

        ThreadPool::Options threadPoolOptions;
        threadPoolOptions.poolName = "replication";
        threadPoolOptions.minThreads = 1U;
        threadPoolOptions.maxThreads = 1U;
        threadPoolOptions.onCreateThread = [](const std::string& threadName) {
            Client::initThread(threadName.c_str());
        };

        auto dataReplicatorExternalState = std::make_unique<DataReplicatorExternalStateMock>();
        dataReplicatorExternalState->taskExecutor = _executorProxy.get();
        dataReplicatorExternalState->currentTerm = 1LL;
        dataReplicatorExternalState->lastCommittedOpTime = _myLastOpTime;
        {
            ReplSetConfig config;
            ASSERT_OK(
                config.initialize(BSON("_id"
                                       << "myset"
                                       << "version" << 1 << "protocolVersion" << 1 << "members"
                                       << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                << "localhost:12345"))
                                       << "settings" << BSON("electionTimeoutMillis" << 10000))));
            dataReplicatorExternalState->replSetConfigResult = config;
        }
        _externalState = dataReplicatorExternalState.get();

        _lastApplied = getDetectableErrorStatus();
        _onCompletion = [this](const StatusWith<OpTimeAndWallTime>& lastApplied) {
            _lastApplied = lastApplied;
        };

        _clonerExecutor = makeThreadPoolTestExecutor(std::make_unique<NetworkInterfaceMock>(),
                                                     makeClonerThreadPoolMockOptions());
        _clonerExecutor->startup();

        try {
            // When creating InitialSyncer, we wrap _onCompletion so that we can override the
            // InitialSyncer's callback behavior post-construction.
            // See InitialSyncerTransitionsToCompleteWhenFinishCallbackThrowsException.
            _initialSyncer = std::make_unique<InitialSyncer>(
                options,
                std::move(dataReplicatorExternalState),
                _dbWorkThreadPool.get(),
                _storageInterface.get(),
                _replicationProcess.get(),
                [this](const StatusWith<OpTimeAndWallTime>& lastApplied) {
                    _onCompletion(lastApplied);
                });
            _initialSyncer->setCreateClientFn_forTest([this]() {
                return std::unique_ptr<DBClientConnection>(
                    new MockDBClientConnection(_mockServer.get()));
            });
            _initialSyncer->setClonerExecutor_forTest(_clonerExecutor.get());
        } catch (...) {
            ASSERT_OK(exceptionToStatus());
        }
    }

    void tearDownExecutorThread() {
        if (_executorThreadShutdownComplete) {
            return;
        }
        getExecutor().shutdown();
        getExecutor().join();
        _clonerExecutor->shutdown();
        _clonerExecutor->join();
        _executorThreadShutdownComplete = true;
    }

    void tearDown() override {
        tearDownExecutorThread();
        _initialSyncer.reset();
        _dbWorkThreadPool.reset();
        _replicationProcess.reset();
        _storageInterface.reset();
    }

    /**
     * Note: An empty cmdName will skip validation.
     */
    void verifyNextRequestCommandName(std::string cmdName) {
        const auto net = getNet();
        ASSERT_TRUE(net->hasReadyRequests());

        if (cmdName != "") {
            const NetworkInterfaceMock::NetworkOperationIterator req =
                net->getFrontOfUnscheduledQueue();
            const BSONObj reqBSON = req->getRequest().cmdObj;
            const BSONElement cmdElem = reqBSON.firstElement();
            auto reqCmdName = cmdElem.fieldNameStringData();
            ASSERT_EQ(cmdName, reqCmdName);
        }
    }

    void runInitialSyncWithBadFCVResponse(std::vector<BSONObj> docs,
                                          ErrorCodes::Error expectedError);
    void doSuccessfulInitialSyncWithOneBatch();
    OplogEntry doInitialSyncWithOneBatch();

    std::unique_ptr<TaskExecutorMock> _executorProxy;
    std::unique_ptr<executor::ThreadPoolTaskExecutor> _clonerExecutor;

    InitialSyncerOptions _options;
    InitialSyncerOptions::SetMyLastOptimeFn _setMyLastOptime;
    OpTime _myLastOpTime;
    Date_t _myLastWallTime;
    std::unique_ptr<SyncSourceSelectorMock> _syncSourceSelector;
    std::unique_ptr<StorageInterfaceMock> _storageInterface;
    HostAndPort _target;
    std::unique_ptr<MockRemoteDBServer> _mockServer;
    CollectionOptions _options1;
    std::unique_ptr<ReplicationProcess> _replicationProcess;
    std::unique_ptr<ThreadPool> _dbWorkThreadPool;
    std::map<NamespaceString, CollectionMockStats> _collectionStats;
    std::map<NamespaceString, CollectionCloneInfo> _collections;

    StatusWith<OpTimeAndWallTime> _lastApplied = Status(ErrorCodes::NotYetInitialized, "");
    InitialSyncer::OnCompletionFn _onCompletion;

private:
    DataReplicatorExternalStateMock* _externalState;
    std::unique_ptr<InitialSyncer> _initialSyncer;
    ThreadClient _threadClient;
    bool _executorThreadShutdownComplete = false;
};

executor::ThreadPoolMock::Options InitialSyncerTest::makeThreadPoolMockOptions() const {
    executor::ThreadPoolMock::Options options;
    options.onCreateThread = []() { Client::initThread("InitialSyncerTest"); };
    return options;
}

executor::ThreadPoolMock::Options InitialSyncerTest::makeClonerThreadPoolMockOptions() const {
    executor::ThreadPoolMock::Options options;
    options.onCreateThread = []() { Client::initThread("ClonerThreadTest"); };
    return options;
}

void advanceClock(NetworkInterfaceMock* net, Milliseconds duration) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
    auto when = net->now() + duration;
    ASSERT_EQUALS(when, net->runUntil(when));
}

ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

/**
 * Generates a replSetGetRBID response.
 */
BSONObj makeRollbackCheckerResponse(int rollbackId) {
    return BSON("ok" << 1 << "rbid" << rollbackId);
}

/**
 * Generates a cursor response for a Fetcher to consume.
 */
RemoteCommandResponse makeCursorResponse(CursorId cursorId,
                                         const NamespaceString& nss,
                                         std::vector<BSONObj> docs,
                                         bool isFirstBatch = true,
                                         int rbid = 1) {
    OpTime futureOpTime(Timestamp(1000, 1000), 1000);
    Date_t futureWallTime = Date_t() + Seconds(futureOpTime.getSecs());
    rpc::OplogQueryMetadata oqMetadata({futureOpTime, futureWallTime}, futureOpTime, rbid, 0, 0);

    BSONObjBuilder bob;
    {
        BSONObjBuilder cursorBob(bob.subobjStart("cursor"));
        cursorBob.append("id", cursorId);
        cursorBob.append("ns", nss.toString());
        {
            BSONArrayBuilder batchBob(
                cursorBob.subarrayStart(isFirstBatch ? "firstBatch" : "nextBatch"));
            for (const auto& doc : docs) {
                batchBob.append(doc);
            }
        }
    }
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    bob.append("ok", 1);
    return {bob.obj(), Milliseconds()};
}

/**
 * Generates a listDatabases response for an AllDatabaseCloner to consume.
 */
BSONObj makeListDatabasesResponse(std::vector<std::string> databaseNames) {
    BSONObjBuilder bob;
    {
        BSONArrayBuilder databasesBob(bob.subarrayStart("databases"));
        for (const auto& name : databaseNames) {
            BSONObjBuilder nameBob(databasesBob.subobjStart());
            nameBob.append("name", name);
        }
    }
    bob.append("ok", 1);
    return bob.obj();
}

/**
 * Generates oplog entries with the given number used for the timestamp.
 */
OplogEntry makeOplogEntry(int t,
                          OpTypeEnum opType = OpTypeEnum::kInsert,
                          int version = OplogEntry::kOplogVersion) {
    BSONObj oField = BSON("_id" << t << "a" << t);
    if (opType == OpTypeEnum::kCommand) {
        // Insert an arbitrary command name so that the oplog entry is valid.
        oField = BSON("dropIndexes"
                      << "a_1");
    }
    return OplogEntry(OpTime(Timestamp(t, 1), 1),  // optime
                      boost::none,                 // hash
                      opType,                      // op type
                      NamespaceString("a.a"),      // namespace
                      boost::none,                 // uuid
                      boost::none,                 // fromMigrate
                      version,                     // version
                      oField,                      // o
                      boost::none,                 // o2
                      {},                          // sessionInfo
                      boost::none,                 // upsert
                      Date_t() + Seconds(t),       // wall clock time
                      boost::none,                 // statement id
                      boost::none,   // optime of previous write within same transaction
                      boost::none,   // pre-image optime
                      boost::none);  // post-image optime
}

BSONObj makeOplogEntryObj(int t,
                          OpTypeEnum opType = OpTypeEnum::kInsert,
                          int version = OplogEntry::kOplogVersion) {
    return makeOplogEntry(t, opType, version).toBSON();
}

void InitialSyncerTest::processSuccessfulLastOplogEntryFetcherResponse(std::vector<BSONObj> docs) {
    auto net = getNet();
    auto request = assertRemoteCommandNameEquals(
        "find",
        net->scheduleSuccessfulResponse(makeCursorResponse(0LL, _options.localOplogNS, docs)));
    ASSERT_EQUALS(1, request.cmdObj.getIntField("limit"));
    ASSERT_TRUE(request.cmdObj.hasField("sort"));
    ASSERT_EQUALS(mongo::BSONType::Object, request.cmdObj["sort"].type());
    ASSERT_BSONOBJ_EQ(BSON("$natural" << -1), request.cmdObj.getObjectField("sort"));
    net->runReadyNetworkOperations();
}

void assertFCVRequest(RemoteCommandRequest request) {
    ASSERT_EQUALS(NamespaceString::kServerConfigurationNamespace.db(), request.dbname)
        << request.toString();
    ASSERT_EQUALS(NamespaceString::kServerConfigurationNamespace.coll(),
                  request.cmdObj.getStringField("find"));
    ASSERT_BSONOBJ_EQ(BSON("_id" << FeatureCompatibilityVersionParser::kParameterName),
                      request.cmdObj.getObjectField("filter"));
}

void InitialSyncerTest::processSuccessfulFCVFetcherResponseLastStable() {
    auto docs = {BSON("_id" << FeatureCompatibilityVersionParser::kParameterName << "version"
                            << FeatureCompatibilityVersionParser::kVersion42)};
    processSuccessfulFCVFetcherResponse(docs);
}

void InitialSyncerTest::processSuccessfulFCVFetcherResponse(std::vector<BSONObj> docs) {
    auto net = getNet();
    auto request = assertRemoteCommandNameEquals(
        "find",
        net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kServerConfigurationNamespace, docs)));
    assertFCVRequest(request);
    net->runReadyNetworkOperations();
}

TEST_F(InitialSyncerTest, InvalidConstruction) {
    InitialSyncerOptions options;
    options.getMyLastOptime = []() { return OpTime(); };
    options.setMyLastOptime = [](const OpTimeAndWallTime&,
                                 ReplicationCoordinator::DataConsistency consistency) {};
    options.resetOptimes = []() {};
    options.syncSourceSelector = this;
    auto callback = [](const StatusWith<OpTimeAndWallTime>&) {};

    // Null task executor in external state.
    {
        auto dataReplicatorExternalState = std::make_unique<DataReplicatorExternalStateMock>();
        ASSERT_THROWS_CODE_AND_WHAT(InitialSyncer(options,
                                                  std::move(dataReplicatorExternalState),
                                                  _dbWorkThreadPool.get(),
                                                  _storageInterface.get(),
                                                  _replicationProcess.get(),
                                                  callback),
                                    AssertionException,
                                    ErrorCodes::BadValue,
                                    "task executor cannot be null");
    }

    // Null callback function.
    {
        auto dataReplicatorExternalState = std::make_unique<DataReplicatorExternalStateMock>();
        dataReplicatorExternalState->taskExecutor = &getExecutor();
        ASSERT_THROWS_CODE_AND_WHAT(InitialSyncer(options,
                                                  std::move(dataReplicatorExternalState),
                                                  _dbWorkThreadPool.get(),
                                                  _storageInterface.get(),
                                                  _replicationProcess.get(),
                                                  InitialSyncer::OnCompletionFn()),
                                    AssertionException,
                                    ErrorCodes::BadValue,
                                    "callback function cannot be null");
    }
}

TEST_F(InitialSyncerTest, CreateDestroy) {}

const std::uint32_t maxAttempts = 1U;

TEST_F(InitialSyncerTest, StartupReturnsIllegalOperationIfAlreadyActive) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();
    ASSERT_FALSE(initialSyncer->isActive());
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));
    ASSERT_TRUE(initialSyncer->isActive());
    ASSERT_EQUALS(ErrorCodes::IllegalOperation, initialSyncer->startup(opCtx.get(), maxAttempts));
    ASSERT_TRUE(initialSyncer->isActive());
}

TEST_F(InitialSyncerTest, StartupReturnsShutdownInProgressIfInitialSyncerIsShuttingDown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();
    ASSERT_FALSE(initialSyncer->isActive());
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));
    ASSERT_TRUE(initialSyncer->isActive());
    // SyncSourceSelector returns an invalid sync source so InitialSyncer is stuck waiting for
    // another sync source in 'Options::syncSourceRetryWait' ms.
    ASSERT_OK(initialSyncer->shutdown());
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, initialSyncer->startup(opCtx.get(), maxAttempts));
}

TEST_F(InitialSyncerTest, StartupReturnsShutdownInProgressIfExecutorIsShutdown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();
    getExecutor().shutdown();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, initialSyncer->startup(opCtx.get(), maxAttempts));
    ASSERT_FALSE(initialSyncer->isActive());

    // Cannot startup initial syncer again since it's in the Complete state.
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, initialSyncer->startup(opCtx.get(), maxAttempts));
}

TEST_F(InitialSyncerTest, ShutdownTransitionsStateToCompleteIfCalledBeforeStartup) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();
    ASSERT_OK(initialSyncer->shutdown());
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, initialSyncer->startup(opCtx.get(), maxAttempts));
    // Initial syncer is inactive when it's in the Complete state.
    ASSERT_FALSE(initialSyncer->isActive());
}

TEST_F(InitialSyncerTest, StartupSetsInitialSyncFlagOnSuccess) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // Initial sync flag should not be set before starting.
    ASSERT_FALSE(_replicationProcess->getConsistencyMarkers()->getInitialSyncFlag(opCtx.get()));

    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));
    ASSERT_TRUE(initialSyncer->isActive());

    // Initial sync flag should be set.
    ASSERT_TRUE(_replicationProcess->getConsistencyMarkers()->getInitialSyncFlag(opCtx.get()));
}

TEST_F(InitialSyncerTest, StartupSetsInitialDataTimestampAndStableTimestampOnSuccess) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // Set initial data timestamp forward first.
    auto serviceCtx = opCtx.get()->getServiceContext();
    _storageInterface->setInitialDataTimestamp(serviceCtx, Timestamp(5, 5));
    _storageInterface->setStableTimestamp(serviceCtx, Timestamp(6, 6));

    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));
    ASSERT_TRUE(initialSyncer->isActive());

    ASSERT_EQUALS(Timestamp::kAllowUnstableCheckpointsSentinel,
                  _storageInterface->getInitialDataTimestamp());
    ASSERT_EQUALS(Timestamp::min(), _storageInterface->getStableTimestamp());
}

TEST_F(InitialSyncerTest, InitialSyncerReturnsCallbackCanceledIfShutdownImmediatelyAfterStartup) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    // This will cancel the _startInitialSyncAttemptCallback() task scheduled by startup().
    ASSERT_OK(initialSyncer->shutdown());

    // Depending on which InitialSyncer stage (_chooseSyncSource or _rollbackCheckerResetCallback)
    // was interrupted by shutdown(), we may have to request the network interface to deliver
    // cancellation signals to the InitialSyncer callbacks in for InitialSyncer to run to
    // completion.
    executor::NetworkInterfaceMock::InNetworkGuard(getNet())->runReadyNetworkOperations();

    initialSyncer->join();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerRetriesSyncSourceSelectionIfChooseNewSyncSourceReturnsInvalidSyncSource) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // Override chooseNewSyncSource() result in SyncSourceSelectorMock before calling startup()
    // because InitialSyncer will look for a valid sync source immediately after startup.
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort());

    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    // Run first sync source selection attempt.
    executor::NetworkInterfaceMock::InNetworkGuard(getNet())->runReadyNetworkOperations();

    // InitialSyncer will not drop user databases while looking for a valid sync source.
    ASSERT_FALSE(_storageInterfaceWorkDone.droppedUserDBs);

    // First sync source selection attempt failed. Update SyncSourceSelectorMock to return valid
    // sync source next time chooseNewSyncSource() is called.
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));

    // Advance clock until the next sync source selection attempt.
    advanceClock(getNet(), _options.syncSourceRetryWait);

    // DataReplictor drops user databases after obtaining a valid sync source.
    ASSERT_TRUE(_storageInterfaceWorkDone.droppedUserDBs);
}

const std::uint32_t chooseSyncSourceMaxAttempts = 10U;

/**
 * Advances executor clock so that InitialSyncer exhausts all 'chooseSyncSourceMaxAttempts' (server
 * parameter numInitialSyncConnectAttempts) sync source selection attempts.
 * If SyncSourceSelectorMock keeps returning an invalid sync source, InitialSyncer will retry every
 * '_options.syncSourceRetryWait' ms up to a maximum of 'chooseSyncSourceMaxAttempts' attempts.
 */
void _simulateChooseSyncSourceFailure(executor::NetworkInterfaceMock* net,
                                      Milliseconds syncSourceRetryWait) {
    advanceClock(net, int(chooseSyncSourceMaxAttempts - 1) * syncSourceRetryWait);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerReturnsInitialSyncOplogSourceMissingIfNoValidSyncSourceCanBeFoundAfterTenFailedChooseSyncSourceAttempts) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // Override chooseNewSyncSource() result in SyncSourceSelectorMock before calling startup()
    // because InitialSyncer will look for a valid sync source immediately after startup.
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort());

    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    _simulateChooseSyncSourceFailure(getNet(), _options.syncSourceRetryWait);

    initialSyncer->join();

    ASSERT_EQUALS(ErrorCodes::InitialSyncOplogSourceMissing, _lastApplied);
}

// Confirms that InitialSyncer keeps retrying initial sync.
// Make every initial sync attempt fail early by having the sync source selector always return an
// invalid sync source.
TEST_F(InitialSyncerTest,
       InitialSyncerRetriesInitialSyncUpToMaxAttemptsAndReturnsLastAttemptError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort());

    const std::uint32_t initialSyncMaxAttempts = 3U;
    ASSERT_OK(initialSyncer->startup(opCtx.get(), initialSyncMaxAttempts));

    auto net = getNet();
    for (std::uint32_t i = 0; i < initialSyncMaxAttempts; ++i) {
        _simulateChooseSyncSourceFailure(net, _options.syncSourceRetryWait);
        advanceClock(net, _options.initialSyncRetryWait);
    }

    initialSyncer->join();

    ASSERT_EQUALS(ErrorCodes::InitialSyncOplogSourceMissing, _lastApplied);

    // Check number of failed attempts in stats.
    auto progress = initialSyncer->getInitialSyncProgress();
    unittest::log() << "Progress after " << initialSyncMaxAttempts
                    << " failed attempts: " << progress;
    ASSERT_EQUALS(progress.getIntField("failedInitialSyncAttempts"), int(initialSyncMaxAttempts))
        << progress;
    ASSERT_EQUALS(progress.getIntField("maxFailedInitialSyncAttempts"), int(initialSyncMaxAttempts))
        << progress;
}

TEST_F(InitialSyncerTest, InitialSyncerResetsOptimesOnNewAttempt) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort());

    // Set the last optime to an arbitrary nonzero value. The value of the 'consistency' argument
    // doesn't matter. Also set last wall time to an arbitrary non-minimum value.
    auto origOptime = OpTime(Timestamp(1000, 1), 1);
    _setMyLastOptime({origOptime, Date_t::max()},
                     ReplicationCoordinator::DataConsistency::Inconsistent);

    // Start initial sync.
    const std::uint32_t initialSyncMaxAttempts = 1U;
    ASSERT_OK(initialSyncer->startup(opCtx.get(), initialSyncMaxAttempts));

    auto net = getNet();

    // Simulate a failed initial sync attempt
    _simulateChooseSyncSourceFailure(net, _options.syncSourceRetryWait);
    advanceClock(net, _options.initialSyncRetryWait);

    initialSyncer->join();

    // Make sure the initial sync attempt reset optimes.
    ASSERT_EQUALS(OpTime(), _options.getMyLastOptime());
    ASSERT_EQUALS(Date_t(), initialSyncer->getWallClockTime_forTest());
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsCallbackCanceledIfShutdownWhileRetryingSyncSourceSelection) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort());
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        auto when = net->now() + _options.syncSourceRetryWait / 2;
        ASSERT_GREATER_THAN(when, net->now());
        ASSERT_EQUALS(when, net->runUntil(when));
    }

    // This will cancel the _chooseSyncSourceCallback() task scheduled at getNet()->now() +
    // '_options.syncSourceRetryWait'.
    ASSERT_OK(initialSyncer->shutdown());

    initialSyncer->join();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsScheduleErrorIfTaskExecutorFailsToScheduleNextChooseSyncSourceCallback) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort());
    _executorProxy->shouldFailScheduleWorkAtRequest = []() { return true; };
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    initialSyncer->join();

    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsScheduleErrorIfTaskExecutorFailsToScheduleNextInitialSyncAttempt) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort());

    ASSERT_EQUALS(InitialSyncer::State::kPreStart, initialSyncer->getState_forTest());

    ASSERT_OK(initialSyncer->startup(opCtx.get(), 2U));
    ASSERT_EQUALS(InitialSyncer::State::kRunning, initialSyncer->getState_forTest());

    // Advance clock so that we run all but the last sync source callback.
    auto net = getNet();
    advanceClock(net, int(chooseSyncSourceMaxAttempts - 2) * _options.syncSourceRetryWait);

    // Last choose sync source attempt should now be scheduled. Advance clock so we fail last
    // choose sync source attempt which cause the next initial sync attempt to be scheduled.
    _executorProxy->shouldFailScheduleWorkAtRequest = []() { return true; };
    advanceClock(net, _options.syncSourceRetryWait);

    initialSyncer->join();

    ASSERT_EQUALS(InitialSyncer::State::kComplete, initialSyncer->getState_forTest());
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

// This test verifies that the initial syncer will still transition to a complete state even if
// the completion callback function throws an exception.
TEST_F(InitialSyncerTest, InitialSyncerTransitionsToCompleteWhenFinishCallbackThrowsException) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _onCompletion = [this](const StatusWith<OpTimeAndWallTime>& lastApplied) {
        _lastApplied = lastApplied;
        uassert(ErrorCodes::InternalError, "", false);
    };

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort());
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    ASSERT_OK(initialSyncer->shutdown());
    initialSyncer->join();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

class SharedCallbackState {
    SharedCallbackState(const SharedCallbackState&) = delete;
    SharedCallbackState& operator=(const SharedCallbackState&) = delete;

public:
    explicit SharedCallbackState(bool* sharedCallbackStateDestroyed)
        : _sharedCallbackStateDestroyed(sharedCallbackStateDestroyed) {}
    ~SharedCallbackState() {
        *_sharedCallbackStateDestroyed = true;
    }

private:
    bool* _sharedCallbackStateDestroyed;
};

TEST_F(InitialSyncerTest, InitialSyncerResetsOnCompletionCallbackFunctionPointerUponCompletion) {
    bool sharedCallbackStateDestroyed = false;
    auto sharedCallbackData = std::make_shared<SharedCallbackState>(&sharedCallbackStateDestroyed);
    decltype(_lastApplied) lastApplied = getDetectableErrorStatus();

    auto dataReplicatorExternalState = std::make_unique<DataReplicatorExternalStateMock>();
    dataReplicatorExternalState->taskExecutor = &getExecutor();
    auto initialSyncer = std::make_unique<InitialSyncer>(
        _options,
        std::move(dataReplicatorExternalState),
        _dbWorkThreadPool.get(),
        _storageInterface.get(),
        _replicationProcess.get(),
        [&lastApplied, sharedCallbackData](const StatusWith<OpTimeAndWallTime>& result) {
            lastApplied = result;
        });
    ON_BLOCK_EXIT([this]() { getExecutor().shutdown(); });

    auto opCtx = makeOpCtx();

    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    sharedCallbackData.reset();
    ASSERT_FALSE(sharedCallbackStateDestroyed);

    ASSERT_OK(initialSyncer->shutdown());

    // Depending on which InitialSyncer stage (_chooseSyncSource or _rollbackCheckerResetCallback)
    // was interrupted by shutdown(), we may have to request the network interface to deliver
    // cancellation signals to the InitialSyncer callbacks in for InitialSyncer to run to
    // completion.
    executor::NetworkInterfaceMock::InNetworkGuard(getNet())->runReadyNetworkOperations();

    initialSyncer->join();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, lastApplied.getStatus());

    // InitialSyncer should reset 'InitialSyncer::_onCompletion' after running callback function
    // for the last time before becoming inactive.
    // This ensures that we release resources associated with 'InitialSyncer::_onCompletion'.
    ASSERT_TRUE(sharedCallbackStateDestroyed);
}

TEST_F(InitialSyncerTest, InitialSyncerTruncatesOplogAndDropsReplicatedDatabases) {
    // We are not interested in proceeding beyond the dropUserDB stage so we inject a failure
    // after setting '_storageInterfaceWorkDone.droppedUserDBs' to true.
    auto oldDropUserDBsFn = _storageInterface->dropUserDBsFn;
    _storageInterface->dropUserDBsFn = [oldDropUserDBsFn](OperationContext* opCtx) {
        ASSERT_OK(oldDropUserDBsFn(opCtx));
        return Status(ErrorCodes::OperationFailed, "drop userdbs failed");
    };

    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);

    LockGuard lock(_storageInterfaceWorkDoneMutex);
    ASSERT_TRUE(_storageInterfaceWorkDone.truncateCalled);
    ASSERT_TRUE(_storageInterfaceWorkDone.droppedUserDBs);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughGetRollbackIdScheduleError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // replSetGetRBID is the first remote command to be scheduled by the initial syncer after
    // creating the oplog collection.
    executor::RemoteCommandRequest request;
    _executorProxy->shouldFailScheduleRemoteCommandRequest =
        [&request](const executor::RemoteCommandRequestOnAny& requestToSend) {
            request = {requestToSend, 0};
            return true;
        };

    HostAndPort syncSource("localhost", 12345);
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(syncSource);
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);

    ASSERT_EQUALS("admin", request.dbname);
    assertRemoteCommandNameEquals("replSetGetRBID", request);
    ASSERT_EQUALS(syncSource, request.target);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerReturnsShutdownInProgressIfSchedulingRollbackCheckerFailedDueToExecutorShutdown) {
    // The rollback id request is sent immediately after oplog truncation. We shut the task executor
    // down before returning from truncate() to make the scheduleRemoteCommand() call for
    // replSetGetRBID fail.
    auto oldTruncateCollFn = _storageInterface->truncateCollFn;
    _storageInterface->truncateCollFn = [oldTruncateCollFn, this](OperationContext* opCtx,
                                                                  const NamespaceString& nss) {
        auto status = oldTruncateCollFn(opCtx, nss);
        getExecutor().shutdown();
        return status;
    };

    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, _lastApplied);

    LockGuard lock(_storageInterfaceWorkDoneMutex);
    ASSERT_TRUE(_storageInterfaceWorkDone.truncateCalled);
}

TEST_F(InitialSyncerTest, InitialSyncerCancelsRollbackCheckerOnShutdown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    HostAndPort syncSource("localhost", 12345);
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(syncSource);

    ASSERT_EQUALS(InitialSyncer::State::kPreStart, initialSyncer->getState_forTest());

    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));
    ASSERT_EQUALS(InitialSyncer::State::kRunning, initialSyncer->getState_forTest());

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        ASSERT_TRUE(net->hasReadyRequests());
        auto noi = net->getNextReadyRequest();
        const auto& request = assertRemoteCommandNameEquals("replSetGetRBID", noi->getRequest());
        ASSERT_EQUALS("admin", request.dbname);
        ASSERT_EQUALS(syncSource, request.target);
        net->blackHole(noi);
    }

    ASSERT_OK(initialSyncer->shutdown());
    // Since we need to request the NetworkInterfaceMock to deliver the cancellation event,
    // the InitialSyncer has to be in a pre-completion state (ie. ShuttingDown).
    ASSERT_EQUALS(InitialSyncer::State::kShuttingDown, initialSyncer->getState_forTest());

    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
    ASSERT_EQUALS(InitialSyncer::State::kComplete, initialSyncer->getState_forTest());

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughRollbackCheckerCallbackError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        assertRemoteCommandNameEquals(
            "replSetGetRBID",
            net->scheduleErrorResponse(
                Status(ErrorCodes::OperationFailed, "replSetGetRBID failed at sync source")));
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughDefaultBeginFetchingOpTimeScheduleError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // We reject the 'find' command on the oplog and save the request for inspection at the end of
    // this test case.
    executor::RemoteCommandRequest request;
    _executorProxy->shouldFailScheduleRemoteCommandRequest =
        [&request](const executor::RemoteCommandRequestOnAny& requestToSend) {
            request = {requestToSend, 0};
            auto elem = requestToSend.cmdObj.firstElement();
            return (("find" == elem.fieldNameStringData()) &&
                    ("oplog.rs" == elem.valueStringData()));
        };

    HostAndPort syncSource("localhost", 12345);
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(syncSource);
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);

    ASSERT_EQUALS(syncSource, request.target);
    ASSERT_EQUALS(NamespaceString::kLocalDb, request.dbname);
    assertRemoteCommandNameEquals("find", request);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughDefaultBeginFetchingOpTimeCallbackError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        assertRemoteCommandNameEquals(
            "find",
            net->scheduleErrorResponse(
                Status(ErrorCodes::OperationFailed, "find command failed at sync source")));
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerCancelsDefaultBeginFetchingOpTimeFetcherOnShutdown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();
    }

    ASSERT_OK(initialSyncer->shutdown());
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied.getStatus());
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughGetBeginFetchingOpTimeScheduleError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // We reject the 'find' command for the begin fetching optime and save the request for
    // inspection at the end of this test case.
    executor::RemoteCommandRequest request;
    _executorProxy->shouldFailScheduleRemoteCommandRequest =
        [&request](const executor::RemoteCommandRequestOnAny& requestToSend) {
            request = {requestToSend, 0};
            auto elem = requestToSend.cmdObj.firstElement();
            return (("find" == elem.fieldNameStringData()) &&
                    (NamespaceString::kSessionTransactionsTableNamespace.coll() ==
                     elem.valueStringData()));
        };

    HostAndPort syncSource("localhost", 12345);
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(syncSource);
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);

    ASSERT_EQUALS(syncSource, request.target);
    ASSERT_EQUALS(NamespaceString::kConfigDb, request.dbname);
    assertRemoteCommandNameEquals("find", request);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughGetBeginFetchingOpTimeCallbackError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        assertRemoteCommandNameEquals(
            "find",
            net->scheduleErrorResponse(
                Status(ErrorCodes::OperationFailed, "find command failed at sync source")));
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerCancelsBeginFetchingOpTimeFetcherOnShutdown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
    }

    ASSERT_OK(initialSyncer->shutdown());
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied.getStatus());
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughLastOplogEntryFetcherScheduleError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // We reject the 'find' command on the oplog and save the request for inspection at the end of
    // this test case.
    executor::RemoteCommandRequest request;

    HostAndPort syncSource("localhost", 12345);
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(syncSource);
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        _executorProxy->shouldFailScheduleRemoteCommandRequest =
            [&request](const executor::RemoteCommandRequestOnAny& requestToSend) {
                request = {requestToSend, 0};
                auto elem = requestToSend.cmdObj.firstElement();
                return (("find" == elem.fieldNameStringData()) &&
                        ("oplog.rs" == elem.valueStringData()));
            };

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);

    ASSERT_EQUALS(syncSource, request.target);
    ASSERT_EQUALS(_options.localOplogNS.db(), request.dbname);
    assertRemoteCommandNameEquals("find", request);
    ASSERT_BSONOBJ_EQ(BSON("$natural" << -1), request.cmdObj.getObjectField("sort"));
    ASSERT_EQUALS(1, request.cmdObj.getIntField("limit"));
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughLastOplogEntryFetcherCallbackError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        assertRemoteCommandNameEquals(
            "find",
            net->scheduleErrorResponse(
                Status(ErrorCodes::OperationFailed, "find command failed at sync source")));
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerCancelsLastOplogEntryFetcherOnShutdown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        ASSERT_TRUE(net->hasReadyRequests());
    }

    ASSERT_OK(initialSyncer->shutdown());
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsNoMatchingDocumentIfLastOplogEntryFetcherReturnsEmptyBatchOfDocuments) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({});
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerResendsFindCommandIfLastOplogEntryFetcherReturnsRetriableError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);

    // Base rollback ID.
    net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

    // Oplog entry associated with the defaultBeginFetchingTimestamp.
    processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

    // Send an empty optime as the response to the beginFetchingOptime find request, which will
    // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
    auto request = net->scheduleSuccessfulResponse(
        makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
    assertRemoteCommandNameEquals("find", request);
    net->runReadyNetworkOperations();

    // Last oplog entry first attempt - retriable error.
    assertRemoteCommandNameEquals(
        "find", net->scheduleErrorResponse(Status(ErrorCodes::HostUnreachable, "")));
    net->runReadyNetworkOperations();

    // InitialSyncer stays active because it resends the find request for the last oplog entry.
    ASSERT_TRUE(initialSyncer->isActive());

    // Last oplog entry second attempt.
    processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsNoSuchKeyIfLastOplogEntryFetcherReturnsEntryWithMissingTimestamp) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({BSONObj()});
    }

    initialSyncer->join();

    // OpTimeAndWallTime now uses the IDL parser, so the status code returned is from
    // IDLParserErrorContext
    ASSERT_EQUALS(_lastApplied.getStatus().code(), 40414);
}

TEST_F(InitialSyncerTest,
       InitialSyncerPassesThroughErrorFromDataReplicatorExternalStateGetCurrentConfig) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    getExternalState()->replSetConfigResult = Status(ErrorCodes::OperationFailed, "");

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Feature Compatibility Version.
        processSuccessfulFCVFetcherResponseLastStable();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughFCVFetcherScheduleError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // We reject the first find command that is on the fCV collection.
    executor::RemoteCommandRequest request;
    _executorProxy->shouldFailScheduleRemoteCommandRequest =
        [&request](const executor::RemoteCommandRequestOnAny& requestToSend) {
            request = {requestToSend, 0};
            return "find" == requestToSend.cmdObj.firstElement().fieldNameStringData() &&
                NamespaceString::kServerConfigurationNamespace.coll() ==
                requestToSend.cmdObj.firstElement().str();
        };

    HostAndPort syncSource("localhost", 12345);
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(syncSource);
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);

    ASSERT_EQUALS(syncSource, request.target);
    assertFCVRequest(request);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughFCVFetcherCallbackError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        request = assertRemoteCommandNameEquals(
            "find",
            net->scheduleErrorResponse(
                Status(ErrorCodes::OperationFailed, "find command failed at sync source")));
        assertFCVRequest(request);
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerCancelsFCVFetcherOnShutdown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        ASSERT_TRUE(net->hasReadyRequests());
    }

    ASSERT_OK(initialSyncer->shutdown());
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerResendsFindCommandIfFCVFetcherReturnsRetriableError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);

    // Base rollback ID.
    net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

    // Oplog entry associated with the defaultBeginFetchingTimestamp.
    processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

    // Send an empty optime as the response to the beginFetchingOptime find request, which will
    // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
    auto request = net->scheduleSuccessfulResponse(
        makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
    assertRemoteCommandNameEquals("find", request);
    net->runReadyNetworkOperations();

    // Oplog entry associated with the beginApplyingTimestamp.
    processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

    // FCV first attempt - retriable error.
    assertRemoteCommandNameEquals(
        "find", net->scheduleErrorResponse(Status(ErrorCodes::HostUnreachable, "")));
    net->runReadyNetworkOperations();

    // InitialSyncer stays active because it resends the find request for the fCV.
    ASSERT_TRUE(initialSyncer->isActive());

    // FCV second attempt.
    processSuccessfulFCVFetcherResponseLastStable();
}

void InitialSyncerTest::runInitialSyncWithBadFCVResponse(std::vector<BSONObj> docs,
                                                         ErrorCodes::Error expectedError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        processSuccessfulFCVFetcherResponse(docs);
    }

    initialSyncer->join();
    ASSERT_EQUALS(expectedError, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsIncompatibleServerVersionWhenFCVFetcherReturnsEmptyBatchOfDocuments) {
    runInitialSyncWithBadFCVResponse({}, ErrorCodes::IncompatibleServerVersion);
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsTooManyMatchingDocumentsWhenFCVFetcherReturnsMultipleDocuments) {
    auto docs = {BSON("_id" << FeatureCompatibilityVersionParser::kParameterName << "version"
                            << FeatureCompatibilityVersionParser::kVersion42),
                 BSON("_id"
                      << "other")};
    runInitialSyncWithBadFCVResponse(docs, ErrorCodes::TooManyMatchingDocuments);
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsIncompatibleServerVersionWhenFCVFetcherReturnsUpgradeTargetVersion) {
    auto docs = {BSON("_id" << FeatureCompatibilityVersionParser::kParameterName << "version"
                            << FeatureCompatibilityVersionParser::kVersion42 << "targetVersion"
                            << FeatureCompatibilityVersionParser::kVersion44)};
    runInitialSyncWithBadFCVResponse(docs, ErrorCodes::IncompatibleServerVersion);
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsIncompatibleServerVersionWhenFCVFetcherReturnsDowngradeTargetVersion) {
    auto docs = {BSON("_id" << FeatureCompatibilityVersionParser::kParameterName << "version"
                            << FeatureCompatibilityVersionParser::kVersion42 << "targetVersion"
                            << FeatureCompatibilityVersionParser::kVersion42)};
    runInitialSyncWithBadFCVResponse(docs, ErrorCodes::IncompatibleServerVersion);
}

TEST_F(InitialSyncerTest, InitialSyncerReturnsBadValueWhenFCVFetcherReturnsNoVersion) {
    auto docs = {BSON("_id" << FeatureCompatibilityVersionParser::kParameterName << "targetVersion"
                            << FeatureCompatibilityVersionParser::kVersion42)};
    runInitialSyncWithBadFCVResponse(docs, ErrorCodes::BadValue);
}

TEST_F(InitialSyncerTest, InitialSyncerSucceedsWhenFCVFetcherReturnsOldVersion) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        auto docs = {BSON("_id" << FeatureCompatibilityVersionParser::kParameterName << "version"
                                << FeatureCompatibilityVersionParser::kVersion42)};
        processSuccessfulFCVFetcherResponse(docs);
        ASSERT_TRUE(net->hasReadyRequests());
    }

    // We shut it down so we do not have to finish initial sync. If the fCV fetcher got an error,
    // we would return that.
    ASSERT_OK(initialSyncer->shutdown());
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughOplogFetcherScheduleError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // Make the tailable oplog query fail. Allow all other requests to be scheduled.
    executor::RemoteCommandRequest request;
    _executorProxy->shouldFailScheduleRemoteCommandRequest =
        [&request](const executor::RemoteCommandRequestOnAny& requestToSend) {
            if ("find" == requestToSend.cmdObj.firstElement().fieldNameStringData() &&
                requestToSend.cmdObj.getBoolField("tailable")) {
                request = {requestToSend, 0};
                return true;
            }
            return false;
        };

    HostAndPort syncSource("localhost", 12345);
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(syncSource);
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        // Keep the cloner from finishing so end-of-clone-stage network events don't interfere.
        FailPointEnableBlock clonerFailpoint("hangBeforeClonerStage", kListDatabasesFailPointData);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});
        net->runReadyNetworkOperations();

        // Feature Compatibility Version.
        processSuccessfulFCVFetcherResponseLastStable();

        // OplogFetcher will shut down AllDatabaseCloner on error after setting the completion
        // status.  We call runReadyNetworkOperations() again to deliver the cancellation status to
        // __allDatabaseClonerCallback().
        net->runReadyNetworkOperations();
    }
    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);

    ASSERT_EQUALS(syncSource, request.target);
    ASSERT_EQUALS(_options.localOplogNS.db(), request.dbname);
    assertRemoteCommandNameEquals("find", request);
    ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
    ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughOplogFetcherCallbackError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        // Keep the cloner from finishing so end-of-clone-stage network events don't interfere.
        FailPointEnableBlock clonerFailpoint("hangBeforeClonerStage", kListDatabasesFailPointData);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, _options.localOplogNS, {makeOplogEntryObj(1)}));
        net->runReadyNetworkOperations();

        // Feature Compatibility Version.
        processSuccessfulFCVFetcherResponseLastStable();

        // Oplog tailing query.
        request = assertRemoteCommandNameEquals(
            "find", net->scheduleErrorResponse(Status(ErrorCodes::OperationFailed, "dead cursor")));
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerSucceedsOnEarlyOplogFetcherCompletionIfThereAreNoOperationsToApply) {
    // Skip reconstructing prepared transactions at the end of initial sync because
    // InitialSyncerTest does not construct ServiceEntryPoint and this causes a segmentation fault
    // when reconstructPreparedTransactions uses DBDirectClient to call into ServiceEntryPoint.
    FailPointEnableBlock skipReconstructPreparedTransactions("skipReconstructPreparedTransactions");

    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        request =
            assertRemoteCommandNameEquals("find",
                                          net->scheduleSuccessfulResponse(makeCursorResponse(
                                              0LL, _options.localOplogNS, {makeOplogEntryObj(1)})));
        ASSERT_EQUALS(1, request.cmdObj.getIntField("limit"));
        net->runReadyNetworkOperations();

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // Oplog tailing query.
            // Simulate cursor closing on sync source.
            request = assertRemoteCommandNameEquals(
                "find",
                net->scheduleSuccessfulResponse(
                    makeCursorResponse(0LL, _options.localOplogNS, {makeOplogEntryObj(1)})));
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            net->runReadyNetworkOperations();
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Last rollback checker replSetGetRBID command.
        assertRemoteCommandNameEquals(
            "replSetGetRBID", net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1)));
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_OK(_lastApplied.getStatus());
    auto dummyEntry = makeOplogEntry(1);
    ASSERT_EQUALS(dummyEntry.getOpTime(), _lastApplied.getValue().opTime);
    ASSERT_EQUALS(dummyEntry.getWallClockTime(), _lastApplied.getValue().wallTime);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerSucceedsOnEarlyOplogFetcherCompletionIfThereAreEnoughOperationsInTheOplogBufferToReachEndTimestamp) {
    // Skip reconstructing prepared transactions at the end of initial sync because
    // InitialSyncerTest does not construct ServiceEntryPoint and this causes a segmentation fault
    // when reconstructPreparedTransactions uses DBDirectClient to call into ServiceEntryPoint.
    FailPointEnableBlock skipReconstructPreparedTransactions("skipReconstructPreparedTransactions");

    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // Oplog tailing query.
            // Simulate cursor closing on sync source.
            request =
                assertRemoteCommandNameEquals("find",
                                              net->scheduleSuccessfulResponse(makeCursorResponse(
                                                  0LL,
                                                  _options.localOplogNS,
                                                  {makeOplogEntryObj(1),
                                                   makeOplogEntryObj(2, OpTypeEnum::kCommand),
                                                   makeOplogEntryObj(3, OpTypeEnum::kCommand)})));
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            net->runReadyNetworkOperations();
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(3)});

        // Last rollback checker replSetGetRBID command.
        assertRemoteCommandNameEquals(
            "replSetGetRBID", net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1)));
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_OK(_lastApplied.getStatus());
    auto dummyEntry = makeOplogEntry(3);
    ASSERT_EQUALS(dummyEntry.getOpTime(), _lastApplied.getValue().opTime);
    ASSERT_EQUALS(dummyEntry.getWallClockTime(), _lastApplied.getValue().wallTime);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerReturnsRemoteResultsUnavailableOnEarlyOplogFetcherCompletionIfThereAreNotEnoughOperationsInTheOplogBufferToReachEndTimestamp) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // Oplog tailing query.
            // Simulate cursor closing on sync source.
            request =
                assertRemoteCommandNameEquals("find",
                                              net->scheduleSuccessfulResponse(makeCursorResponse(
                                                  0LL,
                                                  _options.localOplogNS,
                                                  {makeOplogEntryObj(1),
                                                   makeOplogEntryObj(2, OpTypeEnum::kCommand),
                                                   makeOplogEntryObj(3, OpTypeEnum::kCommand)})));
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            net->runReadyNetworkOperations();
        }

        // Oplog entry associated with the stopTimestamp.
        // Return an oplog entry with an optime that is more recent than what the completed
        // OplogFetcher has read from the sync source.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(4)});
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::RemoteResultsUnavailable, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerPassesThroughAllDatabaseClonerCallbackErrorAndCancelsOplogFetcher) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // Make the initial listDatabases reply an error.
    _mockServer->setCommandReply("listDatabases",
                                 Status(ErrorCodes::FailedToParse, "listDatabases failed"));

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Feature Compatibility Version.
        processSuccessfulFCVFetcherResponseLastStable();

        // Oplog tailing query.
        // Simulate cursor remaining open.
        // The cloner and oplog fetcher don't run in lockstep, so we run the oplog fetcher in
        // an infinite loop here, and allow the cancel to cancel the loop by making
        // "hasReadyRequests" false.
        int oplogEntry = 1;
        while (net->hasReadyRequests()) {
            if (oplogEntry == 1) {
                request = assertRemoteCommandNameEquals(
                    "find",
                    net->scheduleSuccessfulResponse(makeCursorResponse(
                        1LL, _options.localOplogNS, {makeOplogEntryObj(oplogEntry++)})));
                ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
                net->runReadyNetworkOperations();
            } else {
                request = assertRemoteCommandNameEquals(
                    "getMore",
                    net->scheduleSuccessfulResponse(makeCursorResponse(
                        1LL, _options.localOplogNS, {makeOplogEntryObj(oplogEntry++)}, false)));
                net->runReadyNetworkOperations();
            }
        }
        // We call runReadyNetworkOperations() again to deliver the cancellation status to the
        // _oplogFetcherCallback()
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::FailedToParse, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerCancelsBothOplogFetcherAndAllDatabaseClonerOnShutdown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Feature Compatibility Version.
        processSuccessfulFCVFetcherResponseLastStable();
    }

    ASSERT_OK(initialSyncer->shutdown());
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerPassesThroughSecondLastOplogEntryFetcherScheduleErrorAndCancelsOplogFetcher) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // Make the third last oplog entry fetcher command fail. Allow all other requests to be
    // scheduled.
    executor::RemoteCommandRequest request;
    int count = 0;
    _executorProxy->shouldFailScheduleRemoteCommandRequest =
        [&count, &request](const executor::RemoteCommandRequestOnAny& requestToSend) {
            auto elem = requestToSend.cmdObj.firstElement();
            if (("find" == elem.fieldNameStringData()) && (requestToSend.cmdObj.hasField("sort")) &&
                (1 == requestToSend.cmdObj.getIntField("limit")) &&
                (NamespaceString::kRsOplogNamespace.coll().toString() == elem.valueStringData())) {
                if (count < 2) {
                    count++;
                    return false;
                }
                request = {requestToSend, 0};
                return true;
            }
            return false;
        };

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Feature Compatibility Version.
        processSuccessfulFCVFetcherResponseLastStable();

        // AllDatabaseCloner will shut down the OplogFetcher on failing to schedule the last entry
        // oplog fetcher after setting the completion status.  Run the oplog fetcher until
        // that happens.
        int oplogEntry = 1;
        while (net->hasReadyRequests()) {
            net->scheduleSuccessfulResponse(makeCursorResponse(
                1LL, _options.localOplogNS, {makeOplogEntryObj(oplogEntry)}, oplogEntry == 1));
            oplogEntry++;
            net->runReadyNetworkOperations();
        }

        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerPassesThroughSecondLastOplogEntryFetcherCallbackErrorAndCancelsOplogFetcher) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and
            // move on to the AllDatabaseCloner's request.
            auto noi = net->getNextReadyRequest();
            request = assertRemoteCommandNameEquals("find", noi->getRequest());
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            net->blackHole(noi);
        }

        // Oplog entry associated with the stopTimestamp.
        request = assertRemoteCommandNameEquals(
            "find",
            net->scheduleErrorResponse(
                Status(ErrorCodes::OperationFailed,
                       "Oplog entry fetcher associated with the stopTimestamp failed")));
        ASSERT_TRUE(request.cmdObj.hasField("sort"));
        ASSERT_EQUALS(1, request.cmdObj.getIntField("limit"));
        net->runReadyNetworkOperations();

        // _lastOplogEntryFetcherCallbackAfterCloningData() will shut down the OplogFetcher after
        // setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerCancelsBothSecondLastOplogEntryFetcherAndOplogFetcherOnShutdown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and
            // move on to the AllDatabaseCloner's request.
            auto noi = net->getNextReadyRequest();
            request = assertRemoteCommandNameEquals("find", noi->getRequest());
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            net->blackHole(noi);
        }

        // Oplog entry associated with the stopTimestamp.
        auto noi = net->getNextReadyRequest();
        request = assertRemoteCommandNameEquals("find", noi->getRequest());
        ASSERT_TRUE(request.cmdObj.hasField("sort"));
        ASSERT_EQUALS(1, request.cmdObj.getIntField("limit"));
        net->blackHole(noi);
    }

    initialSyncer->shutdown().transitional_ignore();
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerCancelsSecondLastOplogEntryFetcherOnOplogFetcherCallbackError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        NetworkInterfaceMock::NetworkOperationIterator oplogFetcherNetworkOperationIterator;
        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // Save request for OplogFetcher's oplog tailing query. This request will be canceled.
            auto noi = net->getNextReadyRequest();
            request = assertRemoteCommandNameEquals("find", noi->getRequest());
            ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            oplogFetcherNetworkOperationIterator = noi;
        }

        // Oplog entry associated with the stopTimestamp.
        // Blackhole this request which will be canceled when oplog fetcher fails.
        auto noi = net->getNextReadyRequest();
        request = assertRemoteCommandNameEquals("find", noi->getRequest());
        ASSERT_TRUE(request.cmdObj.hasField("sort"));
        ASSERT_EQUALS(1, request.cmdObj.getIntField("limit"));
        net->blackHole(noi);

        // Make oplog fetcher fail.
        net->scheduleErrorResponse(oplogFetcherNetworkOperationIterator,
                                   Status(ErrorCodes::OperationFailed, "oplog fetcher failed"));
        net->runReadyNetworkOperations();

        // _oplogFetcherCallback() will shut down the '_lastOplogEntryFetcher' after setting the
        // completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _lastOplogEntryFetcherCallbackAfterCloningData().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerReturnsTypeMismatchErrorWhenSecondLastOplogEntryFetcherReturnsMalformedDocument) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto oplogEntry = makeOplogEntryObj(1);
    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and
            // move on to the AllDatabaseCloner's request.
            auto noi = net->getNextReadyRequest();
            request = noi->getRequest();
            assertRemoteCommandNameEquals("find", request);
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            net->blackHole(noi);
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({BSON("ts"
                                                             << "not a timestamp"
                                                             << "t" << 1)});

        // _lastOplogEntryFetcherCallbackAfterCloningData() will shut down the OplogFetcher after
        // setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsOplogOutOfOrderIfStopTimestampPrecedesBeginTimestamp) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(2)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(2)});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and
            // move on to the AllDatabaseCloner's request.
            auto noi = net->getNextReadyRequest();
            request = assertRemoteCommandNameEquals("find", noi->getRequest());
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            net->blackHole(noi);
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // _lastOplogEntryFetcherCallbackAfterCloningData() will shut down the OplogFetcher after
        // setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OplogOutOfOrder, _lastApplied);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerPassesThroughInsertOplogSeedDocumentErrorAfterDataCloningFinishesWithNoOperationsToApply) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    NamespaceString insertDocumentNss;
    TimestampedBSONObj insertDocumentDoc;
    long long insertDocumentTerm;
    _storageInterface->insertDocumentFn =
        [&insertDocumentDoc, &insertDocumentNss, &insertDocumentTerm](
            OperationContext*,
            const NamespaceStringOrUUID& nsOrUUID,
            const TimestampedBSONObj& doc,
            long long term) {
            insertDocumentNss = *nsOrUUID.nss();
            insertDocumentDoc = doc;
            insertDocumentTerm = term;
            return Status(ErrorCodes::OperationFailed, "failed to insert oplog entry");
        };

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto oplogEntry = makeOplogEntryObj(1);
    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and
            // move on to the AllDatabaseCloner's request.
            auto noi = net->getNextReadyRequest();
            request = assertRemoteCommandNameEquals("find", noi->getRequest());
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            net->blackHole(noi);
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // _lastOplogEntryFetcherCallbackAfterCloningData() will shut down the OplogFetcher after
        // setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
    ASSERT_EQUALS(_options.localOplogNS, insertDocumentNss);
    ASSERT_BSONOBJ_EQ(oplogEntry, insertDocumentDoc.obj);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerReturnsCallbackCanceledAndDoesNotScheduleRollbackCheckerIfShutdownAfterInsertingInsertOplogSeedDocument) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    NamespaceString insertDocumentNss;
    TimestampedBSONObj insertDocumentDoc;
    long long insertDocumentTerm;
    _storageInterface->insertDocumentFn =
        [initialSyncer, &insertDocumentDoc, &insertDocumentNss, &insertDocumentTerm](
            OperationContext*,
            const NamespaceStringOrUUID& nsOrUUID,
            const TimestampedBSONObj& doc,
            long long term) {
            insertDocumentNss = *nsOrUUID.nss();
            insertDocumentDoc = doc;
            insertDocumentTerm = term;
            initialSyncer->shutdown().transitional_ignore();
            return Status::OK();
        };

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto oplogEntry = makeOplogEntryObj(1);
    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and
            // move on to the AllDatabaseCloner's request.
            auto noi = net->getNextReadyRequest();
            request = assertRemoteCommandNameEquals("find", noi->getRequest());
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            net->blackHole(noi);
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // _lastOplogEntryFetcherCallbackAfterCloningData() will shut down the OplogFetcher after
        // setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
    ASSERT_EQUALS(_options.localOplogNS, insertDocumentNss);
    ASSERT_BSONOBJ_EQ(oplogEntry, insertDocumentDoc.obj);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerPassesThroughRollbackCheckerScheduleErrorAfterCloningFinishesWithNoOperationsToApply) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // Make the second replSetGetRBID command fail. Allow all other requests to be scheduled.
    executor::RemoteCommandRequest request;
    bool first = true;
    _executorProxy->shouldFailScheduleRemoteCommandRequest =
        [&first, &request](const executor::RemoteCommandRequestOnAny& requestToSend) {
            if ("replSetGetRBID" == requestToSend.cmdObj.firstElement().fieldNameStringData()) {
                if (first) {
                    first = false;
                    return false;
                }
                request = {requestToSend, 0};
                return true;
            }
            return false;
        };

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto oplogEntry = makeOplogEntryObj(1);
    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and
            // move on to the AllDatabaseCloner's request.
            auto noi = net->getNextReadyRequest();
            request = assertRemoteCommandNameEquals("find", noi->getRequest());
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            net->blackHole(noi);
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // _lastOplogEntryFetcherCallbackAfterCloningData() will shut down the OplogFetcher after
        // setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerPassesThroughRollbackCheckerCallbackErrorAfterCloningFinishesWithNoOperationsToApply) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto oplogEntry = makeOplogEntryObj(1);
    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and
            // move on to the AllDatabaseCloner's request.
            auto noi = net->getNextReadyRequest();
            request = assertRemoteCommandNameEquals("find", noi->getRequest());
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            net->blackHole(noi);
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Last rollback checker replSetGetRBID command.
        assertRemoteCommandNameEquals(
            "replSetGetRBID",
            net->scheduleErrorResponse(
                Status(ErrorCodes::OperationFailed, "replSetGetRBID command failed")));
        net->runReadyNetworkOperations();

        // _rollbackCheckerCheckForRollbackCallback() will shut down the OplogFetcher after setting
        // the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerCancelsLastRollbackCheckerOnShutdown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto oplogEntry = makeOplogEntryObj(1);
    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and
            // move on to the AllDatabaseCloner's request.
            auto noi = net->getNextReadyRequest();
            request = assertRemoteCommandNameEquals("find", noi->getRequest());
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            net->blackHole(noi);
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Last rollback checker replSetGetRBID command.
        auto noi = net->getNextReadyRequest();
        assertRemoteCommandNameEquals("replSetGetRBID", noi->getRequest());
        net->blackHole(noi);

        // _rollbackCheckerCheckForRollbackCallback() will shut down the OplogFetcher after setting
        // the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    ASSERT_OK(initialSyncer->shutdown());
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerCancelsLastRollbackCheckerOnOplogFetcherCallbackError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto oplogEntry = makeOplogEntryObj(1);
    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        NetworkInterfaceMock::NetworkOperationIterator oplogFetcherNetworkOperationIterator;
        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // Save request for OplogFetcher's oplog tailing query. This request will be canceled.
            auto noi = net->getNextReadyRequest();
            request = assertRemoteCommandNameEquals("find", noi->getRequest());
            ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            oplogFetcherNetworkOperationIterator = noi;
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Last rollback checker replSetGetRBID command.
        auto noi = net->getNextReadyRequest();
        request = noi->getRequest();
        assertRemoteCommandNameEquals("replSetGetRBID", request);
        net->blackHole(noi);

        // Make oplog fetcher fail.
        net->scheduleErrorResponse(oplogFetcherNetworkOperationIterator,
                                   Status(ErrorCodes::OperationFailed, "oplog fetcher failed"));
        net->runReadyNetworkOperations();

        // _oplogFetcherCallback() will shut down the last rollback checker after setting the
        // completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _rollbackCheckerCheckForRollbackCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsUnrecoverableRollbackErrorIfSyncSourceRolledBackAfterCloningData) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto oplogEntry = makeOplogEntryObj(1);
    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and
            // move on to the AllDatabaseCloner's request.
            auto noi = net->getNextReadyRequest();
            request = noi->getRequest();
            assertRemoteCommandNameEquals("find", request);
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            net->blackHole(noi);
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Last rollback checker replSetGetRBID command.
        request = net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId + 1));
        net->runReadyNetworkOperations();
        assertRemoteCommandNameEquals("replSetGetRBID", request);
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, _lastApplied);
}

TEST_F(InitialSyncerTest, LastOpTimeShouldBeSetEvenIfNoOperationsAreAppliedAfterCloning) {
    // Skip reconstructing prepared transactions at the end of initial sync because
    // InitialSyncerTest does not construct ServiceEntryPoint and this causes a segmentation fault
    // when reconstructPreparedTransactions uses DBDirectClient to call into ServiceEntryPoint.
    FailPointEnableBlock skipReconstructPreparedTransactions("skipReconstructPreparedTransactions");

    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    ASSERT_TRUE(_replicationProcess->getConsistencyMarkers()->getInitialSyncFlag(opCtx.get()));

    auto oplogEntry = makeOplogEntry(1);
    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry.toBSON()});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry.toBSON()});

        // Instead of fast forwarding to AllDatabaseCloner completion by returning an empty list of
        // database names, we'll simulate copying a single database with a single collection on the
        // sync source.  We must do this setup before responding to the FCV, to avoid a race.
        NamespaceString nss("a.a");
        _mockServer->setCommandReply("listDatabases",
                                     makeListDatabasesResponse({nss.db().toString()}));

        // Set up data for "a"
        _mockServer->assignCollectionUuid(nss.ns(), *_options1.uuid);
        _mockServer->insert(nss.ns(), BSON("_id" << 1 << "a" << 1));

        // listCollections for "a"
        _mockServer->setCommandReply(
            "listCollections",
            makeCursorResponse(
                0LL /* cursorId */,
                nss,
                {BSON("name" << nss.coll() << "type"
                             << "collection"
                             << "options" << _options1.toBSON() << "info"
                             << BSON("readOnly" << false << "uuid" << *_options1.uuid))})
                .data);

        // count:a
        _mockServer->setCommandReply("count", BSON("n" << 1 << "ok" << 1));

        // listIndexes:a
        _mockServer->setCommandReply(
            "listIndexes",
            makeCursorResponse(
                0LL,
                NamespaceString(nss.getCommandNS()),
                {BSON("v" << OplogEntry::kOplogVersion << "key" << BSON("_id" << 1) << "name"
                          << "_id_"
                          << "ns" << nss.ns())})
                .data);

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and
            // move on to the AllDatabaseCloner's request.
            auto noi = net->getNextReadyRequest();
            request = noi->getRequest();
            assertRemoteCommandNameEquals("find", request);
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            net->blackHole(noi);
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry.toBSON()});

        // Last rollback checker replSetGetRBID command.
        request = assertRemoteCommandNameEquals(
            "replSetGetRBID",
            net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId)));
        net->runReadyNetworkOperations();

        // Deliver cancellation to OplogFetcher.
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_OK(_lastApplied.getStatus());
    ASSERT_EQUALS(oplogEntry.getOpTime(), _lastApplied.getValue().opTime);
    ASSERT_EQUALS(oplogEntry.getWallClockTime(), _lastApplied.getValue().wallTime);
    ASSERT_FALSE(_replicationProcess->getConsistencyMarkers()->getInitialSyncFlag(opCtx.get()));
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughGetNextApplierBatchScheduleError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    ASSERT_TRUE(_replicationProcess->getConsistencyMarkers()->getInitialSyncFlag(opCtx.get()));

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and
            // move on to the AllDatabaseCloner's request.
            auto noi = net->getNextReadyRequest();
            request = noi->getRequest();
            assertRemoteCommandNameEquals("find", request);
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            net->blackHole(noi);
        }

        // The cloners start right after the FCV is received. The oplog entry fetcher associated
        // with the stopTimestamp will not start until the cloners are done, so wait for them.
        getInitialSyncer().waitForCloner_forTest();

        // Before processing scheduled last oplog entry fetcher response, set flag in
        // TaskExecutorMock so that InitialSyncer will fail to schedule
        // _getNextApplierBatchCallback().
        _executorProxy->shouldFailScheduleWorkRequest = []() { return true; };

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(2)});

        // _lastOplogEntryFetcherCallbackAfterCloningData() will shut down the OplogFetcher after
        // setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughSecondGetNextApplierBatchScheduleError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    ASSERT_TRUE(_replicationProcess->getConsistencyMarkers()->getInitialSyncFlag(opCtx.get()));

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and
            // move on to the AllDatabaseCloner's request.
            auto noi = net->getNextReadyRequest();
            request = noi->getRequest();
            assertRemoteCommandNameEquals("find", request);
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            net->blackHole(noi);
        }

        // Before processing scheduled last oplog entry fetcher response, set flag in
        // TaskExecutorMock so that InitialSyncer will fail to schedule second
        // _getNextApplierBatchCallback() at (now + options.getApplierBatchCallbackRetryWait).
        _executorProxy->shouldFailScheduleWorkAtRequest = []() { return true; };

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(2)});

        // _lastOplogEntryFetcherCallbackAfterCloningData() will shut down the OplogFetcher after
        // setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerCancelsGetNextApplierBatchOnShutdown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    ASSERT_TRUE(_replicationProcess->getConsistencyMarkers()->getInitialSyncFlag(opCtx.get()));

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and
            // move on to the AllDatabaseCloner's request.
            auto noi = net->getNextReadyRequest();
            request = noi->getRequest();
            assertRemoteCommandNameEquals("find", request);
            ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
            net->blackHole(noi);
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(2)});

        // Since we black holed OplogFetcher's find request, _getNextApplierBatch_inlock() will
        // not return any operations for us to apply, leading to _getNextApplierBatchCallback()
        // rescheduling itself at new->now() + _options.getApplierBatchCallbackRetryWait.
    }

    ASSERT_OK(initialSyncer->shutdown());
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughGetNextApplierBatchInLockError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    ASSERT_TRUE(_replicationProcess->getConsistencyMarkers()->getInitialSyncFlag(opCtx.get()));

    // _getNextApplierBatch_inlock() returns BadValue when it gets an oplog entry with an unexpected
    // version (not OplogEntry::kOplogVersion).
    auto oplogEntry = makeOplogEntryObj(1);
    auto oplogEntryWithInconsistentVersion =
        makeOplogEntryObj(2, OpTypeEnum::kInsert, OplogEntry::kOplogVersion + 100);

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // OplogFetcher's oplog tailing query. Return bad oplog entry that will be added to the
            // oplog buffer and processed by _getNextApplierBatch_inlock().
            request = assertRemoteCommandNameEquals(
                "find",
                net->scheduleSuccessfulResponse(makeCursorResponse(
                    1LL, _options.localOplogNS, {oplogEntry, oplogEntryWithInconsistentVersion})));
            ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
            net->runReadyNetworkOperations();

            // Blackhole the getMore
            auto noi = net->getNextReadyRequest();
            request = noi->getRequest();
            assertRemoteCommandNameEquals("getMore", request);
            net->blackHole(noi);
            net->runReadyNetworkOperations();
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(2)});

        // _getNextApplierBatchCallback() will shut down the OplogFetcher after setting the
        // completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::BadValue, _lastApplied);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerReturnsEmptyBatchFromGetNextApplierBatchInLockIfRsSyncApplyStopFailPointIsEnabled) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    ASSERT_TRUE(_replicationProcess->getConsistencyMarkers()->getInitialSyncFlag(opCtx.get()));

    // _getNextApplierBatch_inlock() returns BadValue when it gets an oplog entry with an unexpected
    // version (not OplogEntry::kOplogVersion).
    auto oplogEntry = makeOplogEntryObj(1);
    auto oplogEntryWithInconsistentVersion =
        makeOplogEntryObj(2, OpTypeEnum::kInsert, OplogEntry::kOplogVersion + 100);

    // Enable 'rsSyncApplyStop' so that _getNextApplierBatch_inlock() returns an empty batch of
    // operations instead of a batch containing an oplog entry with a bad version.
    auto failPoint = globalFailPointRegistry().find("rsSyncApplyStop");
    failPoint->setMode(FailPoint::alwaysOn);
    ON_BLOCK_EXIT([failPoint]() { failPoint->setMode(FailPoint::off); });

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});
        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // OplogFetcher's oplog tailing query. Return bad oplog entry that will be added to the
            // oplog buffer and processed by _getNextApplierBatch_inlock().
            request = net->scheduleSuccessfulResponse(makeCursorResponse(
                1LL, _options.localOplogNS, {oplogEntry, oplogEntryWithInconsistentVersion}));
            assertRemoteCommandNameEquals("find", request);
            ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
            net->runReadyNetworkOperations();

            // Blackhole the getMore
            auto noi = net->getNextReadyRequest();
            request = noi->getRequest();
            assertRemoteCommandNameEquals("getMore", request);
            net->blackHole(noi);
            net->runReadyNetworkOperations();
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(2)});

        // Since the 'rsSyncApplyStop' fail point is enabled, InitialSyncer will get an empty
        // batch of operations from _getNextApplierBatch_inlock() even though the oplog buffer
        // is not empty.
    }

    // If the fail point is not working, the initial sync status will be set to BadValue (due to the
    // bad oplog entry in the oplog buffer) and shutdown() will not be able to overwrite this status
    // with CallbackCanceled.
    // Otherwise, shutdown() will cancel both the OplogFetcher and the scheduled
    // _getNextApplierBatchCallback() task. The final initial sync status will be CallbackCanceled.
    ASSERT_OK(initialSyncer->shutdown());
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughMultiApplierScheduleError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    ASSERT_TRUE(_replicationProcess->getConsistencyMarkers()->getInitialSyncFlag(opCtx.get()));

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        NetworkInterfaceMock::NetworkOperationIterator oplogFetcherNoi;
        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // OplogFetcher's oplog tailing query. Save for later.
            auto noi = net->getNextReadyRequest();
            request = noi->getRequest();
            assertRemoteCommandNameEquals("find", request);
            ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
            oplogFetcherNoi = noi;
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(2)});

        // _getNextApplierBatchCallback() should have rescheduled itself.
        // We'll insert some operations in the oplog buffer so that we'll attempt to schedule
        // MultiApplier next time _getNextApplierBatchCallback() runs.
        net->scheduleSuccessfulResponse(
            oplogFetcherNoi,
            makeCursorResponse(
                1LL, _options.localOplogNS, {makeOplogEntryObj(1), makeOplogEntryObj(2)}));
        net->runReadyNetworkOperations();

        // Ignore OplogFetcher's getMore request.
        auto noi = net->getNextReadyRequest();
        request = noi->getRequest();
        assertRemoteCommandNameEquals("getMore", request);

        // Make MultiApplier::startup() fail.
        _executorProxy->shouldFailScheduleWorkRequest = []() { return true; };

        // Advance clock until _getNextApplierBatchCallback() runs.
        auto when = net->now() + _options.getApplierBatchCallbackRetryWait;
        ASSERT_EQUALS(when, net->runUntil(when));

        // _getNextApplierBatchCallback() will shut down the OplogFetcher after setting the
        // completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughMultiApplierCallbackError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    getExternalState()->applyOplogBatchFn =
        [](OperationContext*, const std::vector<OplogEntry>&, OplogApplier::Observer*) {
            return Status(ErrorCodes::OperationFailed, "applyOplogBatch failed");
        };
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);

            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // OplogFetcher's oplog tailing query. Provide enough operations to trigger
            // MultiApplier.
            request = net->scheduleSuccessfulResponse(makeCursorResponse(
                1LL, _options.localOplogNS, {makeOplogEntryObj(1), makeOplogEntryObj(2)}));
            assertRemoteCommandNameEquals("find", request);
            ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
            net->runReadyNetworkOperations();

            // Black hole OplogFetcher's getMore request.
            auto noi = net->getNextReadyRequest();
            request = noi->getRequest();
            assertRemoteCommandNameEquals("getMore", request);
            net->blackHole(noi);
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(2)});

        // _multiApplierCallback() will shut down the OplogFetcher after setting the completion
        // status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerCancelsGetNextApplierBatchCallbackOnOplogFetcherError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        NetworkInterfaceMock::NetworkOperationIterator oplogFetcherNoi;
        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // OplogFetcher's oplog tailing query. Save for later.
            auto noi = net->getNextReadyRequest();
            request = noi->getRequest();
            assertRemoteCommandNameEquals("find", request);
            ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
            oplogFetcherNoi = noi;
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(2)});

        // Send error to _oplogFetcherCallback().
        net->scheduleErrorResponse(oplogFetcherNoi,
                                   Status(ErrorCodes::OperationFailed, "oplog fetcher failed"));

        // _oplogFetcherCallback() will cancel the _getNextApplierBatchCallback() task after setting
        // the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

OplogEntry InitialSyncerTest::doInitialSyncWithOneBatch() {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto lastOp = makeOplogEntry(2);

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // OplogFetcher's oplog tailing query. Response has enough operations to reach
            // end timestamp.
            request = net->scheduleSuccessfulResponse(makeCursorResponse(
                1LL, _options.localOplogNS, {makeOplogEntryObj(1), lastOp.toBSON()}));
            assertRemoteCommandNameEquals("find", request);
            ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
            net->runReadyNetworkOperations();

            // Black hole OplogFetcher's getMore request.
            auto noi = net->getNextReadyRequest();
            request = noi->getRequest();
            assertRemoteCommandNameEquals("getMore", request);
            net->blackHole(noi);
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({lastOp.toBSON()});

        request = net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        assertRemoteCommandNameEquals("replSetGetRBID", request);
        net->runReadyNetworkOperations();

        // _multiApplierCallback() will cancel the _getNextApplierBatchCallback() task after setting
        // the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    return lastOp;
}

void InitialSyncerTest::doSuccessfulInitialSyncWithOneBatch() {
    auto lastOp = doInitialSyncWithOneBatch();
    serverGlobalParams.featureCompatibility.reset();
    ASSERT_OK(_lastApplied.getStatus());
    ASSERT_EQUALS(lastOp.getOpTime(), _lastApplied.getValue().opTime);
    ASSERT_EQUALS(lastOp.getWallClockTime(), _lastApplied.getValue().wallTime);

    ASSERT_EQUALS(lastOp.getOpTime().getTimestamp(), _storageInterface->getInitialDataTimestamp());
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsLastAppliedOnReachingStopTimestampAfterApplyingOneBatch) {
    // Skip reconstructing prepared transactions at the end of initial sync because
    // InitialSyncerTest does not construct ServiceEntryPoint and this causes a segmentation fault
    // when reconstructPreparedTransactions uses DBDirectClient to call into ServiceEntryPoint.
    FailPointEnableBlock skipReconstructPreparedTransactions("skipReconstructPreparedTransactions");

    doSuccessfulInitialSyncWithOneBatch();
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsLastAppliedOnReachingStopTimestampAfterApplyingMultipleBatches) {
    // Skip reconstructing prepared transactions at the end of initial sync because
    // InitialSyncerTest does not construct ServiceEntryPoint and this causes a segmentation fault
    // when reconstructPreparedTransactions uses DBDirectClient to call into ServiceEntryPoint.
    FailPointEnableBlock skipReconstructPreparedTransactions("skipReconstructPreparedTransactions");

    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    // To make InitialSyncer apply multiple batches, we make the third and last operation a command
    // so that it will go into a separate batch from the second operation. First operation is the
    // last fetched entry before data cloning and is not applied.
    auto lastOp = makeOplogEntry(3, OpTypeEnum::kCommand);

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Instead of fast forwarding to AllDatabaseCloner completion by returning an empty list of
        // database names, we'll simulate copying a single database with a single collection on the
        // sync source.  We must do this setup before responding to the FCV, to avoid a race.
        NamespaceString nss("a.a");
        _mockServer->setCommandReply("listDatabases",
                                     makeListDatabasesResponse({nss.db().toString()}));


        // Set up data for "a"
        _mockServer->assignCollectionUuid(nss.ns(), *_options1.uuid);
        _mockServer->insert(nss.ns(), BSON("_id" << 1 << "a" << 1));

        // listCollections for "a"
        _mockServer->setCommandReply(
            "listCollections",
            makeCursorResponse(
                0LL,
                nss,
                {BSON("name" << nss.coll() << "type"
                             << "collection"
                             << "options" << _options1.toBSON() << "info"
                             << BSON("readOnly" << false << "uuid" << *_options1.uuid))})
                .data);

        // count:a
        _mockServer->setCommandReply("count", BSON("n" << 1 << "ok" << 1));

        // listIndexes:a
        _mockServer->setCommandReply(
            "listIndexes",
            makeCursorResponse(
                0LL,
                NamespaceString(nss.getCommandNS()),
                {BSON("v" << OplogEntry::kOplogVersion << "key" << BSON("_id" << 1) << "name"
                          << "_id_"
                          << "ns" << nss.ns())})
                .data);

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // OplogFetcher's oplog tailing query. Response has enough operations to reach
            // end timestamp.
            request = net->scheduleSuccessfulResponse(
                makeCursorResponse(1LL,
                                   _options.localOplogNS,
                                   {makeOplogEntryObj(1), makeOplogEntryObj(2), lastOp.toBSON()}));
            assertRemoteCommandNameEquals("find", request);
            ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
            net->runReadyNetworkOperations();
            // Black hole OplogFetcher's getMore request.
            auto noi = net->getNextReadyRequest();
            request = noi->getRequest();
            assertRemoteCommandNameEquals("getMore", request);
            net->blackHole(noi);
        }

        // Oplog entry associated with the stopTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({lastOp.toBSON()});

        // Last rollback ID.
        request = net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        assertRemoteCommandNameEquals("replSetGetRBID", request);
        net->runReadyNetworkOperations();

        // _multiApplierCallback() will cancel the _getNextApplierBatchCallback() task after setting
        // the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_OK(_lastApplied.getStatus());
    ASSERT_EQUALS(lastOp.getOpTime(), _lastApplied.getValue().opTime);
    ASSERT_EQUALS(lastOp.getWallClockTime(), _lastApplied.getValue().wallTime);
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsInvalidSyncSourceWhenFailInitialSyncWithBadHostFailpointIsEnabled) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // This fail point makes chooseSyncSourceCallback fail with an InvalidSyncSource error.
    auto failPoint = globalFailPointRegistry().find("failInitialSyncWithBadHost");
    failPoint->setMode(FailPoint::alwaysOn);
    ON_BLOCK_EXIT([failPoint]() { failPoint->setMode(FailPoint::off); });

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::InvalidSyncSource, _lastApplied);
}

TEST_F(InitialSyncerTest, OplogOutOfOrderOnOplogFetchFinish) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        // Keep the cloner from finishing so end-of-clone-stage network events don't interfere.
        FailPointEnableBlock clonerFailpoint("hangBeforeClonerStage", kListDatabasesFailPointData);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));

        // Oplog entry associated with the defaultBeginFetchingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        // Send an empty optime as the response to the beginFetchingOptime find request, which will
        // cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
        assertRemoteCommandNameEquals("find", request);
        net->runReadyNetworkOperations();

        // Oplog entry associated with the beginApplyingTimestamp.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

        {
            // Ensure second lastOplogFetch doesn't happen until we're ready for it.
            FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                                 kListDatabasesFailPointData);
            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // OplogFetcher's oplog tailing query.
            request = net->scheduleSuccessfulResponse(
                makeCursorResponse(1LL, _options.localOplogNS, {makeOplogEntryObj(1)}));
            assertRemoteCommandNameEquals("find", request);
            ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
            net->runReadyNetworkOperations();

            // Ensure that OplogFetcher fails with an OplogOutOfOrder error by responding to the
            // getMore request with oplog entries containing the following timestamps (most recently
            // processed oplog entry has a timestamp of 1):
            //     (last=1), 5, 4
            request = net->scheduleSuccessfulResponse(makeCursorResponse(
                1LL, _options.localOplogNS, {makeOplogEntryObj(5), makeOplogEntryObj(4)}, false));
            assertRemoteCommandNameEquals("getMore", request);
            net->runReadyNetworkOperations();
        }
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OplogOutOfOrder, _lastApplied);
}

TEST_F(InitialSyncerTest, GetInitialSyncProgressReturnsCorrectProgress) {
    // Skip reconstructing prepared transactions at the end of initial sync because
    // InitialSyncerTest does not construct ServiceEntryPoint and this causes a segmentation fault
    // when reconstructPreparedTransactions uses DBDirectClient to call into ServiceEntryPoint.
    FailPointEnableBlock skipReconstructPreparedTransactions("skipReconstructPreparedTransactions");

    // Skip clearing initial sync progress so that we can check initialSyncStatus fields after
    // initial sync is complete.
    FailPointEnableBlock skipClearInitialSyncState("skipClearInitialSyncState");

    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();
    ASSERT_OK(ServerParameterSet::getGlobal()
                  ->getMap()
                  .find("collectionClonerBatchSize")
                  ->second->setFromString("1"));

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 27017));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), 2U));

    auto net = getNet();
    int baseRollbackId = 1;

    {
        FailPointEnableBlock clonerFailpoint("hangBeforeClonerStage", kListDatabasesFailPointData);
        // Play first 2 responses to ensure initial syncer has started the oplog fetcher.
        {
            executor::NetworkInterfaceMock::InNetworkGuard guard(net);

            // Base rollback ID.
            net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));

            // Oplog entry associated with the defaultBeginFetchingTimestamp.
            processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

            // Send an empty optime as the response to the beginFetchingOptime find request, which
            // will cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
            auto request = net->scheduleSuccessfulResponse(makeCursorResponse(
                0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
            assertRemoteCommandNameEquals("find", request);
            net->runReadyNetworkOperations();

            // Oplog entry associated with the beginApplyingTimestamp.
            processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // Deliver cancellation to OplogFetcher
            net->runReadyNetworkOperations();
        }

        log() << "Done playing first failed response";

        auto progress = initialSyncer->getInitialSyncProgress();
        log() << "Progress after first failed response: " << progress;
        ASSERT_EQUALS(progress.nFields(), 7) << progress;
        ASSERT_EQUALS(progress.getIntField("failedInitialSyncAttempts"), 0) << progress;
        ASSERT_EQUALS(progress.getIntField("maxFailedInitialSyncAttempts"), 2) << progress;
        ASSERT_EQUALS(progress["initialSyncStart"].type(), Date) << progress;
        ASSERT_EQUALS(progress["initialSyncOplogStart"].timestamp(), Timestamp(1, 1)) << progress;
        ASSERT_BSONOBJ_EQ(progress.getObjectField("initialSyncAttempts"), BSONObj());
        ASSERT_EQUALS(progress.getIntField("appliedOps"), 0) << progress;
        ASSERT_BSONOBJ_EQ(progress.getObjectField("databases"), BSON("databasesCloned" << 0));

        // Inject the listDatabases failure.
        _mockServer->setCommandReply(
            "listDatabases",
            Status(ErrorCodes::FailedToParse, "fail on clone -- listDBs injected failure"));
    }

    getInitialSyncer().waitForCloner_forTest();

    log() << "Done playing failed responses";

    {
        FailPointEnableBlock clonerFailpoint("hangBeforeClonerStage", kListDatabasesFailPointData);
        // Play the first 2 responses of the successful round of responses to ensure that the
        // initial syncer starts the oplog fetcher.
        {
            executor::NetworkInterfaceMock::InNetworkGuard guard(net);

            // Make the cloner callback run.
            net->runReadyNetworkOperations();

            auto when = net->now() + _options.initialSyncRetryWait;
            ASSERT_EQUALS(when, net->runUntil(when));

            // Base rollback ID.
            auto rbidRequest =
                net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
            assertRemoteCommandNameEquals("replSetGetRBID", rbidRequest);

            // Oplog entry associated with the defaultBeginFetchingTimestamp.
            processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

            // Send an empty optime as the response to the beginFetchingOptime find request, which
            // will cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
            auto findRequest = net->scheduleSuccessfulResponse(makeCursorResponse(
                0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
            assertRemoteCommandNameEquals("find", findRequest);
            net->runReadyNetworkOperations();

            // Oplog entry associated with the beginApplyingTimestamp.
            processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();
        }

        log() << "Done playing first successful response";

        auto progress = initialSyncer->getInitialSyncProgress();
        log() << "Progress after failure: " << progress;
        ASSERT_EQUALS(progress.nFields(), 7) << progress;
        ASSERT_EQUALS(progress.getIntField("failedInitialSyncAttempts"), 1) << progress;
        ASSERT_EQUALS(progress.getIntField("maxFailedInitialSyncAttempts"), 2) << progress;
        ASSERT_EQUALS(progress["initialSyncStart"].type(), Date) << progress;
        ASSERT_EQUALS(progress["initialSyncOplogStart"].timestamp(), Timestamp(1, 1)) << progress;
        ASSERT_EQUALS(progress.getIntField("appliedOps"), 0) << progress;
        ASSERT_BSONOBJ_EQ(progress.getObjectField("databases"), BSON("databasesCloned" << 0));

        BSONObj attempts = progress["initialSyncAttempts"].Obj();
        ASSERT_EQUALS(attempts.nFields(), 1) << attempts;
        BSONObj attempt0 = attempts["0"].Obj();
        ASSERT_EQUALS(attempt0.nFields(), 3) << attempt0;
        const std::string expectedlistDatabaseFailure =
            "FailedToParse: error cloning databases :: caused by :: Command 'listDatabases' "
            "failed.";
        ASSERT_EQUALS(std::string(attempt0.getStringField("status"))
                          .substr(0, expectedlistDatabaseFailure.length()),
                      expectedlistDatabaseFailure)
            << attempt0;
        ASSERT_EQUALS(attempt0["durationMillis"].type(), NumberInt) << attempt0;
        ASSERT_EQUALS(attempt0.getStringField("syncSource"), std::string("localhost:27017"))
            << attempt0;

        // Set up the successful cloner run.
        // listDatabases: a
        NamespaceString nss("a.a");
        _mockServer->setCommandReply("listDatabases",
                                     makeListDatabasesResponse({nss.db().toString()}));

        // Set up data for "a"
        _mockServer->assignCollectionUuid(nss.ns(), *_options1.uuid);
        for (int i = 1; i <= 5; ++i) {
            _mockServer->insert(nss.ns(), BSON("_id" << i << "a" << i));
        }

        // listCollections for "a"
        _mockServer->setCommandReply(
            "listCollections",
            makeCursorResponse(
                0LL,
                nss,
                {BSON("name" << nss.coll() << "type"
                             << "collection"
                             << "options" << _options1.toBSON() << "info"
                             << BSON("readOnly" << false << "uuid" << *_options1.uuid))})
                .data);

        // count:a
        _mockServer->setCommandReply("count", BSON("n" << 5 << "ok" << 1));

        // listIndexes:a
        _mockServer->setCommandReply(
            "listIndexes",
            makeCursorResponse(
                0LL,
                NamespaceString(nss.getCommandNS()),
                {BSON("v" << OplogEntry::kOplogVersion << "key" << BSON("_id" << 1) << "name"
                          << "_id_"
                          << "ns" << nss.ns())})
                .data);

        // Play all but last of the successful round of responses.
        {
            executor::NetworkInterfaceMock::InNetworkGuard guard(net);

            // Ignore oplog tailing query.
            auto request =
                net->scheduleSuccessfulResponse(makeCursorResponse(1LL,
                                                                   _options.localOplogNS,
                                                                   {makeOplogEntryObj(1),
                                                                    makeOplogEntryObj(2),
                                                                    makeOplogEntryObj(3),
                                                                    makeOplogEntryObj(4),
                                                                    makeOplogEntryObj(5),
                                                                    makeOplogEntryObj(6),
                                                                    makeOplogEntryObj(7)}));
            assertRemoteCommandNameEquals("find", request);
            ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
            net->runReadyNetworkOperations();

            auto noi = net->getNextReadyRequest();
            request = noi->getRequest();
            assertRemoteCommandNameEquals("getMore", request);
            net->blackHole(noi);
        }
        // Release failpoint to let cloners finish.
    }
    getInitialSyncer().waitForCloner_forTest();

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Oplog entry associated with the stopTimestamp.
        // Send oplog entry with timestamp 7. InitialSyncer will update this end timestamp after
        // applying the first batch.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(7)});
    }
    log() << "Done playing all but last successful response";

    auto progress = initialSyncer->getInitialSyncProgress();
    log() << "Progress after all but last successful response: " << progress;
    ASSERT_EQUALS(progress.nFields(), 8) << progress;
    ASSERT_EQUALS(progress.getIntField("failedInitialSyncAttempts"), 1) << progress;
    ASSERT_EQUALS(progress.getIntField("maxFailedInitialSyncAttempts"), 2) << progress;
    ASSERT_EQUALS(progress["initialSyncOplogStart"].timestamp(), Timestamp(1, 1)) << progress;
    ASSERT_EQUALS(progress["initialSyncOplogEnd"].timestamp(), Timestamp(7, 1)) << progress;
    ASSERT_EQUALS(progress["initialSyncStart"].type(), Date) << progress;
    // Expected applied ops to be a superset of this range: Timestamp(2,1) ... Timestamp(7,1).
    ASSERT_GREATER_THAN_OR_EQUALS(progress.getIntField("appliedOps"), 6) << progress;
    auto databasesProgress = progress.getObjectField("databases");
    ASSERT_EQUALS(1, databasesProgress.getIntField("databasesCloned")) << databasesProgress;
    auto dbProgress = databasesProgress.getObjectField("a");
    ASSERT_EQUALS(1, dbProgress.getIntField("collections")) << dbProgress;
    ASSERT_EQUALS(1, dbProgress.getIntField("clonedCollections")) << dbProgress;
    auto collectionProgress = dbProgress.getObjectField("a.a");
    ASSERT_EQUALS(
        5, collectionProgress.getIntField(CollectionCloner::Stats::kDocumentsToCopyFieldName))
        << collectionProgress;
    ASSERT_EQUALS(
        5, collectionProgress.getIntField(CollectionCloner::Stats::kDocumentsCopiedFieldName))
        << collectionProgress;
    ASSERT_EQUALS(1, collectionProgress.getIntField("indexes")) << collectionProgress;
    ASSERT_EQUALS(5, collectionProgress.getIntField("receivedBatches")) << collectionProgress;

    auto attempts = progress["initialSyncAttempts"].Obj();
    ASSERT_EQUALS(attempts.nFields(), 1) << progress;
    auto attempt0 = attempts["0"].Obj();
    ASSERT_EQUALS(attempt0.nFields(), 3) << attempt0;
    const std::string expectedlistDatabaseFailure =
        "FailedToParse: error cloning databases :: caused by :: Command 'listDatabases' failed.";
    ASSERT_EQUALS(std::string(attempt0.getStringField("status"))
                      .substr(0, expectedlistDatabaseFailure.length()),
                  expectedlistDatabaseFailure)
        << attempt0;
    ASSERT_EQUALS(attempt0["durationMillis"].type(), NumberInt) << attempt0;
    ASSERT_EQUALS(attempt0.getStringField("syncSource"), std::string("localhost:27017"))
        << attempt0;

    // Play last successful response.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Last rollback ID.
        assertRemoteCommandNameEquals(
            "replSetGetRBID",
            net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId)));
        net->runReadyNetworkOperations();

        // _multiApplierCallback() will cancel the _getNextApplierBatchCallback() task after setting
        // the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    log() << "waiting for initial sync to verify it completed OK";
    initialSyncer->join();
    ASSERT_OK(_lastApplied.getStatus());
    auto dummyEntry = makeOplogEntry(7);
    ASSERT_EQUALS(dummyEntry.getOpTime(), _lastApplied.getValue().opTime);
    ASSERT_EQUALS(dummyEntry.getWallClockTime(), _lastApplied.getValue().wallTime);

    progress = initialSyncer->getInitialSyncProgress();
    log() << "Progress at end: " << progress;
    ASSERT_EQUALS(progress.nFields(), 10) << progress;
    ASSERT_EQUALS(progress.getIntField("failedInitialSyncAttempts"), 1) << progress;
    ASSERT_EQUALS(progress.getIntField("maxFailedInitialSyncAttempts"), 2) << progress;
    ASSERT_EQUALS(progress["initialSyncStart"].type(), Date) << progress;
    ASSERT_EQUALS(progress["initialSyncEnd"].type(), Date) << progress;
    ASSERT_EQUALS(progress["initialSyncOplogStart"].timestamp(), Timestamp(1, 1)) << progress;
    ASSERT_EQUALS(progress["initialSyncOplogEnd"].timestamp(), Timestamp(7, 1)) << progress;
    ASSERT_EQUALS(progress["initialSyncElapsedMillis"].type(), NumberInt) << progress;
    // Expected applied ops to be a superset of this range: Timestamp(2,1) ... Timestamp(7,1).
    ASSERT_GREATER_THAN_OR_EQUALS(progress.getIntField("appliedOps"), 6) << progress;

    attempts = progress["initialSyncAttempts"].Obj();
    ASSERT_EQUALS(attempts.nFields(), 2) << attempts;

    attempt0 = attempts["0"].Obj();
    ASSERT_EQUALS(attempt0.nFields(), 3) << attempt0;
    ASSERT_EQUALS(std::string(attempt0.getStringField("status"))
                      .substr(0, expectedlistDatabaseFailure.length()),
                  expectedlistDatabaseFailure)
        << attempt0;
    ASSERT_EQUALS(attempt0["durationMillis"].type(), NumberInt) << attempt0;
    ASSERT_EQUALS(attempt0.getStringField("syncSource"), std::string("localhost:27017"))
        << attempt0;

    BSONObj attempt1 = attempts["1"].Obj();
    ASSERT_EQUALS(attempt1.nFields(), 3) << attempt1;
    ASSERT_EQUALS(attempt1.getStringField("status"), std::string("OK")) << attempt1;
    ASSERT_EQUALS(attempt1["durationMillis"].type(), NumberInt) << attempt1;
    ASSERT_EQUALS(attempt1.getStringField("syncSource"), std::string("localhost:27017"))
        << attempt1;
}

TEST_F(InitialSyncerTest, GetInitialSyncProgressOmitsClonerStatsIfClonerStatsExceedBsonLimit) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 27017));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), 2U));

    const std::size_t numCollections = 200000U;

    auto net = getNet();
    int baseRollbackId = 1;
    {
        auto collectionClonerFailPoint = globalFailPointRegistry().find("hangAfterClonerStage");
        auto timesEntered = collectionClonerFailPoint->setMode(FailPoint::alwaysOn,
                                                               0,
                                                               BSON("cloner"
                                                                    << "CollectionCloner"
                                                                    << "stage"
                                                                    << "count"));
        ON_BLOCK_EXIT(
            [collectionClonerFailPoint]() { collectionClonerFailPoint->setMode(FailPoint::off); });

        {

            executor::NetworkInterfaceMock::InNetworkGuard guard(net);

            // Base rollback ID.
            net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));

            // Oplog entry associated with the defaultBeginFetchingTimestamp.
            processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

            // Send an empty optime as the response to the beginFetchingOptime find request, which
            // will cause the beginFetchingTimestamp to be set to the defaultBeginFetchingTimestamp.
            auto request = net->scheduleSuccessfulResponse(makeCursorResponse(
                0LL, NamespaceString::kSessionTransactionsTableNamespace, {}, true));
            assertRemoteCommandNameEquals("find", request);
            net->runReadyNetworkOperations();

            // Oplog entry associated with the beginApplyingTimestamp.
            processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntryObj(1)});

            // Set up the cloner data.  This must be done before providing the FCV to avoid races.
            // listDatabases
            NamespaceString nss("a.a");
            _mockServer->setCommandReply("listDatabases",
                                         makeListDatabasesResponse({nss.db().toString()}));

            // listCollections for "a"
            // listCollections data has to be broken up or it will trigger BSONObjTooLarge
            // spuriously. We want it to be triggered for the stats, not the listCollections.
            std::vector<BSONObj> collectionInfos[4];
            for (std::size_t i = 0; i < numCollections; ++i) {
                CollectionOptions options;
                const std::string collName = str::stream() << "coll-" << i;
                options.uuid = UUID::gen();
                collectionInfos[(i * 4) / numCollections].push_back(
                    BSON("name" << collName << "type"
                                << "collection"
                                << "options" << options.toBSON() << "info"
                                << BSON("uuid" << *options.uuid)));
            }
            const bool notFirstBatch = false;
            _mockServer->setCommandReply(
                "listCollections",
                {makeCursorResponse(1LL, nss.getCommandNS(), collectionInfos[0]).data,
                 makeCursorResponse(1LL, nss.getCommandNS(), collectionInfos[1], notFirstBatch)
                     .data,
                 makeCursorResponse(1LL, nss.getCommandNS(), collectionInfos[2], notFirstBatch)
                     .data,
                 makeCursorResponse(0LL, nss.getCommandNS(), collectionInfos[3], notFirstBatch)
                     .data});

            // All document counts are 0.
            _mockServer->setCommandReply("count", BSON("n" << 0 << "ok" << 1));

            // listIndexes for all collections.
            _mockServer->setCommandReply(
                "listIndexes",
                makeCursorResponse(
                    0LL,
                    NamespaceString(nss.getCommandNS()),
                    {BSON("v" << OplogEntry::kOplogVersion << "key" << BSON("_id" << 1) << "name"
                              << "_id_"
                              << "ns" << nss.ns())})
                    .data);

            // Feature Compatibility Version.
            processSuccessfulFCVFetcherResponseLastStable();

            // Ignore oplog tailing query.
            request = net->scheduleSuccessfulResponse(
                makeCursorResponse(1LL, _options.localOplogNS, {makeOplogEntryObj(1)}));
            assertRemoteCommandNameEquals("find", request);
            ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
            net->runReadyNetworkOperations();
        }

        // Wait to reach the CollectionCloner, when stats should be populated;
        collectionClonerFailPoint->waitForTimesEntered(timesEntered + 1);

        // This returns a valid document because we omit the cloner stats when they do not fit in a
        // BSON document.
        auto progress = initialSyncer->getInitialSyncProgress();
        ASSERT_EQUALS(progress["initialSyncStart"].type(), Date) << progress;
        ASSERT_FALSE(progress.hasField("databases")) << progress;

        // Initial sync will attempt to log stats again at shutdown in a callback, where it should
        // not terminate because we now return a valid stats document.
        ASSERT_OK(initialSyncer->shutdown());
    }

    // Deliver cancellation signal to callbacks.
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
}

}  // namespace
