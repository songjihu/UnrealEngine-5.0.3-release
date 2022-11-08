// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Dasync.Collections;
using Jupiter.Implementation;

namespace Horde.Storage.Implementation
{
    public class MemoryReferencesStore : IReferencesStore
    {
        private readonly ConcurrentDictionary<string, MemoryStoreObject> _objects = new ConcurrentDictionary<string, MemoryStoreObject>();
        private readonly List<NamespaceId> _namespaces = new List<NamespaceId>();

        public MemoryReferencesStore()
        {

        }

        public Task<ObjectRecord> Get(NamespaceId ns, BucketId bucket, KeyId key)
        {
            if (_objects.TryGetValue(BuildKey(ns, bucket, key), out MemoryStoreObject? o))
            {
                return Task.FromResult(o.ToObjectRecord());
            }

            throw new ObjectNotFoundException(ns, bucket, key);
        }

        public Task Put(NamespaceId ns, BucketId bucket, KeyId key, BlobIdentifier blobHash, byte[] blob, bool isFinalized)
        {
            lock (_namespaces)
            {
                _namespaces.Add(ns);
            }

            MemoryStoreObject o = _objects.AddOrUpdate(BuildKey(ns, bucket, key),
                s => new MemoryStoreObject(ns, bucket, key, blobHash, blob, isFinalized),
                (s, o) => new MemoryStoreObject(ns, bucket, key, blobHash, blob, isFinalized));

            return Task.FromResult(o);
        }

        public Task Finalize(NamespaceId ns, BucketId bucket, KeyId key, BlobIdentifier blobHash)
        {
            if (!_objects.TryGetValue(BuildKey(ns, bucket, key), out MemoryStoreObject? o))
            {
                throw new ObjectNotFoundException(ns, bucket, key);
            }

            o.FinalizeObject();
            return Task.CompletedTask;
        }

        public Task UpdateLastAccessTime(NamespaceId ns, BucketId bucket, KeyId key, DateTime lastAccessTime)
        {
            if (!_objects.TryGetValue(BuildKey(ns, bucket, key), out MemoryStoreObject? o))
            {
                throw new ObjectNotFoundException(ns, bucket, key);
            }

            o.SetLastAccessTime(lastAccessTime);
            return Task.CompletedTask;
        }

        public async IAsyncEnumerator<ObjectRecord> GetOldestRecords(NamespaceId ns)
        {
            foreach (MemoryStoreObject o in _objects.Values.Where(o => o.Namespace == ns).OrderBy(o => o.LastAccessTime))
            {
                await Task.CompletedTask;
                yield return o.ToObjectRecord();
            }
        }

        public IAsyncEnumerator<NamespaceId> GetNamespaces()
        {
            return _namespaces.GetAsyncEnumerator();
        }

        public Task<long> Delete(NamespaceId ns, BucketId bucket, KeyId key)
        {
            if (!_objects.TryRemove(BuildKey(ns, bucket, key), out MemoryStoreObject? o))
            {
                throw new ObjectNotFoundException(ns, bucket, key);
            }

            return Task.FromResult(1L);
        }

        public Task<long> DropNamespace(NamespaceId ns)
        {
            lock (_namespaces)
            {
                _namespaces.Remove(ns);
            }

            List<string> objectToRemove = new List<string>();

            foreach (MemoryStoreObject o in _objects.Values)
            {
                if (o.Namespace == ns)
                {
                    objectToRemove.Add(BuildKey(o.Namespace, o.Bucket, o.Name));
                }
            }

            long removedCount = 0L;
            foreach (string key in objectToRemove)
            {
                if (_objects.TryRemove(key, out _))
                {
                    removedCount++;
                }
            }

            return Task.FromResult(removedCount);
        }

        public Task<long> DeleteBucket(NamespaceId ns, BucketId bucket)
        {
            List<string> objectToRemove = new List<string>();

            foreach (MemoryStoreObject o in _objects.Values)
            {
                if (o.Namespace == ns && o.Bucket == bucket)
                {
                    objectToRemove.Add(BuildKey(o.Namespace, o.Bucket, o.Name));
                }
            }

            long removedCount = 0L;
            foreach (string key in objectToRemove)
            {
                if (_objects.TryRemove(key, out _))
                {
                    removedCount++;
                }
            }

            return Task.FromResult(removedCount);
        }

        private static string BuildKey(NamespaceId ns, BucketId bucket, KeyId name)
        {
            return $"{ns}.{bucket}.{name}";
        }
    }

    public class MemoryStoreObject
    {
        public MemoryStoreObject(NamespaceId ns, BucketId bucket, KeyId key, BlobIdentifier blobHash, byte[] blob, bool isFinalized)
        {
            Namespace = ns;
            Bucket = bucket;
            Name = key;
            BlobHash = blobHash;
            Blob = blob;
            IsFinalized = isFinalized;
            LastAccessTime = DateTime.Now;
        }

        public NamespaceId Namespace { get; }
        public BucketId Bucket { get; }
        public KeyId Name { get; }
        public byte[] Blob { get; }
        public BlobIdentifier BlobHash { get; }

        public DateTime LastAccessTime { get; private set; }
        public bool IsFinalized { get; private set;}

        public void FinalizeObject()
        {
            IsFinalized = true;
        }
        public void SetLastAccessTime(DateTime lastAccessTime)
        {
            LastAccessTime = lastAccessTime;
        }

        public ObjectRecord ToObjectRecord()
        {
            return new ObjectRecord(Namespace, Bucket, Name, LastAccessTime, Blob, BlobHash, IsFinalized);
        }

    }
}
