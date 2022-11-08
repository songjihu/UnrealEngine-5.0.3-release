// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Net.Mime;
using System.Threading.Tasks;
using async_enumerable_dotnet;
using Cassandra;
using Dasync.Collections;
using Horde.Storage.Controllers;
using Horde.Storage.Implementation;
using Horde.Storage.Implementation.TransactionLog;
using Jupiter;
using Jupiter.Implementation;
using Microsoft.AspNetCore.TestHost;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Mvc;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Serilog;
using Logger = Serilog.Core.Logger;

namespace Horde.Storage.FunctionalTests.References
{

    [TestClass]
    public class ScyllaReplicationTests : ReplicationTests
    {
        protected override IEnumerable<KeyValuePair<string, string>> GetSettings()
        {
            return new[]
            {
                new KeyValuePair<string, string>("Horde.Storage:ReferencesDbImplementation", HordeStorageSettings.ReferencesDbImplementations.Scylla.ToString()),
                new KeyValuePair<string, string>("Horde.Storage:ReplicationLogWriterImplementation", HordeStorageSettings.ReplicationLogWriterImplementations.Scylla.ToString()),
            };
        }

        protected override async Task SeedDb(IServiceProvider provider)
        {
            IReferencesStore referencesStore = provider.GetService<IReferencesStore>()!;
            Assert.IsTrue(referencesStore.GetType() == typeof(ScyllaReferencesStore));

            IReplicationLog replicationLog = provider.GetService<IReplicationLog>()!;
            Assert.IsTrue(replicationLog.GetType() == typeof(ScyllaReplicationLog));

            await SeedTestData(referencesStore);
        }

        protected override async Task TeardownDb(IServiceProvider provider)
        {
            IScyllaSessionManager scyllaSessionManager = provider.GetService<IScyllaSessionManager>()!;

            ISession localKeyspace = scyllaSessionManager.GetSessionForLocalKeyspace();
            
            await Task.WhenAll(
                // remove replication log table as we expect it to be empty when starting the tests
                localKeyspace.ExecuteAsync(new SimpleStatement("DROP TABLE IF EXISTS replication_log;")),
                // remove the snapshots
                localKeyspace.ExecuteAsync(new SimpleStatement("DROP TABLE IF EXISTS replication_snapshot;")),
                // remove the namespaces
                localKeyspace.ExecuteAsync(new SimpleStatement("DROP TABLE IF EXISTS replication_namespace;"))
            );


        }
    }


    [TestClass]
    public class MemoryReplicationTests : ReplicationTests
    {
        protected override IEnumerable<KeyValuePair<string, string>> GetSettings()
        {
            return new[]
            {
                new KeyValuePair<string, string>("Horde.Storage:ReferencesDbImplementation", HordeStorageSettings.ReferencesDbImplementations.Memory.ToString()),
                new KeyValuePair<string, string>("Horde.Storage:ReplicationLogWriterImplementation", HordeStorageSettings.ReplicationLogWriterImplementations.Memory.ToString()),
            };
        }

        protected override async Task SeedDb(IServiceProvider provider)
        {
            IReferencesStore referencesStore = provider.GetService<IReferencesStore>()!;
            //verify we are using the expected refs store
            Assert.IsTrue(referencesStore.GetType() == typeof(MemoryReferencesStore));

            IReplicationLog replicationLog = provider.GetService<IReplicationLog>()!;
            Assert.IsTrue(replicationLog.GetType() == typeof(MemoryReplicationLog));

            await SeedTestData(referencesStore);
        }

        protected override Task TeardownDb(IServiceProvider provider)
        {
            return Task.CompletedTask;
        }
    }
    
    public abstract class ReplicationTests
    {
        private static TestServer? _server;
        private static HttpClient? _httpClient;
        protected IBlobStore _blobStore = null!;
        protected IReferencesStore _referencesStore = null!;
        private IReplicationLog _replicationLog = null!;

        protected readonly NamespaceId TestNamespace = new NamespaceId("test-namespace");
        protected readonly NamespaceId SnapshotNamespace = new NamespaceId("snapshot-namespace");
        protected readonly BucketId TestBucket = new BucketId("default");

        [TestInitialize]
        public async Task Setup()

        {
            IConfigurationRoot configuration = new ConfigurationBuilder()
                // we are not reading the base appSettings here as we want exact control over what runs in the tests
                .AddJsonFile("appsettings.Testing.json", true)
                .AddInMemoryCollection(GetSettings())
                .AddEnvironmentVariables()
                .Build();

            Logger logger = new LoggerConfiguration()
                .ReadFrom.Configuration(configuration)
                .CreateLogger();

            TestServer server = new TestServer(new WebHostBuilder()
                .UseConfiguration(configuration)
                .UseEnvironment("Testing")
                .UseSerilog(logger)
                .UseStartup<HordeStorageStartup>()
            );
            _httpClient = server.CreateClient();
            _server = server;

            _blobStore = _server.Services.GetService<IBlobStore>()!;
            _referencesStore = _server.Services.GetService<IReferencesStore>()!;
            _replicationLog = _server.Services.GetService<IReplicationLog>()!;

            await SeedDb(server.Services);
        }

        protected async Task SeedTestData(IReferencesStore referencesStore)
        {
            await Task.CompletedTask;
        }


        [TestCleanup]
        public async Task Teardown()
        {
            if (_server != null) 
                await TeardownDb(_server.Services);
        }

        protected abstract IEnumerable<KeyValuePair<string, string>> GetSettings();

        protected abstract Task SeedDb(IServiceProvider provider);
        protected abstract Task TeardownDb(IServiceProvider provider);
        
        
        [TestMethod]
        public async Task ReplicationLogCreation()
        {
            CompactBinaryWriter writer = new CompactBinaryWriter();
            writer.BeginObject();
            writer.AddString("thisIsAField", "stringField");
            writer.EndObject();

            byte[] objectData = writer.Save();
            BlobIdentifier objectHash = BlobIdentifier.FromBlob(objectData);

            HttpContent requestContent = new ByteArrayContent(objectData);
            requestContent.Headers.ContentType = new MediaTypeHeaderValue(CustomMediaTypeNames.UnrealCompactBinary);
            requestContent.Headers.Add(CommonHeaders.HashHeaderName, objectHash.ToString());

            {
                HttpResponseMessage result = await _httpClient!.PutAsync(requestUri: $"api/v1/refs/{TestNamespace}/{TestBucket}/newReferenceObject.uecb", requestContent);
                result.EnsureSuccessStatusCode();
            }

            {
                HttpResponseMessage result = await _httpClient!.PutAsync(requestUri: $"api/v1/refs/{TestNamespace}/{TestBucket}/secondObject.uecb", requestContent);
                result.EnsureSuccessStatusCode();
            }

            {
                HttpResponseMessage result = await _httpClient!.PutAsync(requestUri: $"api/v1/refs/{TestNamespace}/{TestBucket}/thirdObject.uecb", requestContent);
                result.EnsureSuccessStatusCode();
            }
            
            {
                HttpResponseMessage result = await _httpClient!.GetAsync(requestUri: $"api/v1/replication-log/incremental/{TestNamespace}");
                result.EnsureSuccessStatusCode();
                string s = await result.Content.ReadAsStringAsync();
                
                Assert.AreEqual(result!.Content.Headers.ContentType!.MediaType, MediaTypeNames.Application.Json);

                ReplicationLogEvents? events = await result.Content.ReadAsAsync<ReplicationLogEvents>();
                Assert.IsNotNull(events);
                Assert.AreEqual(3, events!.Events.Count);

                // parse the events returned, make sure they are in the right order
                {
                    ReplicationLogEvent e = events.Events[0];
                    Assert.AreEqual(TestNamespace, e.Namespace);
                    Assert.AreEqual(TestBucket, e.Bucket);
                    Assert.AreEqual("newReferenceObject", e.Key.ToString());
                    Assert.AreEqual(objectHash, e.Blob);
                }

                {
                    ReplicationLogEvent e = events.Events[1];
                    Assert.AreEqual(TestNamespace, e.Namespace);
                    Assert.AreEqual(TestBucket, e.Bucket);
                    Assert.AreEqual("secondObject", e.Key.ToString());
                    Assert.AreEqual(objectHash, e.Blob);
                }

                {
                    ReplicationLogEvent e = events.Events[2];
                    Assert.AreEqual(TestNamespace, e.Namespace);
                    Assert.AreEqual(TestBucket, e.Bucket);
                    Assert.AreEqual("thirdObject", e.Key.ToString());
                    Assert.AreEqual(objectHash, e.Blob);
                }
            }

            CollectionAssert.AreEqual(new [] {TestNamespace}, await _replicationLog.GetNamespaces().ToArrayAsync());
        }

         
        [TestMethod]
        public async Task ReplicationLogReading()
        {
            CompactBinaryWriter writer = new CompactBinaryWriter();
            writer.BeginObject();
            writer.AddString("thisIsAField", "stringField");
            writer.EndObject();

            byte[] objectData = writer.Save();
            BlobIdentifier objectHash = BlobIdentifier.FromBlob(objectData);

            DateTime oldestTimestamp = DateTime.Now.AddDays(-1);
            (string eventBucket, Guid eventId) = await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("firstObject"), objectHash, oldestTimestamp);
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("secondObject"), objectHash, oldestTimestamp.AddHours(2.0));
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("thirdObject"), objectHash, oldestTimestamp.AddHours(3.0));
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("fourthObject"), objectHash, oldestTimestamp.AddDays(0.9));

            {
                HttpResponseMessage result = await _httpClient!.GetAsync(requestUri: $"api/v1/replication-log/incremental/{TestNamespace}?lastBucket={eventBucket}&lastEvent={eventId}");
                result.EnsureSuccessStatusCode();
                string s = await result.Content.ReadAsStringAsync();
                
                Assert.AreEqual(result!.Content.Headers.ContentType!.MediaType, MediaTypeNames.Application.Json);

                ReplicationLogEvents? events = await result.Content.ReadAsAsync<ReplicationLogEvents>();
                Assert.IsNotNull(events);
                Assert.AreEqual(3, events!.Events.Count);

                // we will not get the first event, as if we ever were to fetch all events we could potentially have events that are missed and snapshots are used instead

                // parse the events returned, make sure they are in the right order
                {
                    ReplicationLogEvent e = events.Events[0];
                    Assert.AreEqual(TestNamespace, e.Namespace);
                    Assert.AreEqual(TestBucket, e.Bucket);
                    Assert.AreEqual("secondObject", e.Key.ToString());
                    Assert.AreEqual(objectHash, e.Blob);
                }

                {
                    ReplicationLogEvent e = events.Events[1];
                    Assert.AreEqual(TestNamespace, e.Namespace);
                    Assert.AreEqual(TestBucket, e.Bucket);
                    Assert.AreEqual("thirdObject", e.Key.ToString());
                    Assert.AreEqual(objectHash, e.Blob);
                }

                {
                    ReplicationLogEvent e = events.Events[2];
                    Assert.AreEqual(TestNamespace, e.Namespace);
                    Assert.AreEqual(TestBucket, e.Bucket);
                    Assert.AreEqual("fourthObject", e.Key.ToString());
                    Assert.AreEqual(objectHash, e.Blob);
                }
            }
         
            CollectionAssert.AreEqual(new [] {TestNamespace}, await _replicationLog.GetNamespaces().ToArrayAsync());
        }

        [TestMethod]
        public async Task ReplicationLogResume()
        {
            CompactBinaryWriter writer = new CompactBinaryWriter();
            writer.BeginObject();
            writer.AddString("thisIsAField", "stringField");
            writer.EndObject();

            byte[] objectData = writer.Save();
            BlobIdentifier objectHash = BlobIdentifier.FromBlob(objectData);

            DateTime oldestTimestamp = DateTime.Now.AddDays(-3);
            // insert multiple objects in the same time bucket, verifying that we correctly get only the objects after this
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("firstObject"), objectHash, oldestTimestamp);
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("secondObject"), objectHash, oldestTimestamp.AddHours(2.0));
            (string eventBucket, Guid eventId) = await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("thirdObject"), objectHash, oldestTimestamp.AddHours(2.1));
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("fourthObject"), objectHash, oldestTimestamp.AddHours(2.11));
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("fifthObject"), objectHash, oldestTimestamp.AddHours(2.12));
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("sixthObject"), objectHash, oldestTimestamp.AddDays(2.13));

            {
                HttpResponseMessage result = await _httpClient!.GetAsync(requestUri: $"api/v1/replication-log/incremental/{TestNamespace}?lastBucket={eventBucket}&lastEvent={eventId}");
                result.EnsureSuccessStatusCode();
                string s = await result.Content.ReadAsStringAsync();
                
                Assert.AreEqual(result!.Content.Headers.ContentType!.MediaType, MediaTypeNames.Application.Json);

                ReplicationLogEvents? events = await result.Content.ReadAsAsync<ReplicationLogEvents>();
                Assert.IsNotNull(events);
                Assert.AreEqual(3, events!.Events.Count);

                // we will not get the first event, as if we ever were to fetch all events we could potentially have events that are missed and snapshots are used instead

                // parse the events returned, make sure they are in the right order
                {
                    ReplicationLogEvent e = events.Events[0];
                    Assert.AreEqual(TestNamespace, e.Namespace);
                    Assert.AreEqual(TestBucket, e.Bucket);
                    Assert.AreEqual("fourthObject", e.Key.ToString());
                    Assert.AreEqual(objectHash, e.Blob);
                }

                {
                    ReplicationLogEvent e = events.Events[1];
                    Assert.AreEqual(TestNamespace, e.Namespace);
                    Assert.AreEqual(TestBucket, e.Bucket);
                    Assert.AreEqual("fifthObject", e.Key.ToString());
                    Assert.AreEqual(objectHash, e.Blob);
                }

                {
                    ReplicationLogEvent e = events.Events[2];
                    Assert.AreEqual(TestNamespace, e.Namespace);
                    Assert.AreEqual(TestBucket, e.Bucket);
                    Assert.AreEqual("sixthObject", e.Key.ToString());
                    Assert.AreEqual(objectHash, e.Blob);
                }
            }
         
            CollectionAssert.AreEqual(new [] {TestNamespace}, await _replicationLog.GetNamespaces().ToArrayAsync());
        }

        [TestMethod]
        public async Task ReplicationLogReadingLimit()
        {
            CompactBinaryWriter writer = new CompactBinaryWriter();
            writer.BeginObject();
            writer.AddString("thisIsAField", "stringField");
            writer.EndObject();

            byte[] objectData = writer.Save();
            BlobIdentifier objectHash = BlobIdentifier.FromBlob(objectData);

            DateTime oldestTimestamp = DateTime.Now.AddDays(-1);
            (string eventBucket, Guid eventId) = await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("firstObject"), objectHash, oldestTimestamp);
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("secondObject"), objectHash, oldestTimestamp.AddHours(1));
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("thirdObject"), objectHash, oldestTimestamp.AddHours(1.5));
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("fourthObject"), objectHash, oldestTimestamp.AddDays(0.9));

            // start from the second event
            const int eventsToFetch = 2;
            {
                HttpResponseMessage result = await _httpClient!.GetAsync(requestUri: $"api/v1/replication-log/incremental/{TestNamespace}?lastBucket={eventBucket}&lastEvent={eventId}&count={eventsToFetch}");
                result.EnsureSuccessStatusCode();
                string s = await result.Content.ReadAsStringAsync();
                
                Assert.AreEqual(result!.Content.Headers.ContentType!.MediaType, MediaTypeNames.Application.Json);

                ReplicationLogEvents? events = await result.Content.ReadAsAsync<ReplicationLogEvents>();
                Assert.IsNotNull(events);
                Assert.AreEqual(eventsToFetch, events!.Events.Count);

                // parse the events returned, make sure they are in the right order
                {
                    ReplicationLogEvent e = events.Events[0];
                    Assert.AreEqual(TestNamespace, e.Namespace);
                    Assert.AreEqual(TestBucket, e.Bucket);
                    Assert.AreEqual("secondObject", e.Key.ToString());
                    Assert.AreEqual(objectHash, e.Blob);
                }

                {
                    ReplicationLogEvent e = events.Events[1];
                    Assert.AreEqual(TestNamespace, e.Namespace);
                    Assert.AreEqual(TestBucket, e.Bucket);
                    Assert.AreEqual("thirdObject", e.Key.ToString());
                    Assert.AreEqual(objectHash, e.Blob);
                }
            }
            
            CollectionAssert.AreEqual(new [] {TestNamespace}, await _replicationLog.GetNamespaces().ToArrayAsync());
        }

        [TestMethod]
        public async Task ReplicationLogInvalidBucket()
        {
            // the namespace exists but the bucket does not
            CompactBinaryWriter writer = new CompactBinaryWriter();
            writer.BeginObject();
            writer.AddString("thisIsAField", "stringField");
            writer.EndObject();

            byte[] objectData = writer.Save();
            BlobIdentifier objectHash = BlobIdentifier.FromBlob(objectData);

            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("firstObject"), objectHash, DateTime.Now.AddDays(-1));

            string eventBucket = "rep-00000000";
            Guid eventId = Guid.NewGuid();

            {
                HttpResponseMessage result = await _httpClient!.GetAsync(requestUri: $"api/v1/replication-log/incremental/{TestNamespace}?lastBucket={eventBucket}&lastEvent={eventId}");
                Assert.AreEqual(HttpStatusCode.BadRequest, result.StatusCode);
                
                string s = await result.Content.ReadAsStringAsync();
                
                ProblemDetails? problem = await result.Content.ReadFromJsonAsync<ProblemDetails?>();
                Assert.IsNotNull(problem);
            }

            CollectionAssert.AreEqual(new [] {TestNamespace}, await _replicationLog.GetNamespaces().ToArrayAsync());
        }

        [TestMethod]
        public async Task ReplicationLogEmptyLog()
        {
            // the namespace does not exist
            {
                HttpResponseMessage result = await _httpClient!.GetAsync(requestUri: $"api/v1/replication-log/incremental/{TestNamespace}");
                Assert.AreEqual(HttpStatusCode.NotFound, result.StatusCode);
                string s = await result.Content.ReadAsStringAsync();
                
                ProblemDetails? problem = await result.Content.ReadFromJsonAsync<ProblemDetails?>();
                Assert.IsNotNull(problem);
            }

            CollectionAssert.AreEqual(Array.Empty<NamespaceId>(), await _replicationLog.GetNamespaces().ToArrayAsync());
        }

        [TestMethod]
        public async Task ReplicationLogNoIncrementalLogAvailable()
        {
            CompactBinaryWriter writer = new CompactBinaryWriter();
            writer.BeginObject();
            writer.AddString("thisIsAField", "stringField");
            writer.EndObject();

            byte[] objectData = writer.Save();
            BlobIdentifier objectHash = BlobIdentifier.FromBlob(objectData);

            DateTime oldestTimestamp = DateTime.Now.AddDays(-1);
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("firstObject"), objectHash, oldestTimestamp);
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("secondObject"), objectHash, oldestTimestamp.AddHours(1));
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("thirdObject"), objectHash, oldestTimestamp.AddHours(1.5));
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("fourthObject"), objectHash, oldestTimestamp.AddDays(0.9));

            // create a snapshot
            ReplicationLogSnapshotBuilder snapshotBuilder = ActivatorUtilities.CreateInstance<ReplicationLogSnapshotBuilder>(_server!.Services);
            Assert.IsNotNull(snapshotBuilder);
            BlobIdentifier snapshotBlobId = await snapshotBuilder.BuildSnapshot(TestNamespace, SnapshotNamespace);
            Assert.IsTrue(await _blobStore.Exists(SnapshotNamespace, snapshotBlobId));

            // use a bucket that does not exist, should raise a message to use a snapshot instead
            string bucketThatDoesNotExist = "rep-0000";
            {
                HttpResponseMessage result = await _httpClient!.GetAsync(requestUri: $"api/v1/replication-log/incremental/{TestNamespace}?lastBucket={bucketThatDoesNotExist}&lastEvent={Guid.NewGuid()}");
                Assert.AreEqual(HttpStatusCode.BadRequest, result.StatusCode);
                string s = await result.Content.ReadAsStringAsync();
                
                ProblemDetails? problem = await result.Content.ReadFromJsonAsync<ProblemDetails?>();
                Assert.IsNotNull(problem);
                Assert.AreEqual("http://jupiter.epicgames.com/replication/useSnapshot", problem!.Type);
                Assert.IsTrue(problem.Extensions.ContainsKey("SnapshotId"));
                Assert.AreEqual(snapshotBlobId, new BlobIdentifier(problem.Extensions["SnapshotId"]!.ToString()!));
            }
            
        }

        [TestMethod]
        public async Task ReplicationLogSnapshotCreation()
        {
            CompactBinaryWriter writer = new CompactBinaryWriter();
            writer.BeginObject();
            writer.AddString("thisIsAField", "stringField");
            writer.EndObject();

            byte[] objectData = writer.Save();
            BlobIdentifier objectHash = BlobIdentifier.FromBlob(objectData);

            DateTime oldestTimestamp = DateTime.Now.AddDays(-1);
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("firstObject"), objectHash, oldestTimestamp);
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("secondObject"), objectHash, oldestTimestamp.AddHours(1));
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("thirdObject"), objectHash, oldestTimestamp.AddHours(1.5));
            (string lastEventBucket, Guid lastEventId) = await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("fourthObject"), objectHash, oldestTimestamp.AddDays(0.7));

            // verify the objects were added
            {
                List<ReplicationLogEvent> logEvents = await _replicationLog.Get(TestNamespace, null, null).ToListAsync();
                Assert.AreEqual(4, logEvents.Count);

                // parse the events returned, make sure they are in the right order
                {
                    ReplicationLogEvent e = logEvents[0];
                    Assert.AreEqual(TestNamespace, e.Namespace);
                    Assert.AreEqual(TestBucket, e.Bucket);
                    Assert.AreEqual("firstObject", e.Key.ToString());
                    Assert.AreEqual(objectHash, e.Blob);
                }

                {
                    ReplicationLogEvent e = logEvents[1];
                    Assert.AreEqual(TestNamespace, e.Namespace);
                    Assert.AreEqual(TestBucket, e.Bucket);
                    Assert.AreEqual("secondObject", e.Key.ToString());
                    Assert.AreEqual(objectHash, e.Blob);
                }

                {
                    ReplicationLogEvent e = logEvents[2];
                    Assert.AreEqual(TestNamespace, e.Namespace);
                    Assert.AreEqual(TestBucket, e.Bucket);
                    Assert.AreEqual("thirdObject", e.Key.ToString());
                    Assert.AreEqual(objectHash, e.Blob);
                }

                {
                    ReplicationLogEvent e = logEvents[3];
                    Assert.AreEqual(TestNamespace, e.Namespace);
                    Assert.AreEqual(TestBucket, e.Bucket);
                    Assert.AreEqual("fourthObject", e.Key.ToString());
                    Assert.AreEqual(objectHash, e.Blob);
                }
            }

            // verify there are no previous snapshots
            Assert.AreEqual(0, await _replicationLog.GetSnapshots(TestNamespace).Count().FirstAsync());

            // create a snapshot
            ReplicationLogSnapshotBuilder snapshotBuilder = ActivatorUtilities.CreateInstance<ReplicationLogSnapshotBuilder>(_server!.Services);
            Assert.IsNotNull(snapshotBuilder);
            BlobIdentifier snapshotBlobId = await snapshotBuilder.BuildSnapshot(TestNamespace, SnapshotNamespace);
            Assert.IsTrue(await _blobStore.Exists(SnapshotNamespace, snapshotBlobId));

            SnapshotInfo? snapshotInfo = await _replicationLog.GetSnapshots(TestNamespace).FirstAsync();
            Assert.IsNotNull(snapshotInfo);
            Assert.AreEqual(snapshotBlobId, snapshotInfo.SnapshotBlob);

            BlobContents blobContents = await _blobStore.GetObject(SnapshotNamespace, snapshotBlobId);
            ReplicationLogSnapshot snapshot = await ReplicationLogSnapshot.DeserializeSnapshot(blobContents.Stream);

            Assert.AreEqual(lastEventId, snapshot.LastEvent);
            Assert.AreEqual(lastEventBucket, snapshot.LastBucket);
            
            Assert.IsTrue(snapshot.LiveObjects.Any(o => o.Bucket == TestBucket && o.Key == new KeyId("firstObject")));
            Assert.IsTrue(snapshot.LiveObjects.Any(o => o.Bucket == TestBucket && o.Key == new KeyId("secondObject")));
            Assert.IsTrue(snapshot.LiveObjects.Any(o => o.Bucket == TestBucket && o.Key == new KeyId("thirdObject")));
            Assert.IsTrue(snapshot.LiveObjects.Any(o => o.Bucket == TestBucket && o.Key == new KeyId("fourthObject")));
        }


        [TestMethod]
        public async Task ReplicationLogSnapshotQuerying()
        {
            CompactBinaryWriter writer = new CompactBinaryWriter();
            writer.BeginObject();
            writer.AddString("thisIsAField", "stringField");
            writer.EndObject();

            byte[] objectData = writer.Save();
            BlobIdentifier objectHash = BlobIdentifier.FromBlob(objectData);

            DateTime oldestTimestamp = DateTime.Now.AddDays(-1);
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("firstObject"), objectHash, oldestTimestamp);
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("secondObject"), objectHash, oldestTimestamp.AddHours(1));
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("thirdObject"), objectHash, oldestTimestamp.AddHours(1.5));
            (string lastEventBucket, Guid lastEventId) = await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("fourthObject"), objectHash, oldestTimestamp.AddDays(0.9));

            // verify the objects were added
            List<ReplicationLogEvent> logEvents = await _replicationLog.Get(TestNamespace, null, null).ToListAsync();
            Assert.AreEqual(4, logEvents.Count);

            // verify there are no previous snapshots
            Assert.AreEqual(0, await _replicationLog.GetSnapshots(TestNamespace).Count().FirstAsync());

            // create a snapshot
            ReplicationLogSnapshotBuilder snapshotBuilder = ActivatorUtilities.CreateInstance<ReplicationLogSnapshotBuilder>(_server!.Services);
            Assert.IsNotNull(snapshotBuilder);
            BlobIdentifier snapshotBlobId = await snapshotBuilder.BuildSnapshot(TestNamespace, SnapshotNamespace);
            Assert.IsTrue(await _blobStore.Exists(SnapshotNamespace, snapshotBlobId));

            SnapshotInfo? snapshotInfo = await _replicationLog.GetSnapshots(TestNamespace).FirstAsync();
            Assert.AreEqual(snapshotBlobId, snapshotInfo.SnapshotBlob);

            // make sure the snapshot is returned by the rest api
            {
                HttpResponseMessage result = await _httpClient!.GetAsync(requestUri: $"api/v1/replication-log/snapshots/{TestNamespace}");
                result.EnsureSuccessStatusCode();
                string s = await result.Content.ReadAsStringAsync();
                
                Assert.AreEqual(result!.Content.Headers.ContentType!.MediaType, MediaTypeNames.Application.Json);

                ReplicationLogSnapshots snapshots = await result.Content.ReadAsAsync<ReplicationLogSnapshots>();
                Assert.IsNotNull(snapshots);
                Assert.AreEqual(1, snapshots.Snapshots.Count);

                SnapshotInfo foundSnapshot = snapshots.Snapshots[0];

                Assert.AreEqual(snapshotBlobId, foundSnapshot.SnapshotBlob);
                BlobContents blobContents = await _blobStore.GetObject(SnapshotNamespace, snapshotBlobId);
                ReplicationLogSnapshot snapshot = await ReplicationLogSnapshot.DeserializeSnapshot(blobContents.Stream);

                Assert.AreEqual(lastEventBucket, snapshot.LastBucket);
                Assert.AreEqual(lastEventId, snapshot.LastEvent);
            }
        }

        // builds a snapshot and make sure we can resume iterating after it
        [TestMethod]
        public async Task ReplicationLogSnapshotResume()
        {
            CompactBinaryWriter writer = new CompactBinaryWriter();
            writer.BeginObject();
            writer.AddString("thisIsAField", "stringField");
            writer.EndObject();

            byte[] objectData = writer.Save();
            BlobIdentifier objectHash = BlobIdentifier.FromBlob(objectData);

            DateTime oldestTimestamp = DateTime.Now.AddDays(-1);
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("firstObject"), objectHash, oldestTimestamp);
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("secondObject"), objectHash, oldestTimestamp.AddHours(1));
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("thirdObject"), objectHash, oldestTimestamp.AddHours(1.5));
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("fourthObject"), objectHash, oldestTimestamp.AddDays(0.9));

            // verify the objects were added
            List<ReplicationLogEvent> logEvents = await _replicationLog.Get(TestNamespace, null, null).ToListAsync();
            Assert.AreEqual(4, logEvents.Count);

            // verify there are no previous snapshots
            Assert.AreEqual(0, await _replicationLog.GetSnapshots(TestNamespace).Count().FirstAsync());

            // create a snapshot
            ReplicationLogSnapshotBuilder snapshotBuilder = ActivatorUtilities.CreateInstance<ReplicationLogSnapshotBuilder>(_server!.Services);
            Assert.IsNotNull(snapshotBuilder);
            BlobIdentifier snapshotBlobId = await snapshotBuilder.BuildSnapshot(TestNamespace, SnapshotNamespace);
            Assert.IsTrue(await _blobStore.Exists(SnapshotNamespace, snapshotBlobId));

            SnapshotInfo? snapshotInfo = await _replicationLog.GetSnapshots(TestNamespace).FirstAsync();
            Assert.AreEqual(snapshotBlobId, snapshotInfo.SnapshotBlob);

            BlobContents blobContents = await _blobStore.GetObject(SnapshotNamespace, snapshotBlobId);
            ReplicationLogSnapshot snapshot = await ReplicationLogSnapshot.DeserializeSnapshot(blobContents.Stream);

            // insert more events
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("fifthObject"), objectHash, oldestTimestamp.AddDays(0.91));
            await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId("sixthObject"), objectHash, oldestTimestamp.AddDays(0.92));

            // verify the new events can be found when resuming from the snapshot
            {
                HttpResponseMessage result = await _httpClient!.GetAsync(requestUri: $"api/v1/replication-log/incremental/{TestNamespace}?lastBucket={snapshot.LastBucket}&lastEvent={snapshot.LastEvent}");
                result.EnsureSuccessStatusCode();
                
                Assert.AreEqual(result!.Content.Headers.ContentType!.MediaType, MediaTypeNames.Application.Json);

                ReplicationLogEvents? events = await result.Content.ReadAsAsync<ReplicationLogEvents>();
                Assert.IsNotNull(events);
                Assert.AreEqual(2, events!.Events.Count);

                // parse the events returned, make sure they are in the right order
                {
                    ReplicationLogEvent e = events.Events[0];
                    Assert.AreEqual(TestNamespace, e.Namespace);
                    Assert.AreEqual(TestBucket, e.Bucket);
                    Assert.AreEqual("fifthObject", e.Key.ToString());
                    Assert.AreEqual(objectHash, e.Blob);
                }

                {
                    ReplicationLogEvent e = events.Events[1];
                    Assert.AreEqual(TestNamespace, e.Namespace);
                    Assert.AreEqual(TestBucket, e.Bucket);
                    Assert.AreEqual("sixthObject", e.Key.ToString());
                    Assert.AreEqual(objectHash, e.Blob);
                }
            }
        }

        [TestMethod]
        public async Task ReplicationLogSnapshotCleanup()
        {
            const int maxCountOfSnapshots = 10;
            int countOfSnapshotsToCreate = maxCountOfSnapshots + 2;

            CompactBinaryWriter writer = new CompactBinaryWriter();
            writer.BeginObject();
            writer.AddString("thisIsAField", "stringField");
            writer.EndObject();

            byte[] objectData = writer.Save();
            BlobIdentifier objectHash = BlobIdentifier.FromBlob(objectData);

            List<BlobIdentifier> createdSnapshots = new List<BlobIdentifier>();
            for (int i = 0; i < countOfSnapshotsToCreate ; i++)
            {
                await _replicationLog.InsertAddEvent(TestNamespace, TestBucket, new KeyId($"object {i}"), objectHash);

                ReplicationLogSnapshotBuilder snapshotBuilder = ActivatorUtilities.CreateInstance<ReplicationLogSnapshotBuilder>(_server!.Services);
                Assert.IsNotNull(snapshotBuilder);
                BlobIdentifier snapshotBlobId = await snapshotBuilder.BuildSnapshot(TestNamespace, SnapshotNamespace);
                Assert.IsTrue(await _blobStore.Exists(SnapshotNamespace, snapshotBlobId));
                createdSnapshots.Add(snapshotBlobId);
            }

            List<BlobIdentifier> snapshots = await _replicationLog.GetSnapshots(TestNamespace).Select(info => info.SnapshotBlob).ToListAsync();
            // snapshots are returned newest first so we inverse this order
            snapshots.Reverse();

            // verify we hit the max number of snapshots
            Assert.AreEqual(maxCountOfSnapshots, snapshots.Count);

            // the first two snapshots should have been removed
            createdSnapshots.RemoveAt(0);
            createdSnapshots.RemoveAt(0);

            CollectionAssert.AreEqual(createdSnapshots, snapshots);
        }
    }
}
