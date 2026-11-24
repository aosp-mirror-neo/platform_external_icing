// Copyright (C) 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "icing/document-builder.h"
#include "icing/file/filesystem.h"
#include "icing/icing-search-engine.h"
#include "icing/jni/jni-cache.h"
#include "icing/legacy/index/icing-filesystem.h"
#include "icing/portable/equals-proto.h"
#include "icing/portable/platform.h"
#include "icing/proto/change_notification.pb.h"
#include "icing/proto/document.pb.h"
#include "icing/proto/initialize.pb.h"
#include "icing/proto/schema.pb.h"
#include "icing/proto/status.pb.h"
#include "icing/schema-builder.h"
#include "icing/testing/common-matchers.h"
#include "icing/testing/fake-clock.h"
#include "icing/testing/jni-test-helpers.h"
#include "icing/testing/test-data.h"
#include "icing/testing/tmp-directory.h"
#include "icing/util/clock.h"
#include "icing/util/icu-data-file-helper.h"

namespace icing {
namespace lib {

namespace {

using ::icing::lib::portable_equals_proto::EqualsProto;
using ::testing::Eq;
using ::testing::Gt;
using ::testing::IsEmpty;
using ::testing::SizeIs;

using DocumentChangeSetProto =
    DocumentChangeResponseProto::DocumentChangeSetProto;

// Allows tests to provide a FakeClock, since the operation timestamps of
// changes are issued from the clock.
class TestIcingSearchEngine : public IcingSearchEngine {
 public:
  TestIcingSearchEngine(const IcingSearchEngineOptions& options,
                        std::unique_ptr<const Filesystem> filesystem,
                        std::unique_ptr<const IcingFilesystem> icing_filesystem,
                        std::unique_ptr<Clock> clock,
                        std::unique_ptr<JniCache> jni_cache)
      : IcingSearchEngine(options, std::move(filesystem),
                          std::move(icing_filesystem), std::move(clock),
                          std::move(jni_cache)) {}
};

std::string GetTestBaseDir() { return GetTestTempDir() + "/icing"; }

IcingSearchEngineOptions GetDefaultIcingOptions() {
  IcingSearchEngineOptions icing_options;
  icing_options.set_base_dir(GetTestBaseDir());
  icing_options.set_enable_offline_observers(true);
  return icing_options;
}

SchemaProto CreateSchema() {
  return SchemaBuilder()
      .AddType(SchemaTypeConfigBuilder().SetType("Email"))
      .AddType(SchemaTypeConfigBuilder().SetType("Message"))
      .Build();
}

DocumentProto CreateDocument(std::string name_space, std::string uri,
                             std::string schema) {
  return DocumentBuilder()
      .SetKey(std::move(name_space), std::move(uri))
      .SetSchema(std::move(schema))
      .Build();
}

// Creates an Email document that is created at creation_timestamp_ms and
// expires ttl_ms after that.
DocumentProto CreateExpiringDocument(std::string name_space, std::string uri,
                                     int64_t creation_timestamp_ms,
                                     int64_t ttl_ms) {
  return DocumentBuilder()
      .SetKey(std::move(name_space), std::move(uri))
      .SetSchema("Email")
      .SetCreationTimestampMs(creation_timestamp_ms)
      .SetTtlMs(ttl_ms)
      .Build();
}

DocumentChangeResponseProto::PaginationToken MakePageToken(
    int64_t last_sync_timestamp_ms) {
  DocumentChangeResponseProto::PaginationToken page_token;
  page_token.set_last_sync_timestamp_ms(last_sync_timestamp_ms);
  return page_token;
}

// A change to the document with uri at operation_timestamp_ms.
struct ExpectedChange {
  std::string uri;
  int64_t operation_timestamp_ms;
};

// Creates a DocumentChangeSetProto for name_space and schema with a change
// token for each of changes, in order.
DocumentChangeSetProto CreateChangeSet(
    std::string name_space, std::string schema,
    const std::vector<ExpectedChange>& changes) {
  DocumentChangeSetProto change_set;
  change_set.set_name_space(std::move(name_space));
  change_set.set_schema(std::move(schema));
  for (const ExpectedChange& change : changes) {
    DocumentChangeSetProto::ChangeToken* token = change_set.add_change_tokens();
    token->set_uri(change.uri);
    token->set_operation_timestamp_ms(change.operation_timestamp_ms);
  }
  return change_set;
}

// Creates an OK DocumentChangeResponseProto with change_sets, in order, no more
// changes, and a next page token at last_sync_timestamp_ms.
DocumentChangeResponseProto CreateExpectedResponse(
    std::vector<DocumentChangeSetProto> change_sets,
    int64_t last_sync_timestamp_ms) {
  DocumentChangeResponseProto response;
  response.mutable_status()->set_code(StatusProto::OK);
  for (DocumentChangeSetProto& change_set : change_sets) {
    *response.add_document_change_sets() = std::move(change_set);
  }
  response.set_has_more_changes(false);
  response.mutable_next_page_token()->set_last_sync_timestamp_ms(
      last_sync_timestamp_ms);
  return response;
}

// This test is meant to cover all tests relating to
// IcingSearchEngine::GetDocumentChanges.
class IcingSearchEngineChangeNotificationTest : public testing::Test {
 protected:
  void SetUp() override {
    if (!IsCfStringTokenization() && !IsReverseJniTokenization()) {
      // If we've specified using the reverse-JNI method for segmentation (i.e.
      // not ICU), then we won't have the ICU data file included to set up.
      // File generated via icu_data_file rule in //icing/BUILD.
      std::string icu_data_file_path =
          GetTestFilePath("icing/icu.dat");
      ICING_ASSERT_OK(
          icu_data_file_helper::SetUpIcuDataFile(icu_data_file_path));
    }
    filesystem_.CreateDirectoryRecursively(GetTestBaseDir().c_str());
  }

  void TearDown() override {
    filesystem_.DeleteDirectoryRecursively(GetTestBaseDir().c_str());
  }

  // Creates an IcingSearchEngine whose clock is fake_clock_.
  std::unique_ptr<IcingSearchEngine> CreateIcing(
      const IcingSearchEngineOptions& options) {
    auto fake_clock = std::make_unique<FakeClock>();
    fake_clock_ = fake_clock.get();
    return std::make_unique<TestIcingSearchEngine>(
        options, std::make_unique<Filesystem>(),
        std::make_unique<IcingFilesystem>(), std::move(fake_clock),
        GetTestJniCache());
  }

  // Puts the document with the clock set to timestamp_ms, so that its
  // operation timestamp is timestamp_ms.
  PutResultProto PutAt(IcingSearchEngine* icing, DocumentProto document,
                       int64_t timestamp_ms) {
    fake_clock_->SetSystemTimeMilliseconds(timestamp_ms);
    return icing->Put(std::move(document));
  }

  // Puts a document with the given key and schema at timestamp_ms.
  PutResultProto PutAt(IcingSearchEngine* icing, std::string name_space,
                       std::string uri, std::string schema,
                       int64_t timestamp_ms) {
    return PutAt(icing,
                 CreateDocument(std::move(name_space), std::move(uri),
                                std::move(schema)),
                 timestamp_ms);
  }

  // Deletes the document with the clock set to timestamp_ms, so that its
  // deletion timestamp is timestamp_ms.
  DeleteResultProto DeleteAt(IcingSearchEngine* icing,
                             std::string_view name_space, std::string_view uri,
                             int64_t timestamp_ms) {
    fake_clock_->SetSystemTimeMilliseconds(timestamp_ms);
    return icing->Delete(name_space, uri);
  }

  // Owned by the IcingSearchEngine returned by the last CreateIcing call.
  FakeClock* fake_clock_ = nullptr;

 private:
  Filesystem filesystem_;
};

TEST_F(IcingSearchEngineChangeNotificationTest, NotInitialized) {
  std::unique_ptr<IcingSearchEngine> icing =
      CreateIcing(GetDefaultIcingOptions());
  DocumentChangeResponseProto response =
      icing->GetDocumentChanges(/*schemas=*/{}, MakePageToken(0));
  EXPECT_THAT(response.status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));
}

TEST_F(IcingSearchEngineChangeNotificationTest, OfflineObserversDisabled) {
  IcingSearchEngineOptions options = GetDefaultIcingOptions();
  options.set_enable_offline_observers(false);
  std::unique_ptr<IcingSearchEngine> icing = CreateIcing(options);
  ASSERT_THAT(icing->Initialize().status(), ProtoIsOk());

  DocumentChangeResponseProto response =
      icing->GetDocumentChanges(/*schemas=*/{}, MakePageToken(0));
  EXPECT_THAT(response.status(),
              ProtoStatusIs(StatusProto::FAILED_PRECONDITION));
}

TEST_F(IcingSearchEngineChangeNotificationTest, NoChanges) {
  std::unique_ptr<IcingSearchEngine> icing =
      CreateIcing(GetDefaultIcingOptions());
  ASSERT_THAT(icing->Initialize().status(), ProtoIsOk());

  // No schema has been set yet.
  DocumentChangeResponseProto expected =
      CreateExpectedResponse({}, /*last_sync_timestamp_ms=*/100);
  EXPECT_THAT(
      icing->GetDocumentChanges(/*schemas=*/{"Email"}, MakePageToken(100)),
      EqualsProto(expected));

  ASSERT_THAT(icing->SetSchema(CreateSchema()).status(), ProtoIsOk());
  EXPECT_THAT(icing->GetDocumentChanges(/*schemas=*/{}, MakePageToken(100)),
              EqualsProto(expected));
}

TEST_F(IcingSearchEngineChangeNotificationTest,
       ReturnsChangesGroupedByNamespaceAndSchema) {
  std::unique_ptr<IcingSearchEngine> icing =
      CreateIcing(GetDefaultIcingOptions());
  ASSERT_THAT(icing->Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing->SetSchema(CreateSchema()).status(), ProtoIsOk());

  ASSERT_THAT(PutAt(icing.get(), "ns1", "email1", "Email", 10).status(),
              ProtoIsOk());
  ASSERT_THAT(PutAt(icing.get(), "ns1", "message1", "Message", 20).status(),
              ProtoIsOk());
  ASSERT_THAT(PutAt(icing.get(), "ns2", "email2", "Email", 30).status(),
              ProtoIsOk());
  ASSERT_THAT(PutAt(icing.get(), "ns1", "email3", "Email", 40).status(),
              ProtoIsOk());

  EXPECT_THAT(
      icing->GetDocumentChanges(/*schemas=*/{}, MakePageToken(0)),
      EqualsProto(CreateExpectedResponse(
          {CreateChangeSet("ns1", "Email", {{"email1", 10}, {"email3", 40}}),
           CreateChangeSet("ns1", "Message", {{"message1", 20}}),
           CreateChangeSet("ns2", "Email", {{"email2", 30}})},
          /*last_sync_timestamp_ms=*/40)));
}

TEST_F(IcingSearchEngineChangeNotificationTest, SchemaFilter) {
  std::unique_ptr<IcingSearchEngine> icing =
      CreateIcing(GetDefaultIcingOptions());
  ASSERT_THAT(icing->Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing->SetSchema(CreateSchema()).status(), ProtoIsOk());

  ASSERT_THAT(PutAt(icing.get(), "ns", "email1", "Email", 10).status(),
              ProtoIsOk());
  ASSERT_THAT(PutAt(icing.get(), "ns", "message1", "Message", 20).status(),
              ProtoIsOk());
  ASSERT_THAT(PutAt(icing.get(), "ns", "message2", "Message", 30).status(),
              ProtoIsOk());
  ASSERT_THAT(PutAt(icing.get(), "ns", "email2", "Email", 40).status(),
              ProtoIsOk());
  ASSERT_THAT(DeleteAt(icing.get(), "ns", "email2", 50).status(), ProtoIsOk());
  ASSERT_THAT(DeleteAt(icing.get(), "ns", "message2", 60).status(),
              ProtoIsOk());

  // Only changes of the requested schema types are returned. message2 was
  // added and then deleted after the timestamp, so only its deletion is
  // returned.
  EXPECT_THAT(icing->GetDocumentChanges(/*schemas=*/{"Message", "Nonexistent"},
                                        MakePageToken(0)),
              EqualsProto(CreateExpectedResponse(
                  {CreateChangeSet("ns", "Message",
                                   {{"message1", 20}, {"message2", 60}})},
                  /*last_sync_timestamp_ms=*/60)));

  // Only deletions of the requested schema types are returned.
  EXPECT_THAT(icing->GetDocumentChanges(/*schemas=*/{"Message", "Nonexistent"},
                                        MakePageToken(40)),
              EqualsProto(CreateExpectedResponse(
                  {CreateChangeSet("ns", "Message", {{"message2", 60}})},
                  /*last_sync_timestamp_ms=*/60)));
}

// The token of the last page (has_more_changes is false) covers every change
// returned so far, so it only returns changes after them. Tokens of pages that
// end in the middle of changes sharing a timestamp re-return the changes at
// that timestamp instead; see the BuildDocumentChangeResponse tests in
// document-change_test.cc.
TEST_F(IcingSearchEngineChangeNotificationTest,
       LastPageTokenReturnsOnlyLaterChanges) {
  std::unique_ptr<IcingSearchEngine> icing =
      CreateIcing(GetDefaultIcingOptions());
  ASSERT_THAT(icing->Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing->SetSchema(CreateSchema()).status(), ProtoIsOk());

  ASSERT_THAT(PutAt(icing.get(), "ns", "uri1", "Email", 10).status(),
              ProtoIsOk());
  ASSERT_THAT(PutAt(icing.get(), "ns", "uri2", "Email", 20).status(),
              ProtoIsOk());

  DocumentChangeResponseProto first_response =
      icing->GetDocumentChanges(/*schemas=*/{}, MakePageToken(0));
  ASSERT_THAT(first_response.status(), ProtoIsOk());
  ASSERT_FALSE(first_response.has_more_changes());
  ASSERT_THAT(first_response.document_change_sets(), SizeIs(1));
  EXPECT_THAT(first_response.document_change_sets(0).change_tokens(),
              SizeIs(2));
  EXPECT_THAT(first_response.next_page_token().last_sync_timestamp_ms(),
              Eq(20));

  // Calling again with the returned token returns no changes, and the same
  // token.
  EXPECT_THAT(
      icing->GetDocumentChanges(/*schemas=*/{},
                                first_response.next_page_token()),
      EqualsProto(CreateExpectedResponse({}, /*last_sync_timestamp_ms=*/20)));

  // Update uri1, delete uri2 and add uri3.
  ASSERT_THAT(PutAt(icing.get(), "ns", "uri1", "Email", 30).status(),
              ProtoIsOk());
  ASSERT_THAT(DeleteAt(icing.get(), "ns", "uri2", 35).status(), ProtoIsOk());
  ASSERT_THAT(PutAt(icing.get(), "ns", "uri3", "Email", 40).status(),
              ProtoIsOk());

  DocumentChangeResponseProto expected = CreateExpectedResponse(
      {CreateChangeSet("ns", "Email",
                       {{"uri1", 30}, {"uri2", 35}, {"uri3", 40}})},
      /*last_sync_timestamp_ms=*/40);
  EXPECT_THAT(icing->GetDocumentChanges(/*schemas=*/{},
                                        first_response.next_page_token()),
              EqualsProto(expected));

  // Retrieving changes is idempotent.
  EXPECT_THAT(icing->GetDocumentChanges(/*schemas=*/{},
                                        first_response.next_page_token()),
              EqualsProto(expected));
}

TEST_F(IcingSearchEngineChangeNotificationTest, ReturnsDeletedDocuments) {
  std::unique_ptr<IcingSearchEngine> icing =
      CreateIcing(GetDefaultIcingOptions());
  ASSERT_THAT(icing->Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing->SetSchema(CreateSchema()).status(), ProtoIsOk());

  ASSERT_THAT(PutAt(icing.get(), "ns1", "email1", "Email", 10).status(),
              ProtoIsOk());
  ASSERT_THAT(PutAt(icing.get(), "ns2", "message1", "Message", 20).status(),
              ProtoIsOk());
  ASSERT_THAT(DeleteAt(icing.get(), "ns1", "email1", 30).status(), ProtoIsOk());
  ASSERT_THAT(DeleteAt(icing.get(), "ns2", "message1", 40).status(),
              ProtoIsOk());

  // Deletions are grouped by namespace and schema like other changes.
  EXPECT_THAT(icing->GetDocumentChanges(/*schemas=*/{}, MakePageToken(20)),
              EqualsProto(CreateExpectedResponse(
                  {CreateChangeSet("ns1", "Email", {{"email1", 30}}),
                   CreateChangeSet("ns2", "Message", {{"message1", 40}})},
                  /*last_sync_timestamp_ms=*/40)));

  // A deletion at exactly the timestamp is not returned.
  EXPECT_THAT(icing->GetDocumentChanges(/*schemas=*/{}, MakePageToken(30)),
              EqualsProto(CreateExpectedResponse(
                  {CreateChangeSet("ns2", "Message", {{"message1", 40}})},
                  /*last_sync_timestamp_ms=*/40)));
}

TEST_F(IcingSearchEngineChangeNotificationTest,
       DocumentAddedAndDeletedAfterTimestampIsReturnedAsDeleted) {
  std::unique_ptr<IcingSearchEngine> icing =
      CreateIcing(GetDefaultIcingOptions());
  ASSERT_THAT(icing->Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing->SetSchema(CreateSchema()).status(), ProtoIsOk());

  ASSERT_THAT(PutAt(icing.get(), "ns", "uri1", "Email", 10).status(),
              ProtoIsOk());
  ASSERT_THAT(PutAt(icing.get(), "ns", "uri2", "Email", 20).status(),
              ProtoIsOk());
  ASSERT_THAT(DeleteAt(icing.get(), "ns", "uri2", 30).status(), ProtoIsOk());

  // uri2 didn't exist as of timestamp 10, but its deletion is still returned,
  // since Icing can't tell whether the caller has seen it (see the tests
  // below). Deletions of documents the caller never saw are harmless.
  EXPECT_THAT(icing->GetDocumentChanges(/*schemas=*/{}, MakePageToken(10)),
              EqualsProto(CreateExpectedResponse(
                  {CreateChangeSet("ns", "Email", {{"uri2", 30}})},
                  /*last_sync_timestamp_ms=*/30)));
}

TEST_F(IcingSearchEngineChangeNotificationTest,
       DocumentUpdatedAfterTimestampAndDeletedIsReturnedAsDeleted) {
  std::unique_ptr<IcingSearchEngine> icing =
      CreateIcing(GetDefaultIcingOptions());
  ASSERT_THAT(icing->Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing->SetSchema(CreateSchema()).status(), ProtoIsOk());

  ASSERT_THAT(PutAt(icing.get(), "ns", "uri", "Email", 10).status(),
              ProtoIsOk());
  DocumentChangeResponseProto first_response =
      icing->GetDocumentChanges(/*schemas=*/{}, MakePageToken(0));
  ASSERT_THAT(first_response,
              EqualsProto(CreateExpectedResponse(
                  {CreateChangeSet("ns", "Email", {{"uri", 10}})},
                  /*last_sync_timestamp_ms=*/10)));

  // uri is updated and then deleted after the caller saw it, so its last
  // update is after the token even though the caller has it.
  ASSERT_THAT(PutAt(icing.get(), "ns", "uri", "Email", 20).status(),
              ProtoIsOk());
  ASSERT_THAT(DeleteAt(icing.get(), "ns", "uri", 30).status(), ProtoIsOk());

  EXPECT_THAT(icing->GetDocumentChanges(/*schemas=*/{},
                                        first_response.next_page_token()),
              EqualsProto(CreateExpectedResponse(
                  {CreateChangeSet("ns", "Email", {{"uri", 30}})},
                  /*last_sync_timestamp_ms=*/30)));
}

TEST_F(IcingSearchEngineChangeNotificationTest,
       RetryWithSameTokenReturnsDeletionOfDocumentAddedAfterIt) {
  std::unique_ptr<IcingSearchEngine> icing =
      CreateIcing(GetDefaultIcingOptions());
  ASSERT_THAT(icing->Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing->SetSchema(CreateSchema()).status(), ProtoIsOk());

  ASSERT_THAT(PutAt(icing.get(), "ns", "uri", "Email", 10).status(),
              ProtoIsOk());
  // The caller gets uri, but e.g. fails to save the returned token, so it
  // retries with the same token later.
  DocumentChangeResponseProto::PaginationToken page_token = MakePageToken(0);
  ASSERT_THAT(icing->GetDocumentChanges(/*schemas=*/{}, page_token),
              EqualsProto(CreateExpectedResponse(
                  {CreateChangeSet("ns", "Email", {{"uri", 10}})},
                  /*last_sync_timestamp_ms=*/10)));

  ASSERT_THAT(DeleteAt(icing.get(), "ns", "uri", 20).status(), ProtoIsOk());

  // The retry must return the deletion, since the caller may have applied the
  // addition from the first call.
  EXPECT_THAT(icing->GetDocumentChanges(/*schemas=*/{}, page_token),
              EqualsProto(CreateExpectedResponse(
                  {CreateChangeSet("ns", "Email", {{"uri", 20}})},
                  /*last_sync_timestamp_ms=*/20)));
}

TEST_F(IcingSearchEngineChangeNotificationTest,
       DocumentDeletedAndReaddedIsReturnedOnce) {
  std::unique_ptr<IcingSearchEngine> icing =
      CreateIcing(GetDefaultIcingOptions());
  ASSERT_THAT(icing->Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing->SetSchema(CreateSchema()).status(), ProtoIsOk());

  ASSERT_THAT(PutAt(icing.get(), "ns", "uri", "Email", 10).status(),
              ProtoIsOk());
  ASSERT_THAT(DeleteAt(icing.get(), "ns", "uri", 20).status(), ProtoIsOk());
  ASSERT_THAT(PutAt(icing.get(), "ns", "uri", "Email", 30).status(),
              ProtoIsOk());

  // Both the deletion and the re-addition are changes, but only the latest one
  // is returned.
  EXPECT_THAT(icing->GetDocumentChanges(/*schemas=*/{}, MakePageToken(10)),
              EqualsProto(CreateExpectedResponse(
                  {CreateChangeSet("ns", "Email", {{"uri", 30}})},
                  /*last_sync_timestamp_ms=*/30)));
}

TEST_F(IcingSearchEngineChangeNotificationTest, DeletionsPersistAcrossReopen) {
  {
    std::unique_ptr<IcingSearchEngine> icing =
        CreateIcing(GetDefaultIcingOptions());
    ASSERT_THAT(icing->Initialize().status(), ProtoIsOk());
    ASSERT_THAT(icing->SetSchema(CreateSchema()).status(), ProtoIsOk());
    ASSERT_THAT(PutAt(icing.get(), "ns", "uri", "Email", 10).status(),
                ProtoIsOk());
    ASSERT_THAT(DeleteAt(icing.get(), "ns", "uri", 20).status(), ProtoIsOk());
  }

  std::unique_ptr<IcingSearchEngine> icing =
      CreateIcing(GetDefaultIcingOptions());
  ASSERT_THAT(icing->Initialize().status(), ProtoIsOk());

  EXPECT_THAT(icing->GetDocumentChanges(/*schemas=*/{}, MakePageToken(10)),
              EqualsProto(CreateExpectedResponse(
                  {CreateChangeSet("ns", "Email", {{"uri", 20}})},
                  /*last_sync_timestamp_ms=*/20)));
}

TEST_F(IcingSearchEngineChangeNotificationTest,
       ExpiredDocumentIsReturnedAsDeletedOncePurgedByOptimize) {
  std::unique_ptr<IcingSearchEngine> icing =
      CreateIcing(GetDefaultIcingOptions());
  ASSERT_THAT(icing->Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing->SetSchema(CreateSchema()).status(), ProtoIsOk());

  // Expires at 110.
  ASSERT_THAT(PutAt(icing.get(),
                    CreateExpiringDocument("ns", "expiring",
                                           /*creation_timestamp_ms=*/10,
                                           /*ttl_ms=*/100),
                    10)
                  .status(),
              ProtoIsOk());
  ASSERT_THAT(PutAt(icing.get(), "ns", "alive", "Email", 20).status(),
              ProtoIsOk());

  // Once expired, but before it's purged, the expired document is neither
  // returned as added nor as deleted.
  fake_clock_->SetSystemTimeMilliseconds(200);
  EXPECT_THAT(icing->GetDocumentChanges(/*schemas=*/{}, MakePageToken(0)),
              EqualsProto(CreateExpectedResponse(
                  {CreateChangeSet("ns", "Email", {{"alive", 20}})},
                  /*last_sync_timestamp_ms=*/20)));

  // Optimize purges the expired document, which is then returned as deleted at
  // the time of the Optimize.
  fake_clock_->SetSystemTimeMilliseconds(300);
  ASSERT_THAT(icing->Optimize().status(), ProtoIsOk());
  EXPECT_THAT(icing->GetDocumentChanges(/*schemas=*/{}, MakePageToken(20)),
              EqualsProto(CreateExpectedResponse(
                  {CreateChangeSet("ns", "Email", {{"expiring", 300}})},
                  /*last_sync_timestamp_ms=*/300)));
}

TEST_F(IcingSearchEngineChangeNotificationTest,
       ExpiredDocumentIsReturnedAsDeletedOncePurgedByHandleExpiredDocuments) {
  IcingSearchEngineOptions options = GetDefaultIcingOptions();
  // HandleExpiredDocuments requires delete propagation. Disable the background
  // task scheduler so that the expired document is only purged by the explicit
  // HandleExpiredDocuments call below.
  options.set_enable_delete_propagation_from(true);
  options.set_enable_background_task_scheduler(false);
  options.set_expired_document_purge_threshold_ms(0);
  std::unique_ptr<IcingSearchEngine> icing = CreateIcing(options);
  ASSERT_THAT(icing->Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing->SetSchema(CreateSchema()).status(), ProtoIsOk());

  // Expires at 110.
  ASSERT_THAT(PutAt(icing.get(),
                    CreateExpiringDocument("ns", "expiring",
                                           /*creation_timestamp_ms=*/10,
                                           /*ttl_ms=*/100),
                    10)
                  .status(),
              ProtoIsOk());

  fake_clock_->SetSystemTimeMilliseconds(200);
  HandleExpiredDocumentsResultProto handle_expired_result =
      icing->HandleExpiredDocuments();
  ASSERT_THAT(handle_expired_result.status(), ProtoIsOk());
  ASSERT_THAT(handle_expired_result.num_expired_documents(), Eq(1));

  EXPECT_THAT(icing->GetDocumentChanges(/*schemas=*/{}, MakePageToken(10)),
              EqualsProto(CreateExpectedResponse(
                  {CreateChangeSet("ns", "Email", {{"expiring", 200}})},
                  /*last_sync_timestamp_ms=*/200)));
}

TEST_F(IcingSearchEngineChangeNotificationTest, Pagination) {
  std::unique_ptr<IcingSearchEngine> icing =
      CreateIcing(GetDefaultIcingOptions());
  ASSERT_THAT(icing->Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing->SetSchema(CreateSchema()).status(), ProtoIsOk());

  // More documents than fit in a single page.
  constexpr int kNumDocuments = 2500;
  for (int i = 1; i <= kNumDocuments; ++i) {
    ASSERT_THAT(PutAt(icing.get(), "ns", "uri" + std::to_string(i), "Email",
                      /*timestamp_ms=*/i)
                    .status(),
                ProtoIsOk());
  }

  std::unordered_set<std::string> seen_uris;
  int num_pages = 0;
  DocumentChangeResponseProto::PaginationToken page_token = MakePageToken(0);
  DocumentChangeResponseProto response;
  do {
    response = icing->GetDocumentChanges(/*schemas=*/{}, page_token);
    ASSERT_THAT(response.status(), ProtoIsOk());
    ASSERT_THAT(response.document_change_sets(), SizeIs(1));
    for (const auto& token : response.document_change_sets(0).change_tokens()) {
      // Every change is returned exactly once, in timestamp order.
      EXPECT_TRUE(seen_uris.insert(token.uri()).second) << token.uri();
      EXPECT_THAT(token.operation_timestamp_ms(),
                  Gt(page_token.last_sync_timestamp_ms()));
    }
    page_token = response.next_page_token();
    ++num_pages;
  } while (response.has_more_changes());

  EXPECT_THAT(seen_uris, SizeIs(kNumDocuments));
  EXPECT_THAT(num_pages, Gt(1));
  EXPECT_THAT(page_token.last_sync_timestamp_ms(), Eq(kNumDocuments));

  // No more changes.
  response = icing->GetDocumentChanges(/*schemas=*/{}, page_token);
  ASSERT_THAT(response.status(), ProtoIsOk());
  EXPECT_THAT(response.document_change_sets(), IsEmpty());
  EXPECT_FALSE(response.has_more_changes());
}

TEST_F(IcingSearchEngineChangeNotificationTest,
       PaginationAcrossAddsAndDeletes) {
  std::unique_ptr<IcingSearchEngine> icing =
      CreateIcing(GetDefaultIcingOptions());
  ASSERT_THAT(icing->Initialize().status(), ProtoIsOk());
  ASSERT_THAT(icing->SetSchema(CreateSchema()).status(), ProtoIsOk());

  // Put documents at odd timestamps, then delete every other one at even
  // timestamps, so that each page has to merge adds and deletes.
  constexpr int kNumDocuments = 2000;
  for (int i = 0; i < kNumDocuments; ++i) {
    ASSERT_THAT(PutAt(icing.get(), "ns", "uri" + std::to_string(i), "Email",
                      /*timestamp_ms=*/2 * i + 1)
                    .status(),
                ProtoIsOk());
  }
  constexpr int64_t kSyncTimestampMs = 2 * kNumDocuments;
  for (int i = 0; i < kNumDocuments; i += 2) {
    ASSERT_THAT(DeleteAt(icing.get(), "ns", "uri" + std::to_string(i),
                         /*timestamp_ms=*/kSyncTimestampMs + 2 * i + 2)
                    .status(),
                ProtoIsOk());
    // Interleave updates to the remaining documents with the deletes.
    ASSERT_THAT(PutAt(icing.get(), "ns", "uri" + std::to_string(i + 1), "Email",
                      /*timestamp_ms=*/kSyncTimestampMs + 2 * i + 3)
                    .status(),
                ProtoIsOk());
  }

  std::unordered_set<std::string> seen_uris;
  int num_pages = 0;
  DocumentChangeResponseProto::PaginationToken page_token =
      MakePageToken(kSyncTimestampMs);
  DocumentChangeResponseProto response;
  do {
    response = icing->GetDocumentChanges(/*schemas=*/{}, page_token);
    ASSERT_THAT(response.status(), ProtoIsOk());
    ASSERT_THAT(response.document_change_sets(), SizeIs(1));
    int64_t previous_timestamp_ms = page_token.last_sync_timestamp_ms();
    for (const auto& token : response.document_change_sets(0).change_tokens()) {
      // Every change is returned exactly once, in timestamp order.
      EXPECT_TRUE(seen_uris.insert(token.uri()).second) << token.uri();
      EXPECT_THAT(token.operation_timestamp_ms(), Gt(previous_timestamp_ms));
      previous_timestamp_ms = token.operation_timestamp_ms();
    }
    page_token = response.next_page_token();
    ++num_pages;
  } while (response.has_more_changes());

  EXPECT_THAT(seen_uris, SizeIs(kNumDocuments));
  EXPECT_THAT(num_pages, Gt(1));
  // The last change is the update to the last document.
  EXPECT_THAT(page_token.last_sync_timestamp_ms(),
              Eq(kSyncTimestampMs + 2 * kNumDocuments - 1));
}

}  // namespace

}  // namespace lib
}  // namespace icing
