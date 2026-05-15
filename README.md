# sptd_lock: Cross-Container Userspace Lock Manager

`sptd_lock` is a lightweight, namespace-aware Linux kernel module. It provides a sysfs interface (`/sys/devices/platform/sptd_lock/lock_mgr`) to orchestrate advisory locking between independent userspace processes, threads, and isolated Docker containers.

## Summary of Features

*   🔒 **Namespace Aware:** Correctly tracks and validates PIDs inside their specific PID namespaces (Docker/containers).
*   💀 **Deadlock Mitigation:** Automatically detects and force-releases orphaned locks if the owning process crashes or gets killed (`SIGKILL`).
*   📊 **Status Dashboard:** Reading the sysfs attribute yields a clean, tab-aligned real-time state table of all defined lock slots.
*   ⚡ **Zero Dependencies:** Pure sysfs engine requiring no specialized userspace CLI binaries—interactable directly via standard shell built-ins.

---

## Technical Details

*   **Maximum Concurrent Locks:** 16 (`MAX_LOCKS`)
*   **Maximum Lock Name Length:** 64 characters (`MAX_NAME_LEN`)
*   **Kernel Primitives:** Backed by kernel semaphores allowing safe, asynchronous cross-thread cross-boundary releases.

---

# Architectural Comparison: Kernel Semaphore vs. SHM + Process-Shared Mutex

Normally for Inter-Process Communication (IPC) synchronization under Linux, we use a **Shared Memory (SHM) + Process-Shared Thread Mutex** (`pthread_mutex_t` with `PTHREAD_PROCESS_SHARED`), here we use **Kernel Semaphore**. First let's look at the differences:

---

## 1. Core Architectural Differences

The primary differentiator between these two mechanisms is the execution space where the synchronization logic takes place:

*   **Kernel Semaphore:** The synchronization structure resides entirely within kernel space. Every lock acquisition or release operation requires a trap to the operating system kernel via a system call (`semop` or `sem_wait`).
*   **SHM + Process-Shared Mutex:** The synchronization structure (`pthread_mutex_t`) resides inside a shared memory segment mapped directly into the virtual address spaces of the participating processes. It leverages the Linux `futex` (Fast Userspace Mutex) architecture.

---

## 2. Technical Feature Matrix


| Technical Metric | Kernel Semaphore Locks | SHM + Process-Shared Mutex |
| :--- | :--- | :--- |
| **Memory Allocation** | Kernel memory space. | Shared user memory page (`shmget` / `mmap`). |
| **Uncontended Lock Cost** | **High Overhead** (Requires a system call every time). | **Near Zero** (Executed via atomic CPU instructions in userspace). |
| **Contended Lock Cost** | **High Overhead** (Immediate transition to kernel wait queue). | **Adaptive Overhead** (Can spin briefly in userspace before sleeping). |
| **Data Locality** | Separated from data payload (Causes CPU cache misses). | Colocated with data payload (High L1/L2 cache efficiency). |
| **Crash Safety** | Native automatic cleanup via kernel `SEM_UNDO`. | Manual cleanup required via `PTHREAD_MUTEX_ROBUST`. |
| **Resource Limits** | Restricted by OS IPC system configurations (`semmni`, `semmsl`). | Bound only by standard virtual memory limits. |
| **Security & Sandbox Isolation** | **High Isolation** (Exposed via discrete sysfs endpoints). | **Low Isolation** (Relies on wide `/dev/shm` filesystem access). |

---

## 3. Performance Dynamics & Execution Paths

### The Fast-Path Optimization
The **SHM + Process-Shared Mutex** utilizes a "fast-path" optimization. When a process attempts to lock an uncontended mutex, the CPU executes an atomic operation (e.g., `CMPXCHG` on x86 architectures) entirely within userspace. 

The kernel is completely bypassed during uncontended operations. The system only drops into a "slow-path" (issuing a `futex` system call to put the process to sleep) if a collision occurs.

---

## 4. Why We Use Kernel Semaphore: Sandboxing & Container Security

While **SHM + Process-Shared Mutex** offers superior performance, it introduces significant deployment roadblocks in locked-down or containerized environments due to file path requirements:

### The `/dev/shm` Security Bottleneck
Standard POSIX shared memory allocations rely on mounting the shared memory filesystem to `/dev/shm`. In multi-tenant environments, hardened systems, or Docker/Kubernetes setups, security policies often block or limit access to the host's `/dev/shm`. 
* Granting container access to `/dev/shm` can leak sensitive data between processes or break container isolation boundaries.
* Without `/dev/shm` permissions, a userspace `pthread_mutex_t` cannot be safely exposed across process boundaries.

### The Sysfs Solution via Kernel Semaphores
**Kernel Semaphores** bypass filesystem-level dependency traps. Instead of exposing a broad memory page shared in userspace, synchronization can be safely bound to a highly restricted sysfs interface—for example, `/sys/devices/platform/sptd_lock/lock_mgr`. 

* **Granular Access Control:** Standard Linux file permissions (`chmod`/`chown`) or fine-grained SELinux policies can be applied directly to the specific sysfs endpoint.
* **Secure Isolation:** Subsystems or containers can safely lock and unlock resources via standard kernel operations without needing dangerous read/write permissions to a shared userspace memory pool.
* **Deterministic Lifetime:** The lock infrastructure stays coupled directly to the hardware/platform driver lifecycle rather than volatile user memory segments.

---

## How to Build and Load

### 1. Prerequisites
Ensure you have kernel headers installed on your system:
```bash
sudo apt-get install linux-headers-$(uname -r)
```

### 2. Makefile Template
Create a file named `Makefile` in the same directory:
```makefile
obj-m += sptd_lock.o

all:
	make -C /lib/modules/$(uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(uname -r)/build M=$(PWD) clean
```

### 3. Build and Insert
Run the following commands to compile and load the module:
```bash
make
sudo insmod sptd_lock.ko
```

---

## Usage Guide

All interactions are driven by piping plain-text commands into the platform resource manager at `/sys/devices/platform/sptd_lock/lock_mgr`.

### 1. Create a Named Lock
Initialize an available slot with a human-readable identifier:
```bash
echo "create database_sync" > /sys/devices/platform/sptd_lock/lock_mgr
```

### 2. Acquire a Lock
Attempt to take ownership. If another process holds it, this execution path **blocks (hangs)** until released:
```bash
echo "lock database_sync" > /sys/devices/platform/sptd_lock/lock_mgr
```

### 3. Release a Lock
Relinquish control so waiting processes can continue:
```bash
echo "unlock database_sync" > /sys/devices/platform/sptd_lock/lock_mgr
```

### 4. Delete a Lock Configuration
Free up the tracking slot entirely:
```bash
echo "delete database_sync" > /sys/devices/platform/sptd_lock/lock_mgr
```

### 5. Inspect Runtime Status
View the status table to monitor owners, execution counts, namespace IDs, and liveness properties:
```bash
cat /sys/devices/platform/sptd_lock/lock_mgr
```
**Example Output Matrix:**
```text
name                           owner  ns         count    status
database_sync                  14022  4026531836 4        held
cache_flush                    0      0          12       free
legacy_worker                  9204   4026531836 1        orphaned
```
