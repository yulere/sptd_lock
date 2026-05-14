// SPDX-License-Identifier: GPL-2.0
/*
 * sptd_lock (sysfs lock resource for userspace lock/unlock between process/thread/docker)
 *
 * Provides:
 *   /sys/devices/platform/sptd_lock/lock_mgr   (create a named lock, lock, unlock, delete)
 *
 * Control commands (write to lock_mgr):
 *   create <name>
 *   delete <name>
 *   lock   <name>
 *   unlock <name>
 *
 * Userspace:
 *   # Create a lock named "resource1"
 *   echo "create resource1" > /sys/devices/platform/sptd_lock/lock_mgr
 * 
 *   # Try to acquire the lock (this will hang until acquired)
 *   echo "lock resource1" > /sys/devices/platform/sptd_lock/lock_mgr
 * 
 *   # In another terminal, check status
 *   cat /sys/devices/platform/sptd_lock/lock_mgr
 * 
 *   # Release the lock
 *   echo "unlock resource1" > /sys/devices/platform/sptd_lock/lock_mgr
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/semaphore.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/string.h>

#define MAX_LOCKS 16
#define MAX_NAME_LEN 64

struct lock_entry {
	bool is_allocated;
	char name[MAX_NAME_LEN];
	pid_t owner_vpid;
	struct pid_namespace *ns;
	struct semaphore sem;
	unsigned long lock_count;  /* NEW: Total times this lock was acquired */
}__aligned(8);


static struct lock_entry locks[MAX_LOCKS];
static DEFINE_MUTEX(global_mgr_mtx);

/* 
 * Safe check: find the PID inside its specific namespace.
 */
static bool is_process_alive(pid_t vpid, struct pid_namespace *ns)
{
	struct task_struct *task;
    struct pid *pid_struct;
	bool alive = false;

	if (vpid <= 0 || !ns)
		return false;

	rcu_read_lock();
	/* 
	 * Use find_pid_ns to get the PID structure, 
	 * then check if any task is still associated with it.
	 */
	pid_struct = find_pid_ns(vpid, ns);
	if (pid_struct) {
		task = pid_task(pid_struct, PIDTYPE_PID);
		/* Check that task exists and hasn't started the exit sequence */
		if (task && !(task->flags & (PF_EXITING | PF_SIGNALED)))
			alive = true;
	}
	rcu_read_unlock();

	return alive;
}


/* 
 * Force-release a lock if the owner is dead.
 * Semaphores allow anyone to call up(), unlike mutexes.
 */
static bool cleanup_dead_lock(int idx)
{
	pid_t vpid;
	struct pid_namespace *ns;
	bool released = false;

	mutex_lock(&global_mgr_mtx);
	vpid = locks[idx].owner_vpid;
	ns = locks[idx].ns;
	mutex_unlock(&global_mgr_mtx);

	if (vpid > 0 && !is_process_alive(vpid, ns)) {
		mutex_lock(&global_mgr_mtx);
		if (locks[idx].owner_vpid == vpid) {
			pr_info("sptd_lock: Force-releasing '%s' from dead PID %d\n",
				locks[idx].name, vpid);
			locks[idx].owner_vpid = 0;
			if (locks[idx].ns) {
				put_pid_ns(locks[idx].ns);
				locks[idx].ns = NULL;
			}
			up(&locks[idx].sem);
			released = true;
		}
		mutex_unlock(&global_mgr_mtx);
	}
	return released;
}

/* --- Command Handlers --- */

static int do_create(const char *name)
{
	int i, free_slot = -1;

	mutex_lock(&global_mgr_mtx);

	for (i = 0; i < MAX_LOCKS; i++) {
        /* 1. If it exists, just return -EEXIST. 
		   The next 'lock' command will handle cleaning up if the owner is dead. */
		if (locks[i].is_allocated && strcmp(locks[i].name, name) == 0) {
			mutex_unlock(&global_mgr_mtx);
			return -EEXIST;
		}
		if (!locks[i].is_allocated && free_slot < 0)
			free_slot = i;
	}

	if (free_slot >= 0) {
		strncpy(locks[free_slot].name, name, MAX_NAME_LEN - 1);
		locks[free_slot].name[MAX_NAME_LEN - 1] = '\0';
		locks[free_slot].is_allocated = true;
		locks[free_slot].owner_vpid = 0;
		locks[free_slot].ns = NULL;
		sema_init(&locks[free_slot].sem, 1);
		mutex_unlock(&global_mgr_mtx);
		return 0;
	}

	mutex_unlock(&global_mgr_mtx);
	return -ENOMEM;
}

static int do_lock(int idx)
{
	pid_t my_vpid = task_tgid_vnr(current);
	struct pid_namespace *my_ns = task_active_pid_ns(current);

	cleanup_dead_lock(idx);

	if (down_interruptible(&locks[idx].sem)) {
        pr_info("sptd_lock: [%d] lock down_interruptible ERROR by PID %d, ns %p\n", idx, my_vpid, my_ns);
		return -ERESTARTSYS;
    }

	mutex_lock(&global_mgr_mtx);
	if (!locks[idx].is_allocated) {
		mutex_unlock(&global_mgr_mtx);
		up(&locks[idx].sem);
        pr_info("sptd_lock: [%d] lock ERROR is_allocated by PID %d, ns %p\n", idx, my_vpid, my_ns);
		return -ENODEV;
	}
	locks[idx].owner_vpid = my_vpid;
	locks[idx].ns = get_pid_ns(my_ns);
	locks[idx].lock_count++; /* Increment on every success */
	mutex_unlock(&global_mgr_mtx);

	return 0;
}


static int do_unlock(int idx)
{
	pid_t my_vpid = task_tgid_vnr(current);
	struct pid_namespace *my_ns = task_active_pid_ns(current);

	mutex_lock(&global_mgr_mtx);
	if (locks[idx].owner_vpid == my_vpid && locks[idx].ns == my_ns) {
		locks[idx].owner_vpid = 0;
		if (locks[idx].ns) {
			put_pid_ns(locks[idx].ns);
			locks[idx].ns = NULL;
		}
		mutex_unlock(&global_mgr_mtx);
		up(&locks[idx].sem);
		return 0;
	}
	mutex_unlock(&global_mgr_mtx);
    pr_info("sptd_lock: [%d] unlocked ERROR by PID %d, ns %p\n", idx, my_vpid, my_ns);
	return -EPERM;
}

static int do_delete(int idx)
{
	mutex_lock(&global_mgr_mtx);
	locks[idx].is_allocated = false;
	locks[idx].owner_vpid = 0;
	if (locks[idx].ns) {
		put_pid_ns(locks[idx].ns);
		locks[idx].ns = NULL;
	}
	up(&locks[idx].sem);
	mutex_unlock(&global_mgr_mtx);
	return 0;
}

/* --- Sysfs Store Dispatcher --- */

static ssize_t lock_mgr_store(struct device *dev, struct device_attribute *attr,
                              const char *buf, size_t count)
{
	char *kbuf, *cmd, *arg;
	int i, idx = -1, ret = -EINVAL;

	if (count > 256) return -EINVAL;

	kbuf = kmalloc(count + 1, GFP_KERNEL);
	if (!kbuf) return -ENOMEM;
	memcpy(kbuf, buf, count);
	kbuf[count] = '\0';

	/* 1. Find start of CMD */
	cmd = kbuf;
	while (*cmd && (*cmd == ' ' || *cmd == '\t' || *cmd == '\n' || *cmd == '\r')) cmd++;

	/* 2. Find end of CMD / start of ARG */
	arg = cmd;
	while (*arg && *arg != ' ' && *arg != '\t' && *arg != '\n' && *arg != '\r') arg++;

	if (*arg) {
		*arg = '\0'; // Terminate CMD
		arg++;
		/* Skip whitespace to find start of ARG */
		while (*arg && (*arg == ' ' || *arg == '\t' || *arg == '\n' || *arg == '\r')) arg++;
	}

	/* 3. TRIM THE END: This removes the trailing \n from echo commands */
	if (*arg) {
		char *end = arg + strlen(arg) - 1;
		while (end >= arg && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
			*end = '\0';
			end--;
		}
	}

	if (!*cmd || !*arg) {
		ret = -EINVAL;
		goto out_free;
	}

	/* ... rest of your logic (create, find index, dispatch) ... */

	if (strcmp(cmd, "create") == 0) {
		ret = do_create(arg);
		goto out_free;
	}

	mutex_lock(&global_mgr_mtx);
	for (i = 0; i < MAX_LOCKS; i++) {
		if (locks[i].is_allocated && strcmp(locks[i].name, arg) == 0) {
			idx = i;
			break;
		}
	}
	mutex_unlock(&global_mgr_mtx);

	if (idx < 0) {
		ret = -ENODEV;
		goto out_free;
	}

	if (strcmp(cmd, "lock") == 0)        ret = do_lock(idx);
	else if (strcmp(cmd, "unlock") == 0) ret = do_unlock(idx);
	else if (strcmp(cmd, "delete") == 0) ret = do_delete(idx);

out_free:
	kfree(kbuf);
	return ret ? ret : count;
}

static ssize_t lock_mgr_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	/* Print aligned table header matching the column widths below */
	len += scnprintf(buf + len, PAGE_SIZE - len, 
			 "%-30s %-6s %-10s %-8s %s\n", 
			 "name", "owner", "ns", "count", "status");

	for (i = 0; i < MAX_LOCKS; i++) {
		pid_t owner; 
		struct pid_namespace *ns; 
		bool allocated, alive = false; 
		unsigned int ns_inum = 0;
		unsigned long count = 0;

		mutex_lock(&global_mgr_mtx);
		allocated = locks[i].is_allocated;
		owner = locks[i].owner_vpid;
		ns = locks[i].ns;
		count = locks[i].lock_count;
		if (allocated && ns) ns_inum = ns->ns.inum;
		mutex_unlock(&global_mgr_mtx);

		if (!allocated) continue;

		if (owner > 0 && ns) alive = is_process_alive(owner, ns);

		mutex_lock(&global_mgr_mtx);
		if (locks[i].is_allocated) {
			/* Kept heavy spacing for precise column alignment without repetitive labels */
			len += scnprintf(buf + len, PAGE_SIZE - len, 
					 "%-30s %-6d %-10u %-8lu %s\n",
					 locks[i].name, owner, ns_inum, count, 
					 (owner == 0) ? "free" : (alive ? "held" : "orphaned"));
		}
		mutex_unlock(&global_mgr_mtx);

		if (len >= PAGE_SIZE - 128) break;
	}
	return len;
}




static DEVICE_ATTR_RW(lock_mgr);
static struct platform_device *sl_pdev;

static int __init sptd_lock_init(void)
{
	int i;

	for (i = 0; i < MAX_LOCKS; i++) {
		locks[i].is_allocated = false;
		locks[i].owner_vpid = 0;
		locks[i].ns = NULL;
		sema_init(&locks[i].sem, 1);
	}

	sl_pdev = platform_device_register_simple("sptd_lock", -1, NULL, 0);
	if (IS_ERR(sl_pdev)) return PTR_ERR(sl_pdev);

	if (device_create_file(&sl_pdev->dev, &dev_attr_lock_mgr)) {
		platform_device_unregister(sl_pdev);
		return -ENODEV;
	}

	pr_info("sptd_lock: sptd_lock loaded\n");
	return 0;
}

static void __exit sptd_lock_exit(void)
{
	int i;

	if (sl_pdev)
		device_remove_file(&sl_pdev->dev, &dev_attr_lock_mgr);

	mutex_lock(&global_mgr_mtx);
	for (i = 0; i < MAX_LOCKS; i++) {
		if (locks[i].is_allocated) {
			locks[i].is_allocated = false;
			locks[i].owner_vpid = 0;
			if (locks[i].ns) {
				put_pid_ns(locks[i].ns);
				locks[i].ns = NULL;
			}
			up(&locks[i].sem);  /* Wake all waiters */
		}
	}
	mutex_unlock(&global_mgr_mtx);

	platform_device_unregister(sl_pdev);
	pr_info("sptd_lock: Unloaded\n");
}

module_init(sptd_lock_init);
module_exit(sptd_lock_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("SPTD Team");
MODULE_DESCRIPTION("Semaphore-based Lock Manager with PID namespace support");
