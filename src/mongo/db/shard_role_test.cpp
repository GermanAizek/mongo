/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

void createTestCollection(OperationContext* opCtx, const NamespaceString& nss) {
    uassertStatusOK(createCollection(opCtx, nss.dbName(), BSON("create" << nss.coll())));
}

void installDatabaseMetadata(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             const DatabaseVersion& dbVersion) {
    AutoGetDb autoDb(opCtx, dbName, MODE_X, {});
    auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx, dbName);
    scopedDss->setDbInfo(opCtx, {dbName.db(), ShardId("this"), dbVersion});
}

void installUnshardedCollectionMetadata(OperationContext* opCtx, const NamespaceString& nss) {
    const auto unshardedCollectionMetadata = CollectionMetadata();
    AutoGetCollection coll(opCtx, nss, MODE_IX);
    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss)
        ->setFilteringMetadata(opCtx, unshardedCollectionMetadata);
}

void installShardedCollectionMetadata(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const DatabaseVersion& dbVersion,
                                      std::vector<ChunkType> chunks,
                                      ShardId thisShardId) {
    ASSERT(!chunks.empty());

    const auto uuid = [&] {
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        return autoColl.getCollection()->uuid();
    }();

    const std::string shardKey("skey");
    const ShardKeyPattern shardKeyPattern{BSON(shardKey << 1)};
    const auto epoch = chunks.front().getVersion().epoch();
    const auto timestamp = chunks.front().getVersion().getTimestamp();

    auto rt = RoutingTableHistory::makeNew(nss,
                                           uuid,
                                           shardKeyPattern.getKeyPattern(),
                                           nullptr,
                                           false,
                                           epoch,
                                           timestamp,
                                           boost::none /* timeseriesFields */,
                                           boost::none /* resharding Fields */,
                                           boost::none /* chunkSizeBytes */,
                                           true /* allowMigrations */,
                                           chunks);

    const auto version = rt.getVersion();
    const auto rtHandle =
        RoutingTableHistoryValueHandle(std::make_shared<RoutingTableHistory>(std::move(rt)),
                                       ComparableChunkVersion::makeComparableChunkVersion(version));

    const auto collectionMetadata = CollectionMetadata(
        ChunkManager(thisShardId, dbVersion, rtHandle, boost::none), thisShardId);

    AutoGetCollection coll(opCtx, nss, MODE_IX);
    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss)
        ->setFilteringMetadata(opCtx, collectionMetadata);
}

UUID getCollectionUUID(OperationContext* opCtx, const NamespaceString& nss) {
    const auto optUuid = CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, nss);
    ASSERT(optUuid);
    return *optUuid;
}

class ShardRoleTest : public ServiceContextMongoDTest {
protected:
    OperationContext* opCtx() {
        return _opCtx.get();
    }

    void setUp() override;
    void tearDown() override;

    const ShardId thisShardId{"this"};

    const DatabaseName dbNameTestDb{"test"};
    const DatabaseVersion dbVersionTestDb{UUID::gen(), Timestamp(1, 0)};

    const NamespaceString nssUnshardedCollection1 =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "unsharded");

    const NamespaceString nssShardedCollection1 =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "sharded");
    const ShardVersion shardVersionShardedCollection1{
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(5, 0)}, CollectionPlacement(10, 1)),
        boost::optional<CollectionIndexes>(boost::none)};

    // Workaround to be able to write parametrized TEST_F
    void testRestoreFailsIfCollectionNoLongerExists(
        AcquisitionPrerequisites::OperationType operationType);
    void testRestoreFailsIfCollectionRenamed(AcquisitionPrerequisites::OperationType operationType);
    void testRestoreFailsIfCollectionDroppedAndRecreated(
        AcquisitionPrerequisites::OperationType operationType);

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

void ShardRoleTest::setUp() {
    ServiceContextMongoDTest::setUp();
    _opCtx = getGlobalServiceContext()->makeOperationContext(&cc());
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;

    const repl::ReplSettings replSettings = {};
    repl::ReplicationCoordinator::set(
        getGlobalServiceContext(),
        std::unique_ptr<repl::ReplicationCoordinator>(
            new repl::ReplicationCoordinatorMock(_opCtx->getServiceContext(), replSettings)));
    ASSERT_OK(repl::ReplicationCoordinator::get(getGlobalServiceContext())
                  ->setFollowerMode(repl::MemberState::RS_PRIMARY));

    repl::createOplog(_opCtx.get());

    ShardingState::get(getServiceContext())->setInitialized(ShardId("this"), OID::gen());

    // Setup test collections and metadata
    installDatabaseMetadata(opCtx(), dbNameTestDb, dbVersionTestDb);

    createTestCollection(opCtx(), nssUnshardedCollection1);
    installUnshardedCollectionMetadata(opCtx(), nssUnshardedCollection1);

    createTestCollection(opCtx(), nssShardedCollection1);
    const auto uuidShardedCollection1 = getCollectionUUID(_opCtx.get(), nssShardedCollection1);
    installDatabaseMetadata(opCtx(), dbNameTestDb, dbVersionTestDb);
    installShardedCollectionMetadata(
        opCtx(),
        nssShardedCollection1,
        dbVersionTestDb,
        {ChunkType(uuidShardedCollection1,
                   ChunkRange{BSON("skey" << MINKEY), BSON("skey" << MAXKEY)},
                   shardVersionShardedCollection1.placementVersion(),
                   thisShardId)},
        thisShardId);
}

void ShardRoleTest::tearDown() {
    _opCtx.reset();
    ServiceContextMongoDTest::tearDown();
    repl::ReplicationCoordinator::set(getGlobalServiceContext(), nullptr);
}

TEST_F(ShardRoleTest, NamespaceOrViewAcquisitionRequestWithOpCtxTakesPlacementFromOSS) {
    const auto nss = nssUnshardedCollection1;

    {
        NamespaceOrViewAcquisitionRequest acquisitionRequest(
            opCtx(), nss, {}, AcquisitionPrerequisites::kWrite);
        ASSERT_EQ(boost::none, acquisitionRequest.placementConcern.dbVersion);
        ASSERT_EQ(boost::none, acquisitionRequest.placementConcern.shardVersion);
    }

    {
        const NamespaceString anotherCollection =
            NamespaceString::createNamespaceString_forTest("test2.foo");
        ScopedSetShardRole setShardRole(
            opCtx(), anotherCollection, ShardVersion::UNSHARDED(), dbVersionTestDb);
        NamespaceOrViewAcquisitionRequest acquisitionRequest(
            opCtx(), nss, {}, AcquisitionPrerequisites::kWrite);
        ASSERT_EQ(boost::none, acquisitionRequest.placementConcern.dbVersion);
        ASSERT_EQ(boost::none, acquisitionRequest.placementConcern.shardVersion);
    }

    {
        const auto dbVersion = boost::none;
        const auto shardVersion = boost::none;
        ScopedSetShardRole setShardRole(opCtx(), nss, shardVersion, dbVersion);
        NamespaceOrViewAcquisitionRequest acquisitionRequest(
            opCtx(), nss, {}, AcquisitionPrerequisites::kWrite);
        ASSERT_EQ(dbVersion, acquisitionRequest.placementConcern.dbVersion);
        ASSERT_EQ(shardVersion, acquisitionRequest.placementConcern.shardVersion);
    }

    {
        const auto dbVersion = dbVersionTestDb;
        const auto shardVersion = ShardVersion::UNSHARDED();
        ScopedSetShardRole setShardRole(opCtx(), nss, shardVersion, dbVersion);
        NamespaceOrViewAcquisitionRequest acquisitionRequest(
            opCtx(), nss, {}, AcquisitionPrerequisites::kWrite);
        ASSERT_EQ(dbVersion, acquisitionRequest.placementConcern.dbVersion);
        ASSERT_EQ(shardVersion, acquisitionRequest.placementConcern.shardVersion);
    }

    {
        const auto dbVersion = boost::none;
        const auto shardVersion = shardVersionShardedCollection1;
        ScopedSetShardRole setShardRole(opCtx(), nss, shardVersion, dbVersion);
        NamespaceOrViewAcquisitionRequest acquisitionRequest(
            opCtx(), nss, {}, AcquisitionPrerequisites::kWrite);
        ASSERT_EQ(dbVersion, acquisitionRequest.placementConcern.dbVersion);
        ASSERT_EQ(shardVersion, acquisitionRequest.placementConcern.shardVersion);
    }
}

// ---------------------------------------------------------------------------
// Placement checks when acquiring unsharded collections

TEST_F(ShardRoleTest, AcquireUnshardedCollWithCorrectPlacementVersion) {
    AcquisitionPrerequisites::PlacementConcern placementConcern =
        AcquisitionPrerequisites::PlacementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    const auto acquisitions =
        acquireCollectionsOrViews(opCtx(),
                                  {{nssUnshardedCollection1,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kMustBeCollection}},
                                  MODE_IX);

    ASSERT_EQ(1, acquisitions.size());
    ASSERT_EQ(nssUnshardedCollection1, acquisitions.front().nss());
    ASSERT_EQ(nssUnshardedCollection1, acquisitions.front().getCollectionPtr()->ns());
    ASSERT_FALSE(acquisitions.front().isView());
    ASSERT_FALSE(acquisitions.front().getShardingDescription().isSharded());
    ASSERT_FALSE(acquisitions.front().getCollectionFilter().has_value());
}

TEST_F(ShardRoleTest, AcquireUnshardedCollWithIncorrectPlacementVersionThrows) {
    const auto incorrectDbVersion = DatabaseVersion(UUID::gen(), Timestamp(50, 0));

    AcquisitionPrerequisites::PlacementConcern placementConcern =
        AcquisitionPrerequisites::PlacementConcern{incorrectDbVersion, ShardVersion::UNSHARDED()};
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViews(opCtx(),
                                  {{nssUnshardedCollection1,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kMustBeCollection}},
                                  MODE_IX),
        ExceptionFor<ErrorCodes::StaleDbVersion>,
        [&](const DBException& ex) {
            const auto exInfo = ex.extraInfo<StaleDbRoutingVersion>();
            ASSERT_EQ(dbNameTestDb.db(), exInfo->getDb());
            ASSERT_EQ(incorrectDbVersion, exInfo->getVersionReceived());
            ASSERT_EQ(dbVersionTestDb, exInfo->getVersionWanted());
            ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
        });
}

TEST_F(ShardRoleTest, AcquireUnshardedCollWhenShardDoesNotKnowThePlacementVersionThrows) {
    {
        // Clear the database metadata
        AutoGetDb autoDb(opCtx(), dbNameTestDb, MODE_X, {});
        auto scopedDss =
            DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx(), dbNameTestDb);
        scopedDss->clearDbInfo(opCtx());
    }

    AcquisitionPrerequisites::PlacementConcern placementConcern =
        AcquisitionPrerequisites::PlacementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViews(opCtx(),
                                  {{nssUnshardedCollection1,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kMustBeCollection}},
                                  MODE_IX),
        ExceptionFor<ErrorCodes::StaleDbVersion>,
        [&](const DBException& ex) {
            const auto exInfo = ex.extraInfo<StaleDbRoutingVersion>();
            ASSERT_EQ(dbNameTestDb.db(), exInfo->getDb());
            ASSERT_EQ(dbVersionTestDb, exInfo->getVersionReceived());
            ASSERT_EQ(boost::none, exInfo->getVersionWanted());
            ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
        });
}

TEST_F(ShardRoleTest, AcquireUnshardedCollWhenCriticalSectionIsActiveThrows) {
    const BSONObj criticalSectionReason = BSON("reason" << 1);
    {
        // Enter critical section.
        AutoGetDb autoDb(opCtx(), dbNameTestDb, MODE_X, {});
        auto scopedDss =
            DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx(), dbNameTestDb);
        scopedDss->enterCriticalSectionCatchUpPhase(opCtx(), criticalSectionReason);
        scopedDss->enterCriticalSectionCommitPhase(opCtx(), criticalSectionReason);
    }

    {
        AcquisitionPrerequisites::PlacementConcern placementConcern =
            AcquisitionPrerequisites::PlacementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
        ASSERT_THROWS_WITH_CHECK(
            acquireCollectionsOrViews(opCtx(),
                                      {{nssUnshardedCollection1,
                                        placementConcern,
                                        repl::ReadConcernArgs(),
                                        AcquisitionPrerequisites::kWrite,
                                        AcquisitionPrerequisites::kMustBeCollection}},
                                      MODE_IX),
            ExceptionFor<ErrorCodes::StaleDbVersion>,
            [&](const DBException& ex) {
                const auto exInfo = ex.extraInfo<StaleDbRoutingVersion>();
                ASSERT_EQ(dbNameTestDb.db(), exInfo->getDb());
                ASSERT_EQ(dbVersionTestDb, exInfo->getVersionReceived());
                ASSERT_EQ(boost::none, exInfo->getVersionWanted());
                ASSERT_TRUE(exInfo->getCriticalSectionSignal().is_initialized());
            });
    }

    {
        // Exit critical section.
        AutoGetDb autoDb(opCtx(), dbNameTestDb, MODE_X, {});
        const BSONObj criticalSectionReason = BSON("reason" << 1);
        auto scopedDss =
            DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx(), dbNameTestDb);
        scopedDss->exitCriticalSection(opCtx(), criticalSectionReason);
    }
}

TEST_F(ShardRoleTest, AcquireUnshardedCollWithoutSpecifyingPlacementVersion) {
    AcquisitionPrerequisites::PlacementConcern placementConcern =
        NamespaceOrViewAcquisitionRequest::kPretendUnshardedDueToDirectConnection;
    const auto acquisitions =
        acquireCollectionsOrViews(opCtx(),
                                  {{nssUnshardedCollection1,
                                    {placementConcern},
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kMustBeCollection}},
                                  MODE_IX);

    ASSERT_EQ(1, acquisitions.size());
    ASSERT_EQ(nssUnshardedCollection1, acquisitions.front().nss());
    ASSERT_EQ(nssUnshardedCollection1, acquisitions.front().getCollectionPtr()->ns());
    ASSERT_FALSE(acquisitions.front().isView());
    ASSERT_FALSE(acquisitions.front().getShardingDescription().isSharded());
    ASSERT_FALSE(acquisitions.front().getCollectionFilter().has_value());
}

// ---------------------------------------------------------------------------
// Placement checks when acquiring sharded collections

TEST_F(ShardRoleTest, AcquireShardedCollWithCorrectPlacementVersion) {
    AcquisitionPrerequisites::PlacementConcern placementConcern =
        AcquisitionPrerequisites::PlacementConcern{{} /* dbVersion */,
                                                   shardVersionShardedCollection1};
    const auto acquisitions =
        acquireCollectionsOrViews(opCtx(),
                                  {{nssShardedCollection1,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kMustBeCollection}},
                                  MODE_IX);

    ASSERT_EQ(1, acquisitions.size());
    ASSERT_EQ(nssShardedCollection1, acquisitions.front().nss());
    ASSERT_EQ(nssShardedCollection1, acquisitions.front().getCollectionPtr()->ns());
    ASSERT_FALSE(acquisitions.front().isView());
    ASSERT_TRUE(acquisitions.front().getShardingDescription().isSharded());
    ASSERT_TRUE(acquisitions.front().getCollectionFilter().has_value());
}

TEST_F(ShardRoleTest, AcquireShardedCollWithIncorrectPlacementVersionThrows) {
    AcquisitionPrerequisites::PlacementConcern placementConcern =
        AcquisitionPrerequisites::PlacementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViews(opCtx(),
                                  {{nssShardedCollection1,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kMustBeCollection}},
                                  MODE_IX),
        ExceptionFor<ErrorCodes::StaleConfig>,
        [&](const DBException& ex) {
            const auto exInfo = ex.extraInfo<StaleConfigInfo>();
            ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
            ASSERT_EQ(ShardVersion::UNSHARDED(), exInfo->getVersionReceived());
            ASSERT_EQ(shardVersionShardedCollection1, exInfo->getVersionWanted());
            ASSERT_EQ(ShardId("this"), exInfo->getShardId());
            ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
        });
}

TEST_F(ShardRoleTest, AcquireShardedCollWhenShardDoesNotKnowThePlacementVersionThrows) {
    {
        // Clear the collection filtering metadata on the shard.
        AutoGetCollection coll(opCtx(), nssShardedCollection1, MODE_IX);
        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx(),
                                                                             nssShardedCollection1)
            ->clearFilteringMetadata(opCtx());
    }

    AcquisitionPrerequisites::PlacementConcern placementConcern =
        AcquisitionPrerequisites::PlacementConcern{{}, shardVersionShardedCollection1};
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViews(opCtx(),
                                  {{nssShardedCollection1,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kMustBeCollection}},
                                  MODE_IX),
        ExceptionFor<ErrorCodes::StaleConfig>,
        [&](const DBException& ex) {
            const auto exInfo = ex.extraInfo<StaleConfigInfo>();
            ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
            ASSERT_EQ(shardVersionShardedCollection1, exInfo->getVersionReceived());
            ASSERT_EQ(boost::none, exInfo->getVersionWanted());
            ASSERT_EQ(ShardId("this"), exInfo->getShardId());
            ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
        });
}

TEST_F(ShardRoleTest, AcquireShardedCollWhenCriticalSectionIsActiveThrows) {
    const BSONObj criticalSectionReason = BSON("reason" << 1);
    {
        // Enter the critical section.
        AutoGetCollection coll(opCtx(), nssShardedCollection1, MODE_X);
        const auto& csr = CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
            opCtx(), nssShardedCollection1);
        csr->enterCriticalSectionCatchUpPhase(criticalSectionReason);
        csr->enterCriticalSectionCommitPhase(criticalSectionReason);
    }

    {
        AcquisitionPrerequisites::PlacementConcern placementConcern =
            AcquisitionPrerequisites::PlacementConcern{{}, shardVersionShardedCollection1};
        ASSERT_THROWS_WITH_CHECK(
            acquireCollectionsOrViews(opCtx(),
                                      {{nssShardedCollection1,
                                        placementConcern,
                                        repl::ReadConcernArgs(),
                                        AcquisitionPrerequisites::kWrite,
                                        AcquisitionPrerequisites::kMustBeCollection}},
                                      MODE_IX),
            ExceptionFor<ErrorCodes::StaleConfig>,
            [&](const DBException& ex) {
                const auto exInfo = ex.extraInfo<StaleConfigInfo>();
                ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
                ASSERT_EQ(shardVersionShardedCollection1, exInfo->getVersionReceived());
                ASSERT_EQ(boost::none, exInfo->getVersionWanted());
                ASSERT_EQ(ShardId("this"), exInfo->getShardId());
                ASSERT_TRUE(exInfo->getCriticalSectionSignal().is_initialized());
            });
    }

    {
        // Exit the critical section.
        AutoGetCollection coll(opCtx(), nssShardedCollection1, MODE_X);
        const auto& csr = CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
            opCtx(), nssShardedCollection1);
        csr->exitCriticalSection(criticalSectionReason);
    }
}

TEST_F(ShardRoleTest, AcquireShardedCollWithoutSpecifyingPlacementVersion) {
    AcquisitionPrerequisites::PlacementConcern placementConcern =
        NamespaceOrViewAcquisitionRequest::kPretendUnshardedDueToDirectConnection;
    const auto acquisitions =
        acquireCollectionsOrViews(opCtx(),
                                  {{nssShardedCollection1,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kMustBeCollection}},
                                  MODE_IX);

    ASSERT_EQ(1, acquisitions.size());
    ASSERT_EQ(nssShardedCollection1, acquisitions.front().nss());
    ASSERT_EQ(nssShardedCollection1, acquisitions.front().getCollectionPtr()->ns());
    ASSERT_FALSE(acquisitions.front().isView());

    // Note that the collection is treated as unsharded because the operation is unversioned.
    ASSERT_FALSE(acquisitions.front().getShardingDescription().isSharded());
    ASSERT_FALSE(acquisitions.front().getCollectionFilter().has_value());
}

// ---------------------------------------------------------------------------
// Acquire inexistent collections

TEST_F(ShardRoleTest, AcquireCollectionFailsIfItDoesNotExist) {
    const NamespaceString inexistentNss =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "inexistent");
    AcquisitionPrerequisites::PlacementConcern placementConcern;
    ASSERT_THROWS_CODE(acquireCollectionsOrViews(opCtx(),
                                                 {{inexistentNss,
                                                   placementConcern,
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kWrite,
                                                   AcquisitionPrerequisites::kMustBeCollection}},
                                                 MODE_IX),
                       DBException,
                       ErrorCodes::NamespaceNotFound);
}

TEST_F(ShardRoleTest, AcquireInexistentCollectionWithWrongPlacementThrowsBecauseWrongPlacement) {
    const auto incorrectDbVersion = dbVersionTestDb.makeUpdated();
    const NamespaceString inexistentNss =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "inexistent");

    AcquisitionPrerequisites::PlacementConcern placementConcern{incorrectDbVersion, {}};
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViews(opCtx(),
                                  {{inexistentNss,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kMustBeCollection}},
                                  MODE_IX),
        ExceptionFor<ErrorCodes::StaleDbVersion>,
        [&](const DBException& ex) {
            const auto exInfo = ex.extraInfo<StaleDbRoutingVersion>();
            ASSERT_EQ(dbNameTestDb.db(), exInfo->getDb());
            ASSERT_EQ(incorrectDbVersion, exInfo->getVersionReceived());
            ASSERT_EQ(dbVersionTestDb, exInfo->getVersionWanted());
            ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
        });
}

// ---------------------------------------------------------------------------
// Acquire multiple collections

TEST_F(ShardRoleTest, AcquireMultipleCollectionsAllWithCorrectPlacementConcern) {
    const auto acquisitions = acquireCollectionsOrViews(
        opCtx(),
        {{nssUnshardedCollection1,
          AcquisitionPrerequisites::PlacementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()},
          repl::ReadConcernArgs(),
          AcquisitionPrerequisites::kWrite,
          AcquisitionPrerequisites::kMustBeCollection},
         {nssShardedCollection1,
          AcquisitionPrerequisites::PlacementConcern{{}, shardVersionShardedCollection1},
          repl::ReadConcernArgs(),
          AcquisitionPrerequisites::kWrite,
          AcquisitionPrerequisites::kMustBeCollection}},
        MODE_IX);

    ASSERT_EQ(2, acquisitions.size());

    const auto& acquisitionUnshardedColl =
        std::find_if(acquisitions.begin(),
                     acquisitions.end(),
                     [nss = nssUnshardedCollection1](const auto& acquisition) {
                         return acquisition.nss() == nss;
                     });
    ASSERT(acquisitionUnshardedColl != acquisitions.end());
    ASSERT_FALSE(acquisitionUnshardedColl->isView());
    ASSERT_FALSE(acquisitionUnshardedColl->getShardingDescription().isSharded());
    ASSERT_FALSE(acquisitionUnshardedColl->getCollectionFilter().has_value());

    const auto& acquisitionShardedColl =
        std::find_if(acquisitions.begin(),
                     acquisitions.end(),
                     [nss = nssShardedCollection1](const auto& acquisition) {
                         return acquisition.nss() == nss;
                     });
    ASSERT(acquisitionShardedColl != acquisitions.end());
    ASSERT_FALSE(acquisitionShardedColl->isView());
    ASSERT_TRUE(acquisitionShardedColl->getShardingDescription().isSharded());
    ASSERT_TRUE(acquisitionShardedColl->getCollectionFilter().has_value());

    // Assert the DB lock is held, but not recursively (i.e. only once).
    ASSERT_TRUE(opCtx()->lockState()->isDbLockedForMode(dbNameTestDb, MODE_IX));
    ASSERT_FALSE(opCtx()->lockState()->isGlobalLockedRecursively());

    // Assert both collections are locked.
    ASSERT_TRUE(opCtx()->lockState()->isCollectionLockedForMode(nssUnshardedCollection1, MODE_IX));
    ASSERT_TRUE(opCtx()->lockState()->isCollectionLockedForMode(nssShardedCollection1, MODE_IX));
}

TEST_F(ShardRoleTest, AcquireMultipleCollectionsWithIncorrectPlacementConcernThrows) {
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViews(opCtx(),
                                  {{nssUnshardedCollection1,
                                    AcquisitionPrerequisites::PlacementConcern{
                                        dbVersionTestDb, ShardVersion::UNSHARDED()},
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kMustBeCollection},
                                   {nssShardedCollection1,
                                    AcquisitionPrerequisites::PlacementConcern{
                                        dbVersionTestDb, ShardVersion::UNSHARDED()},
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kMustBeCollection}},
                                  MODE_IX),
        ExceptionFor<ErrorCodes::StaleConfig>,
        [&](const DBException& ex) {
            const auto exInfo = ex.extraInfo<StaleConfigInfo>();
            ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
            ASSERT_EQ(ShardVersion::UNSHARDED(), exInfo->getVersionReceived());
            ASSERT_EQ(shardVersionShardedCollection1, exInfo->getVersionWanted());
            ASSERT_EQ(ShardId("this"), exInfo->getShardId());
            ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
        });
}

DEATH_TEST_REGEX_F(ShardRoleTest,
                   ForbiddenToAcquireMultipleCollectionsOnDifferentDatabases,
                   "Tripwire assertion") {
    ASSERT_THROWS_CODE(
        acquireCollectionsOrViews(
            opCtx(),
            {{nssUnshardedCollection1,
              NamespaceOrViewAcquisitionRequest::kPretendUnshardedDueToDirectConnection,
              repl::ReadConcernArgs(),
              AcquisitionPrerequisites::kWrite,
              AcquisitionPrerequisites::kMustBeCollection},
             {NamespaceString::createNamespaceString_forTest("anotherDb", "foo"),
              NamespaceOrViewAcquisitionRequest::kPretendUnshardedDueToDirectConnection,
              repl::ReadConcernArgs(),
              AcquisitionPrerequisites::kWrite,
              AcquisitionPrerequisites::kMustBeCollection}},
            MODE_IX),
        DBException,
        7300400);
}

// ---------------------------------------------------------------------------
// Acquire collection by UUID

TEST_F(ShardRoleTest, AcquireCollectionByUUID) {
    const auto uuid = getCollectionUUID(opCtx(), nssUnshardedCollection1);
    const auto acquisitions = acquireCollectionsOrViews(
        opCtx(),
        {{NamespaceStringOrUUID(dbNameTestDb, uuid),
          AcquisitionPrerequisites::PlacementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()},
          repl::ReadConcernArgs(),
          AcquisitionPrerequisites::kWrite,
          AcquisitionPrerequisites::kMustBeCollection}},
        MODE_IX);

    ASSERT_EQ(1, acquisitions.size());
    ASSERT_EQ(nssUnshardedCollection1, acquisitions.front().nss());
    ASSERT_EQ(nssUnshardedCollection1, acquisitions.front().getCollectionPtr()->ns());
}

TEST_F(ShardRoleTest, AcquireCollectionByUUIDButWrongDbNameThrows) {
    const auto uuid = getCollectionUUID(opCtx(), nssUnshardedCollection1);
    ASSERT_THROWS_CODE(acquireCollectionsOrViews(opCtx(),
                                                 {{NamespaceStringOrUUID("anotherDbName", uuid),
                                                   {},
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kWrite,
                                                   AcquisitionPrerequisites::kMustBeCollection}},
                                                 MODE_IX),
                       DBException,
                       ErrorCodes::NamespaceNotFound);
}

TEST_F(ShardRoleTest, AcquireCollectionByWrongUUID) {
    const auto uuid = UUID::gen();
    ASSERT_THROWS_CODE(acquireCollectionsOrViews(opCtx(),
                                                 {{NamespaceStringOrUUID(dbNameTestDb, uuid),
                                                   {},
                                                   repl::ReadConcernArgs(),
                                                   AcquisitionPrerequisites::kWrite,
                                                   AcquisitionPrerequisites::kMustBeCollection}},
                                                 MODE_IX),
                       DBException,
                       ErrorCodes::NamespaceNotFound);
}

// ---------------------------------------------------------------------------
// Acquire collection by nss and expected UUID

TEST_F(ShardRoleTest, AcquireCollectionByNssAndExpectedUUID) {
    const auto uuid = getCollectionUUID(opCtx(), nssUnshardedCollection1);
    const auto acquisitions =
        acquireCollectionsOrViews(opCtx(),
                                  {{nssUnshardedCollection1,
                                    uuid,
                                    {},
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kMustBeCollection}},
                                  MODE_IX);

    ASSERT_EQ(1, acquisitions.size());
    ASSERT_EQ(nssUnshardedCollection1, acquisitions.front().nss());
    ASSERT_EQ(nssUnshardedCollection1, acquisitions.front().getCollectionPtr()->ns());
}

TEST_F(ShardRoleTest, AcquireCollectionByNssAndWrongExpectedUUIDThrows) {
    const auto nss = nssUnshardedCollection1;
    const auto wrongUuid = UUID::gen();
    ASSERT_THROWS_WITH_CHECK(
        acquireCollectionsOrViews(opCtx(),
                                  {{nss,
                                    wrongUuid,
                                    {},
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kMustBeCollection}},
                                  MODE_IX),
        ExceptionFor<ErrorCodes::CollectionUUIDMismatch>,
        [&](const DBException& ex) {
            const auto exInfo = ex.extraInfo<CollectionUUIDMismatchInfo>();
            ASSERT_EQ(nss.dbName(), exInfo->dbName());
            ASSERT_EQ(wrongUuid, exInfo->collectionUUID());
            ASSERT_EQ(nss.coll(), exInfo->expectedCollection());
            ASSERT_EQ(boost::none, exInfo->actualCollection());
        });
}

// ---------------------------------------------------------------------------
// Yield and restore

TEST_F(ShardRoleTest, YieldAndRestoreAcquisitionWithLocks) {
    const auto nss = nssUnshardedCollection1;

    AcquisitionPrerequisites::PlacementConcern placementConcern =
        AcquisitionPrerequisites::PlacementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    const auto acquisition =
        acquireCollectionsOrViews(opCtx(),
                                  {{nss,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kMustBeCollection}},
                                  MODE_IX);

    ASSERT_TRUE(opCtx()->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
    ASSERT_TRUE(opCtx()->lockState()->isCollectionLockedForMode(nss, MODE_IX));

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());
    ASSERT_FALSE(opCtx()->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
    ASSERT_FALSE(opCtx()->lockState()->isCollectionLockedForMode(nss, MODE_IX));

    // Restore the resources
    restoreTransactionResourcesToOperationContext(opCtx(), std::move(yieldedTransactionResources));
    ASSERT_TRUE(opCtx()->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
    ASSERT_TRUE(opCtx()->lockState()->isCollectionLockedForMode(nss, MODE_IX));
}

TEST_F(ShardRoleTest, RestoreForWriteFailsIfPlacementConcernNoLongerMet) {
    const auto nss = nssShardedCollection1;

    AcquisitionPrerequisites::PlacementConcern placementConcern =
        AcquisitionPrerequisites::PlacementConcern{{}, shardVersionShardedCollection1};
    const auto acquisition =
        acquireCollectionsOrViews(opCtx(),
                                  {{nss,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kMustBeCollection}},
                                  MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());

    // Placement changes
    const auto newShardVersion = [&]() {
        auto newPlacementVersion = shardVersionShardedCollection1.placementVersion();
        newPlacementVersion.incMajor();
        return ShardVersion(newPlacementVersion, boost::optional<CollectionIndexes>(boost::none));
    }();
    const auto uuid = getCollectionUUID(opCtx(), nss);
    installShardedCollectionMetadata(
        opCtx(),
        nss,
        dbVersionTestDb,
        {ChunkType(uuid,
                   ChunkRange{BSON("skey" << MINKEY), BSON("skey" << MAXKEY)},
                   newShardVersion.placementVersion(),
                   thisShardId)},
        thisShardId);

    // Try to restore the resources should fail because placement concern is no longer met.
    ASSERT_THROWS_WITH_CHECK(restoreTransactionResourcesToOperationContext(
                                 opCtx(), std::move(yieldedTransactionResources)),
                             ExceptionFor<ErrorCodes::StaleConfig>,
                             [&](const DBException& ex) {
                                 const auto exInfo = ex.extraInfo<StaleConfigInfo>();
                                 ASSERT_EQ(nssShardedCollection1, exInfo->getNss());
                                 ASSERT_EQ(shardVersionShardedCollection1,
                                           exInfo->getVersionReceived());
                                 ASSERT_EQ(newShardVersion, exInfo->getVersionWanted());
                                 ASSERT_EQ(ShardId("this"), exInfo->getShardId());
                                 ASSERT_FALSE(exInfo->getCriticalSectionSignal().is_initialized());
                             });

    ASSERT_FALSE(opCtx()->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
    ASSERT_FALSE(opCtx()->lockState()->isCollectionLockedForMode(nss, MODE_IX));
}

TEST_F(ShardRoleTest, RestoreWithShardVersionIgnored) {
    const auto nss = nssShardedCollection1;

    AcquisitionPrerequisites::PlacementConcern placementConcern =
        AcquisitionPrerequisites::PlacementConcern{{}, ShardVersion::IGNORED()};
    const auto acquisition =
        acquireCollectionsOrViews(opCtx(),
                                  {{nss,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kMustBeCollection}},
                                  MODE_IX);

    ASSERT_TRUE(acquisition.front().getShardingDescription().isSharded());
    ASSERT_TRUE(acquisition.front().getCollectionFilter().has_value());

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());

    // Placement changes
    const auto newShardVersion = [&]() {
        auto newPlacementVersion = shardVersionShardedCollection1.placementVersion();
        newPlacementVersion.incMajor();
        return ShardVersion(newPlacementVersion, boost::optional<CollectionIndexes>(boost::none));
    }();

    const auto uuid = getCollectionUUID(opCtx(), nss);
    installShardedCollectionMetadata(
        opCtx(),
        nss,
        dbVersionTestDb,
        {ChunkType(uuid,
                   ChunkRange{BSON("skey" << MINKEY), BSON("skey" << MAXKEY)},
                   newShardVersion.placementVersion(),
                   thisShardId)},
        thisShardId);

    // Try to restore the resources should work because placement concern (IGNORED) can be met.
    restoreTransactionResourcesToOperationContext(opCtx(), std::move(yieldedTransactionResources));
    ASSERT_TRUE(opCtx()->lockState()->isCollectionLockedForMode(nss, MODE_IX));
}

void ShardRoleTest::testRestoreFailsIfCollectionNoLongerExists(
    AcquisitionPrerequisites::OperationType operationType) {
    const auto nss = nssShardedCollection1;

    AcquisitionPrerequisites::PlacementConcern placementConcern =
        AcquisitionPrerequisites::PlacementConcern{{}, shardVersionShardedCollection1};
    const auto acquisition =
        acquireCollectionsOrViews(opCtx(),
                                  {{nss,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    operationType,
                                    AcquisitionPrerequisites::kMustBeCollection}},
                                  MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());

    // Drop the collection
    {
        DBDirectClient client(opCtx());
        client.dropCollection(nss);
    }

    // Try to restore the resources should fail because the collection no longer exists.
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           opCtx(), std::move(yieldedTransactionResources)),
                       DBException,
                       ErrorCodes::NamespaceNotFound);
}
TEST_F(ShardRoleTest, RestoreForReadFailsIfCollectionNoLongerExists) {
    testRestoreFailsIfCollectionNoLongerExists(AcquisitionPrerequisites::kRead);
}
TEST_F(ShardRoleTest, RestoreForWriteFailsIfCollectionNoLongerExists) {
    testRestoreFailsIfCollectionNoLongerExists(AcquisitionPrerequisites::kWrite);
}

void ShardRoleTest::testRestoreFailsIfCollectionRenamed(
    AcquisitionPrerequisites::OperationType operationType) {
    const auto nss = nssUnshardedCollection1;

    AcquisitionPrerequisites::PlacementConcern placementConcern =
        AcquisitionPrerequisites::PlacementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    const auto acquisition =
        acquireCollectionsOrViews(opCtx(),
                                  {{nss,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    operationType,
                                    AcquisitionPrerequisites::kMustBeCollection}},
                                  MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());

    // Rename the collection.
    {
        DBDirectClient client(opCtx());
        BSONObj info;
        ASSERT_TRUE(client.runCommand(
            DatabaseName(boost::none, dbNameTestDb.db()),
            BSON("renameCollection"
                 << nss.ns() << "to"
                 << NamespaceString::createNamespaceString_forTest(dbNameTestDb, "foo2").ns()),
            info));
    }

    // Try to restore the resources should fail because the collection has been renamed.
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           opCtx(), std::move(yieldedTransactionResources)),
                       DBException,
                       ErrorCodes::NamespaceNotFound);
}
TEST_F(ShardRoleTest, RestoreForReadFailsIfCollectionRenamed) {
    testRestoreFailsIfCollectionRenamed(AcquisitionPrerequisites::kRead);
}
TEST_F(ShardRoleTest, RestoreForWriteFailsIfCollectionRenamed) {
    testRestoreFailsIfCollectionRenamed(AcquisitionPrerequisites::kWrite);
}

void ShardRoleTest::testRestoreFailsIfCollectionDroppedAndRecreated(
    AcquisitionPrerequisites::OperationType operationType) {
    const auto nss = nssUnshardedCollection1;

    AcquisitionPrerequisites::PlacementConcern placementConcern =
        AcquisitionPrerequisites::PlacementConcern{dbVersionTestDb, ShardVersion::UNSHARDED()};
    const auto acquisition =
        acquireCollectionsOrViews(opCtx(),
                                  {{nss,
                                    placementConcern,
                                    repl::ReadConcernArgs(),
                                    operationType,
                                    AcquisitionPrerequisites::kMustBeCollection}},
                                  MODE_IX);

    // Yield the resources
    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());

    // Drop the collection and create a new one with the same nss.
    {
        DBDirectClient client(opCtx());
        client.dropCollection(nss);
        createTestCollection(opCtx(), nss);
    }

    // Try to restore the resources should fail because the collection no longer exists.
    ASSERT_THROWS_CODE(restoreTransactionResourcesToOperationContext(
                           opCtx(), std::move(yieldedTransactionResources)),
                       DBException,
                       ErrorCodes::CollectionUUIDMismatch);
}
TEST_F(ShardRoleTest, RestoreForWriteFailsIfCollectionDroppedAndRecreated) {
    testRestoreFailsIfCollectionDroppedAndRecreated(AcquisitionPrerequisites::kWrite);
}
TEST_F(ShardRoleTest, RestoreForReadFailsIfCollectionDroppedAndRecreated) {
    testRestoreFailsIfCollectionDroppedAndRecreated(AcquisitionPrerequisites::kRead);
}

TEST_F(ShardRoleTest, RestoreForReadSucceedsEvenIfPlacementHasChanged) {
    const auto nss = nssShardedCollection1;

    AcquisitionPrerequisites::PlacementConcern placementConcern =
        AcquisitionPrerequisites::PlacementConcern{{}, shardVersionShardedCollection1};

    SharedSemiFuture<void> ongoingQueriesCompletionFuture;

    {
        const auto acquisition =
            acquireCollectionsOrViews(opCtx(),
                                      {{nss,
                                        placementConcern,
                                        repl::ReadConcernArgs(),
                                        AcquisitionPrerequisites::kRead,
                                        AcquisitionPrerequisites::kMustBeCollection}},
                                      MODE_IX);

        ongoingQueriesCompletionFuture =
            CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx(), nss)
                ->getOngoingQueriesCompletionFuture(
                    getCollectionUUID(opCtx(), nss),
                    ChunkRange(BSON("skey" << MINKEY), BSON("skey" << MAXKEY)));

        // Yield the resources
        auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx());

        ASSERT_FALSE(ongoingQueriesCompletionFuture.isReady());
        ASSERT_TRUE(acquisition.front().getCollectionFilter().has_value());
        ASSERT_TRUE(acquisition.front().getCollectionFilter()->keyBelongsToMe(BSON("skey" << 0)));

        // Placement changes
        const auto newShardVersion = [&]() {
            auto newPlacementVersion = shardVersionShardedCollection1.placementVersion();
            newPlacementVersion.incMajor();
            return ShardVersion(newPlacementVersion,
                                boost::optional<CollectionIndexes>(boost::none));
        }();

        const auto uuid = getCollectionUUID(opCtx(), nss);
        installShardedCollectionMetadata(
            opCtx(),
            nss,
            dbVersionTestDb,
            {ChunkType(uuid,
                       ChunkRange{BSON("skey" << MINKEY), BSON("skey" << MAXKEY)},
                       newShardVersion.placementVersion(),
                       ShardId("anotherShard"))},
            thisShardId);

        // Restore should work for reads even though placement has changed.
        restoreTransactionResourcesToOperationContext(opCtx(),
                                                      std::move(yieldedTransactionResources));

        ASSERT_FALSE(ongoingQueriesCompletionFuture.isReady());

        // Even though placement has changed, the filter (and preserver) still point to the original
        // placement.
        ASSERT_TRUE(acquisition.front().getCollectionFilter().has_value());
        ASSERT_TRUE(acquisition.front().getCollectionFilter()->keyBelongsToMe(BSON("skey" << 0)));
    }

    // Acquisition released. Now the range is no longer in use.
    ASSERT_TRUE(ongoingQueriesCompletionFuture.isReady());
}

}  // namespace
}  // namespace mongo
