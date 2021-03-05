#ifndef _VARS_H_
#define _VARS_H_

#include <sys/types.h>
#include <sys/queue.h>

struct disk_conf {
	STAILQ_ENTRY(disk_conf) next;
	char *type;
	char *path;
};

struct iso_conf {
	STAILQ_ENTRY(iso_conf) next;
	char *type;
	char *path;
};

struct net_conf {
	STAILQ_ENTRY(net_conf) next;
	char *type;
	char *bridge;
	char *tap;
};

enum BOOT {
	NO,
	YES,
	DELAYED,
	ONESHOT,
	INSTALL
};

struct vm_conf {
	SLIST_ENTRY(vm_conf) next;
	pid_t pid;
	unsigned int nmdm;
	int boot_delay;
	char *ncpu;
	char *memory;
	char *name;
	char *console;
	enum BOOT boot;
	char *loadcmd;
	int ndisks;
	int nisoes;
	int nnets;
	STAILQ_HEAD(, disk_conf) disks;
	STAILQ_HEAD(, iso_conf) isoes;
	STAILQ_HEAD(, net_conf) nets;
};

#endif
