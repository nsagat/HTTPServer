# HTTP Server

A multithreaded HTTP server written in C that supports concurrent `GET` and `PUT` requests with fine-grained synchronization and file-level atomicity.  

This project extends my **ASGN2** single-threaded HTTP server (basic `GET`/`PUT`) by introducing multithreading and per-file reader/writer locking to handle concurrent requests safely and efficiently.

---

## Overview

The main goal of **ASGN4** was to transform a previously single-threaded server into a **concurrent, thread-safe HTTP server** capable of handling multiple requests simultaneously — without data races, deadlocks, or inconsistent file states.  

To achieve this, I introduced **per-path synchronization** using custom **reader-writer locks**, ensuring correct concurrency semantics for both `GET` and `PUT` operations.

---

## Key Features

- **Multithreading:**  
  Uses worker threads to handle multiple client connections concurrently.

- **Fine-Grained Synchronization:**  
  Each unique file path gets its own `rwlock_t` for precise control over concurrent access.

- **Reader–Writer Locking:**  
  - Multiple readers (`GET` requests) can access the same file simultaneously.  
  - Writers (`PUT` requests) gain exclusive access.  
  - Writers to different files proceed in parallel without blocking each other.

- **Atomicity Guarantees:**  
  `PUT` operations on the same file are serialized correctly to prevent race conditions.  

- **Scalable Design:**  
  The linked-list "path lock map" dynamically manages locks for new files as they appear.

---

## Architecture

- **Thread Pool:** Handles multiple requests concurrently.  
- **Path Lock Manager:** Maintains a mapping between file paths and their corresponding `rwlock_t` locks via a custom linked-list implementation.  
- **HTTP Request Parser:** Reuses the logic from previous assignments to handle `GET` and `PUT`.  
- **Error Handling:** Ensures proper HTTP response codes and prevents inconsistent file states under load.  

---

## How It Works

1. A client connects and sends a `GET` or `PUT` request.  
2. The server assigns a thread from the pool to process it.  
3. The thread acquires the appropriate reader or writer lock for the requested path:  
   - **GET:** acquires a read lock → multiple readers allowed.  
   - **PUT:** acquires a write lock → exclusive access.  
4. After the operation, the lock is released, allowing other threads to proceed.  

This design allows:
- Parallel `PUT`s to different files.  
- Concurrent `GET`s on the same file.  
- Safe serialization when `PUT` and `GET` collide on the same file.

---

## Technologies Used

- **C (POSIX Threads)**  
- **Custom Reader/Writer Locks (`pthread_rwlock_t`)**  
- **Linked-list based Path Lock Manager**  
- **HTTP 1.1 Request Handling**

---
## Testing

To test concurrent access:
- Run multiple `curl` or `ab` (ApacheBench) commands simultaneously.  
- Observe that:
  - Parallel `PUT`s to *different* files proceed concurrently.  
  - `GET`s on the same file can run together.  
  - `PUT`s to the *same* file serialize correctly.  

Example:
```bash
curl -T file1.txt localhost:8080/file1 &
curl -T file2.txt localhost:8080/file2 &
curl localhost:8080/file1 &
