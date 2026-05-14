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
