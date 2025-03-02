/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

#include "eden/fs/testharness/FakeBackingStore.h"

#include <folly/executors/QueuedImmediateExecutor.h>
#include <folly/experimental/TestUtil.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/portability/GTest.h>
#include <folly/test/TestUtils.h>

#include "eden/fs/model/TestOps.h"
#include "eden/fs/store/FilteredBackingStore.h"
#include "eden/fs/testharness/FakeFilter.h"
#include "eden/fs/testharness/TestUtil.h"
#include "eden/fs/utils/PathFuncs.h"

namespace {

using namespace facebook::eden;
using namespace std::literals::chrono_literals;
using folly::io::Cursor;

const char kTestFilter1[] = "foo";
const char kTestFilter2[] = "football2";
const char kTestFilter3[] = "football3";
const char kTestFilter4[] = "shouldFilterZeroObjects";
const char kTestFilter5[] = "bazbar";

class FilteredBackingStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    wrappedStore_ = std::make_shared<FakeBackingStore>();
    auto fakeFilter = std::make_unique<FakeFilter>();
    filteredStore_ = std::make_shared<FilteredBackingStore>(
        wrappedStore_, std::move(fakeFilter));
  }

  void TearDown() override {
    filteredStore_.reset();
  }

  std::shared_ptr<FakeBackingStore> wrappedStore_;
  std::shared_ptr<FilteredBackingStore> filteredStore_;
};

/**
 * Helper function to get blob contents as a string.
 *
 * We unfortunately can't use moveToFbString() or coalesce() since the Blob's
 * contents are always const.
 */
std::string blobContents(const Blob& blob) {
  Cursor c(&blob.getContents());
  return c.readFixedString(blob.getContents().computeChainDataLength());
}

TEST_F(FilteredBackingStoreTest, getNonExistent) {
  // getRootTree()/getTree()/getBlob() should throw immediately
  // when called on non-existent objects.
  EXPECT_THROW_RE(
      filteredStore_->getRootTree(
          RootId{fmt::format("1:{}", kTestFilter1)},
          ObjectFetchContext::getNullContext()),
      std::domain_error,
      "commit 1 not found");
  auto hash = makeTestHash("1");
  auto blobFilterId = FilteredObjectId(hash);
  EXPECT_THROW_RE(
      filteredStore_->getBlob(
          ObjectId{blobFilterId.getValue()},
          ObjectFetchContext::getNullContext()),
      std::domain_error,
      "blob 1.*1 not found");
  auto relPath = RelativePathPiece{"foo/bar"};
  auto treeFilterId = FilteredObjectId(relPath, kTestFilter1, hash);
  EXPECT_THROW_RE(
      filteredStore_->getTree(
          ObjectId{treeFilterId.getValue()},
          ObjectFetchContext::getNullContext()),
      std::domain_error,
      "tree 0.*1 not found");
}

TEST_F(FilteredBackingStoreTest, getBlob) {
  // Add a blob to the tree
  auto hash = makeTestHash("1");
  auto* storedBlob = wrappedStore_->putBlob(hash, "foobar");
  EXPECT_EQ("foobar", blobContents(storedBlob->get()));

  // The blob is not ready yet, so calling getBlob() should yield not-ready
  // Future objects.
  auto future1 =
      filteredStore_->getBlob(hash, ObjectFetchContext::getNullContext());
  EXPECT_FALSE(future1.isReady());
  auto future2 =
      filteredStore_->getBlob(hash, ObjectFetchContext::getNullContext());
  EXPECT_FALSE(future2.isReady());

  // Calling trigger() should make the pending futures ready.
  storedBlob->trigger();
  ASSERT_TRUE(future1.isReady());
  ASSERT_TRUE(future2.isReady());
  EXPECT_EQ("foobar", blobContents(*std::move(future1).get(0ms).blob));
  EXPECT_EQ("foobar", blobContents(*std::move(future2).get(0ms).blob));

  // But subsequent calls to getBlob() should still yield unready futures.
  auto future3 =
      filteredStore_->getBlob(hash, ObjectFetchContext::getNullContext());
  EXPECT_FALSE(future3.isReady());
  auto future4 =
      filteredStore_->getBlob(hash, ObjectFetchContext::getNullContext());
  EXPECT_FALSE(future4.isReady());
  bool future4Failed = false;
  folly::exception_wrapper future4Error;

  std::move(future4)
      .via(&folly::QueuedImmediateExecutor::instance())
      .thenValue([](auto&&) { FAIL() << "future4 should not succeed\n"; })
      .thenError([&](const folly::exception_wrapper& ew) {
        future4Failed = true;
        future4Error = ew;
      });

  // Calling triggerError() should fail pending futures
  storedBlob->triggerError(std::logic_error("does not compute"));
  ASSERT_TRUE(future3.isReady());
  EXPECT_THROW_RE(
      std::move(future3).get(0ms), std::logic_error, "does not compute");
  ASSERT_TRUE(future4Failed);
  EXPECT_THROW_RE(
      future4Error.throw_exception(), std::logic_error, "does not compute");

  // Calling setReady() should make the pending futures ready, as well
  // as all subsequent Futures returned by getBlob()
  auto future5 =
      filteredStore_->getBlob(hash, ObjectFetchContext::getNullContext());
  EXPECT_FALSE(future5.isReady());

  storedBlob->setReady();
  ASSERT_TRUE(future5.isReady());
  EXPECT_EQ("foobar", blobContents(*std::move(future5).get(0ms).blob));

  // Subsequent calls to getBlob() should return Futures that are immediately
  // ready since we called setReady() above.
  auto future6 =
      filteredStore_->getBlob(hash, ObjectFetchContext::getNullContext());
  ASSERT_TRUE(future6.isReady());
  EXPECT_EQ("foobar", blobContents(*std::move(future6).get(0ms).blob));
}

TEST_F(FilteredBackingStoreTest, getTree) {
  // Populate some files in the store
  auto [runme, runme_id] =
      wrappedStore_->putBlob("#!/bin/sh\necho 'hello world!'\n");
  auto foo_id = makeTestHash("f00");
  (void)wrappedStore_->putBlob(foo_id, "this is foo\n");
  auto [bar, bar_id] = wrappedStore_->putBlob("barbarbarbar\n");

  // Populate a couple directories as well
  auto* dir1 = wrappedStore_->putTree(
      makeTestHash("abc"),
      {
          // "foo" will be filtered once the filter is applied
          {"foo", foo_id},
          {"runme", runme_id, FakeBlobType::EXECUTABLE_FILE},
      });
  EXPECT_EQ(makeTestHash("abc"), dir1->get().getHash());
  auto* dir2 = wrappedStore_->putTree(
      {{"README", wrappedStore_->putBlob("docs go here")}});

  // Create a root directory and populate the root tree
  auto rootHash = makeTestHash("10101010");
  auto treeHash = FilteredObjectId(RelativePath{""}, kTestFilter1, rootHash);
  auto treeOID = ObjectId{treeHash.getValue()};
  auto* rootDir = wrappedStore_->putTree(
      rootHash,
      {
          {"bar", bar_id},
          {"dir1", dir1},
          {"readonly", dir2},
          {"zzz", foo_id, FakeBlobType::REGULAR_FILE},
          // this "foo" will also be filtered once the filter is applied.
          {"foo", foo_id, FakeBlobType::REGULAR_FILE},
      });

  // Try getting the root tree but fail it with triggerError()
  auto future1 =
      filteredStore_->getTree(treeOID, ObjectFetchContext::getNullContext());
  EXPECT_FALSE(future1.isReady());
  rootDir->triggerError(std::runtime_error("cosmic rays"));
  EXPECT_THROW_RE(
      std::move(future1).get(0ms), std::runtime_error, "cosmic rays");

  // Now try using trigger()
  auto future2 =
      filteredStore_->getTree(treeOID, ObjectFetchContext::getNullContext());
  EXPECT_FALSE(future2.isReady());
  auto future3 =
      filteredStore_->getTree(treeOID, ObjectFetchContext::getNullContext());
  EXPECT_FALSE(future3.isReady());
  rootDir->trigger();

  // Get the root tree object from the future
  auto tree2 = std::move(future2).get(0ms).tree;
  EXPECT_EQ(treeOID, tree2->getHash());
  EXPECT_EQ(4, tree2->size());

  // Get the tree entries for the root tree
  auto [barName, barTreeEntry] = *tree2->find("bar"_pc);
  auto [dir1Name, dir1TreeEntry] = *tree2->find("dir1"_pc);
  auto [readonlyName, readonlyTreeEntry] = *tree2->find("readonly"_pc);
  auto [zzzName, zzzTreeEntry] = *tree2->find("zzz"_pc);

  // We expect foo to be filtered from the root tree
  auto fooFindRes = tree2->find("foo"_pc);
  EXPECT_EQ(fooFindRes, tree2->cend());

  // Get the subtree for dir1
  auto dir1FOID = FilteredObjectId(
      RelativePath{"dir1"}, kTestFilter1, dir1->get().getHash());
  auto subTreefuture = filteredStore_->getTree(
      ObjectId{dir1FOID.getValue()}, ObjectFetchContext::getNullContext());
  dir1->trigger();
  auto subTree = std::move(subTreefuture).get(0ms).tree;

  // We expect runme to exist in the subtree
  auto [runmeName, runmeTreeEntry] = *subTree->find("runme"_pc);
  EXPECT_EQ("runme"_pc, runmeName);
  auto runmeFOID = FilteredObjectId(runme_id);
  if (folly::kIsWindows) {
    // Windows executables show up as regular files
    EXPECT_EQ(TreeEntryType::REGULAR_FILE, runmeTreeEntry.getType());
  } else {
    EXPECT_EQ(TreeEntryType::EXECUTABLE_FILE, runmeTreeEntry.getType());
  }
  EXPECT_EQ(runmeFOID.getValue(), runmeTreeEntry.getHash().asString());

  // We don't expect foo to be in the subtree. It should be filtered out.
  EXPECT_EQ(subTree->find("foo"_pc), subTree->cend());

  // Finally, test that all other entries in the root tree are valid.
  EXPECT_EQ("bar"_pc, barName);
  auto barFOID = FilteredObjectId(bar_id);
  EXPECT_EQ(barFOID.getValue(), barTreeEntry.getHash().asString());
  EXPECT_EQ(TreeEntryType::REGULAR_FILE, barTreeEntry.getType());

  EXPECT_EQ("dir1"_pc, dir1Name);
  EXPECT_EQ(dir1FOID.getValue(), dir1TreeEntry.getHash().asString());
  EXPECT_EQ(TreeEntryType::TREE, dir1TreeEntry.getType());

  EXPECT_EQ("readonly"_pc, readonlyName);
  auto dir2FOID = FilteredObjectId{
      RelativePath{"readonly"}, kTestFilter1, dir2->get().getHash()};
  EXPECT_EQ(dir2FOID.getValue(), readonlyTreeEntry.getHash().asString());
  // TreeEntry objects only tracking the owner executable bit, so even though
  // we input the permissions as 0500 above this really ends up returning 0755
  EXPECT_EQ(TreeEntryType::TREE, readonlyTreeEntry.getType());

  EXPECT_EQ("zzz"_pc, zzzName);
  auto zzzFOID = FilteredObjectId{foo_id};
  EXPECT_EQ(zzzFOID.getValue(), zzzTreeEntry.getHash().asString());
  EXPECT_EQ(TreeEntryType::REGULAR_FILE, zzzTreeEntry.getType());

  // We expect future3 to also contain the root tree object
  EXPECT_EQ(treeOID, std::move(future3).get(0ms).tree->getHash());

  // Now try using setReady()
  auto future4 =
      filteredStore_->getTree(treeOID, ObjectFetchContext::getNullContext());
  EXPECT_FALSE(future4.isReady());
  rootDir->setReady();
  EXPECT_EQ(treeOID, std::move(future4).get(0ms).tree->getHash());

  auto future5 =
      filteredStore_->getTree(treeOID, ObjectFetchContext::getNullContext());
  EXPECT_EQ(treeOID, std::move(future5).get(0ms).tree->getHash());
}

TEST_F(FilteredBackingStoreTest, getRootTree) {
  // Set up one commit with a root tree
  auto dir1Hash = makeTestHash("abc");
  auto dir1FOID = FilteredObjectId(RelativePath{""}, kTestFilter1, dir1Hash);
  auto* dir1 = wrappedStore_->putTree(
      dir1Hash, {{"foo", wrappedStore_->putBlob("foo\n")}});
  auto* commit1 = wrappedStore_->putCommit(RootId{"1"}, dir1);
  // Set up a second commit, but don't actually add the tree object for this
  // one
  auto* commit2 = wrappedStore_->putCommit(RootId{"2"}, makeTestHash("3"));

  auto future1 = filteredStore_->getRootTree(
      RootId{fmt::format("1:{}", kTestFilter1)},
      ObjectFetchContext::getNullContext());
  EXPECT_FALSE(future1.isReady());
  auto future2 = filteredStore_->getRootTree(
      RootId{fmt::format("2:{}", kTestFilter1)},
      ObjectFetchContext::getNullContext());
  EXPECT_FALSE(future2.isReady());

  // Trigger commit1, then dir1 to make future1 ready.
  commit1->trigger();
  EXPECT_FALSE(future1.isReady());
  dir1->trigger();
  EXPECT_EQ(
      ObjectId{dir1FOID.getValue()}, std::move(future1).get(0ms)->getHash());

  // future2 should still be pending
  EXPECT_FALSE(future2.isReady());

  // Get another future for commit1
  auto future3 = filteredStore_->getRootTree(
      RootId{fmt::format("1:{}", kTestFilter1)},
      ObjectFetchContext::getNullContext());
  EXPECT_FALSE(future3.isReady());

  // Triggering the directory now should have no effect,
  // since there should be no futures for it yet.
  dir1->trigger();
  EXPECT_FALSE(future3.isReady());
  commit1->trigger();
  EXPECT_FALSE(future3.isReady());
  dir1->trigger();
  EXPECT_EQ(ObjectId{dir1FOID.getValue()}, std::move(future3).get()->getHash());

  // Try triggering errors
  auto future4 = filteredStore_->getRootTree(
      RootId{fmt::format("1:{}", kTestFilter1)},
      ObjectFetchContext::getNullContext());
  EXPECT_FALSE(future4.isReady());
  commit1->triggerError(std::runtime_error("bad luck"));
  EXPECT_THROW_RE(std::move(future4).get(0ms), std::runtime_error, "bad luck");

  auto future5 = filteredStore_->getRootTree(
      RootId{fmt::format("1:{}", kTestFilter1)},
      ObjectFetchContext::getNullContext());
  EXPECT_FALSE(future5.isReady());
  commit1->trigger();
  EXPECT_FALSE(future5.isReady());
  dir1->triggerError(std::runtime_error("PC Load Letter"));
  EXPECT_THROW_RE(
      std::move(future5).get(0ms), std::runtime_error, "PC Load Letter");

  // Now trigger commit2.
  // This should trigger future2 to fail since the tree does not actually
  // exist.
  commit2->trigger();
  EXPECT_THROW_RE(
      std::move(future2).get(0ms),
      std::domain_error,
      "tree .* for commit .* not found");
}

TEST_F(FilteredBackingStoreTest, testCompareBlobObjectsById) {
  // Populate some blobs for testing.
  //
  // NOTE: FakeBackingStore is very dumb and implements its
  // compareObjectsById function as a bytewise comparison of hashes. Therefore,
  // in order for two blobs to be equal, their hashes (NOT their contents) need
  // to be equal.
  auto foobarHash = makeTestHash("f00");
  (void)wrappedStore_->putBlob(foobarHash, "foobar");
  auto footballHash = makeTestHash("f001ba11");
  (void)wrappedStore_->putBlob(footballHash, "football");

  // populate some trees
  auto rootDirHash = makeTestHash("f00d");
  auto* rootDirTree = wrappedStore_->putTree(
      rootDirHash,
      {
          {"foobar1", foobarHash},
          {"foobar2", foobarHash},
          {"football1", footballHash},
          {"football2", footballHash},
      });
  auto fooDirExtendedHash = makeTestHash("f00d1e");
  auto* fooDirExtendedTree = wrappedStore_->putTree(
      fooDirExtendedHash,
      {
          {"foobar1", foobarHash},
          {"foobar2", foobarHash},
          {"foobar3", foobarHash},
          {"football1", footballHash},
          {"football2", footballHash},
      });

  // Set up one commit with a root tree
  auto* commit1 = wrappedStore_->putCommit(RootId{"1"}, rootDirTree);
  // Set up a second commit with an additional file
  auto* commit2 = wrappedStore_->putCommit(RootId{"2"}, fooDirExtendedTree);

  auto future1 = filteredStore_->getRootTree(
      RootId{fmt::format("1:{}", kTestFilter2)},
      ObjectFetchContext::getNullContext());
  auto future2 = filteredStore_->getRootTree(
      RootId{fmt::format("2:{}", kTestFilter3)},
      ObjectFetchContext::getNullContext());

  // Trigger commit1, then rootDirTree to make future1 ready.
  commit1->trigger();
  EXPECT_FALSE(future1.isReady());
  rootDirTree->trigger();
  auto fooDirRes = std::move(future1).get(0ms);

  // Get the object IDs of all the blobs from commit 1.
  auto [foobar1Name1, foobar1TreeEntry1] = *fooDirRes->find("foobar1"_pc);
  auto foobar1OID1 = foobar1TreeEntry1.getHash();
  auto [foobar2Name1, foobar2TreeEntry1] = *fooDirRes->find("foobar2"_pc);
  auto foobar2OID1 = foobar2TreeEntry1.getHash();
  auto [football1Name1, football1TreeEntry1] = *fooDirRes->find("football1"_pc);
  auto football1OID1 = football1TreeEntry1.getHash();

  // We expect all the foo blobs in commit 1 to NOT be filtered. Therefore, foos
  // should equal foos. Football2 is filtered, and therefore unavailable for
  // comparison.
  EXPECT_EQ(
      filteredStore_->compareObjectsById(foobar1OID1, foobar2OID1),
      ObjectComparison::Identical);
  EXPECT_EQ(
      filteredStore_->compareObjectsById(foobar2OID1, foobar1OID1),
      ObjectComparison::Identical);
  EXPECT_EQ(
      filteredStore_->compareObjectsById(football1OID1, football1OID1),
      ObjectComparison::Identical);
  EXPECT_NE(
      filteredStore_->compareObjectsById(football1OID1, foobar1OID1),
      ObjectComparison::Identical);
  EXPECT_NE(
      filteredStore_->compareObjectsById(foobar2OID1, football1OID1),
      ObjectComparison::Identical);

  // Trigger commit2, then rootDirTreeExtended to make future2 ready.
  commit2->trigger();
  fooDirExtendedTree->trigger();
  auto fooDirExtRes = std::move(future2).get(0ms);

  // Get the object IDs of all the blobs from commit 1.
  auto [foobar1Name2, foobar1TreeEntry2] = *fooDirExtRes->find("foobar1"_pc);
  auto foobar1OID2 = foobar1TreeEntry2.getHash();
  auto [foobar2Name2, foobar2TreeEntry2] = *fooDirExtRes->find("foobar2"_pc);
  auto foobar2OID2 = foobar2TreeEntry2.getHash();
  auto [football1Name2, football1TreeEntry2] =
      *fooDirExtRes->find("football1"_pc);
  auto football1OID2 = football1TreeEntry2.getHash();
  auto [football2Name2, football2TreeEntry2] =
      *fooDirExtRes->find("football2"_pc);
  auto football2OID2 = football2TreeEntry2.getHash();

  // Only football3 is unavailable for comparison in commit2. Let's make sure
  // all the corresponding blobs evaluate to equal even if they have different
  // filters.
  EXPECT_EQ(
      filteredStore_->compareObjectsById(foobar1OID1, foobar1OID2),
      ObjectComparison::Identical);
  EXPECT_EQ(
      filteredStore_->compareObjectsById(foobar2OID1, foobar1OID2),
      ObjectComparison::Identical);
  EXPECT_EQ(
      filteredStore_->compareObjectsById(football1OID1, football1OID2),
      ObjectComparison::Identical);
  EXPECT_EQ(
      filteredStore_->compareObjectsById(football1OID1, football2OID2),
      ObjectComparison::Identical);
  EXPECT_NE(
      filteredStore_->compareObjectsById(football1OID1, foobar1OID1),
      ObjectComparison::Identical);
  EXPECT_NE(
      filteredStore_->compareObjectsById(foobar2OID1, football2OID2),
      ObjectComparison::Identical);
}

TEST_F(FilteredBackingStoreTest, testCompareTreeObjectsById) {
  // Populate some blobs for testing.
  //
  // NOTE: FakeBackingStore is very dumb and implements its
  // compareObjectsById function as a bytewise comparison of hashes. Therefore,
  // in order for two blobs to be equal, their hashes (NOT their contents) need
  // to be equal.
  auto foobarHash = makeTestHash("f00");
  (void)wrappedStore_->putBlob(foobarHash, "foobar");
  auto footballHash = makeTestHash("f001ba11");
  (void)wrappedStore_->putBlob(footballHash, "football");
  auto bazbarHash = makeTestHash("ba5ba4");
  (void)wrappedStore_->putBlob(bazbarHash, "bazbar");
  auto bazballHash = makeTestHash("ba5ba11");
  (void)wrappedStore_->putBlob(bazballHash, "bazball");

  // populate some trees
  auto grandchildTreeHash = makeTestHash("ba5");
  auto grandchildTree = wrappedStore_->putTree(
      grandchildTreeHash,
      {
          {"bazbar", bazbarHash},
          {"bazball", bazballHash},
      });
  auto childTreeHash = makeTestHash("f00ba5");
  auto childTree =
      wrappedStore_->putTree(childTreeHash, {{"grandchild", grandchildTree}});
  auto modifiedChildTreeHash = makeTestHash("f00ba52");
  auto modifiedChildTree = wrappedStore_->putTree(
      modifiedChildTreeHash,
      {{"grandchild", grandchildTree}, {"newentry", foobarHash}});
  auto rootDirHash = makeTestHash("f00d");
  auto* rootDirTree = wrappedStore_->putTree(
      rootDirHash,
      {
          {"foobar1", foobarHash},
          {"foobar2", foobarHash},
          {"football1", footballHash},
          {"football2", footballHash},
          {"child", childTree},
      });

  auto modifiedRootDirHash = makeTestHash("f00e");
  auto* modifiedRootDirTree = wrappedStore_->putTree(
      modifiedRootDirHash,
      {
          {"foobar1", foobarHash},
          {"foobar2", foobarHash},
          {"football1", footballHash},
          {"football2", footballHash},
          {"child", modifiedChildTree},
      });

  // Set up one commit with a root tree
  auto* commit1 = wrappedStore_->putCommit(RootId{"1"}, rootDirTree);
  // Set up a second commit with an additional file
  auto* commit2 = wrappedStore_->putCommit(RootId{"2"}, modifiedRootDirTree);

  auto rootFuture1 = filteredStore_->getRootTree(
      RootId{fmt::format("1:{}", kTestFilter4)},
      ObjectFetchContext::getNullContext());
  auto rootFuture2 = filteredStore_->getRootTree(
      RootId{fmt::format("2:{}", kTestFilter5)},
      ObjectFetchContext::getNullContext());

  // Trigger commit1, then rootDirTree to make rootFuture1 ready.
  commit1->trigger();
  EXPECT_FALSE(rootFuture1.isReady());
  rootDirTree->trigger();
  auto rootDirRes1 = std::move(rootFuture1).get(0ms);

  // Get the object IDs of all the trees from commit 1.
  auto [childName, childEntry] = *rootDirRes1->find("child"_pc);
  auto childOID = childEntry.getHash();
  auto childFuture1 =
      filteredStore_->getTree(childOID, ObjectFetchContext::getNullContext());
  childTree->trigger();
  auto childDirRes1 = std::move(childFuture1).get(0ms).tree;
  auto [grandchildName, grandchildEntry] = *childDirRes1->find("grandchild"_pc);
  auto grandchildOID = grandchildEntry.getHash();

  // Trigger commit2, then rootDirTreeExtended to make rootFuture2 ready.
  commit2->trigger();
  modifiedRootDirTree->trigger();
  auto rootDirCommit2Res = std::move(rootFuture2).get(0ms);

  // Get the object IDs of all the blobs from commit 1.
  auto [childName2, childEntry2] = *rootDirCommit2Res->find("child"_pc);
  auto childOID2 = childEntry2.getHash();
  auto childFuture2 =
      filteredStore_->getTree(childOID2, ObjectFetchContext::getNullContext());
  modifiedChildTree->trigger();
  auto childDirRes2 = std::move(childFuture2).get(0ms).tree;
  auto [grandchildName2, grandchildEntry2] =
      *childDirRes2->find("grandchild"_pc);
  auto grandchildOID2 = grandchildEntry2.getHash();

  // The child tree should know it changed between filters (since the actual
  // contents changed), BUT FakeBackingStore is dumb and can't determine that.
  // Therefore, this just returns unknown.
  EXPECT_EQ(
      filteredStore_->compareObjectsById(childOID, childOID2),
      ObjectComparison::Unknown);
  // The root tree didn't change, but its children might have. So it reports
  // Unknown.
  EXPECT_EQ(
      filteredStore_->compareObjectsById(
          rootDirRes1->getHash(), rootDirCommit2Res->getHash()),
      ObjectComparison::Unknown);
  // The root tree should be identical to itself
  EXPECT_EQ(
      filteredStore_->compareObjectsById(
          rootDirRes1->getHash(), rootDirRes1->getHash()),
      ObjectComparison::Identical);
  // The grandchild tree got filtered, but it isn't aware that its children were
  // filtered. We return Unknown in this case.
  EXPECT_TRUE(
      filteredStore_->compareObjectsById(grandchildOID, grandchildOID2) ==
      ObjectComparison::Unknown);
}
} // namespace
