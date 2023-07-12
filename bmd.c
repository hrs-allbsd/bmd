#include <sys/param.h>
#include <sys/dirent.h>
#include <sys/event.h>
#include <sys/procctl.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "bmd.h"
#include "conf.h"
#include "log.h"
#include "parser.h"
#include "server.h"
#include "vars.h"
#include "vm.h"

/*
  List of VM configurations.
 */
struct vm_conf_head vm_conf_list = LIST_HEAD_INITIALIZER();

/*
  List of virtual machines.
 */
SLIST_HEAD(, vm_entry) vm_list = SLIST_HEAD_INITIALIZER();
SLIST_HEAD(, plugin_entry) plugin_list = SLIST_HEAD_INITIALIZER();

/*
  All Events
*/
struct events events = LIST_HEAD_INITIALIZER(events);

/*
  Global event queue;
 */
static int eventq;

/*
  Last Timer Event ID
 */
static int timer_id = 0;

extern struct vm_methods method_list[];

static int
kevent_set(struct kevent *kev, int n)
{
	int rc;
	while ((rc = kevent(eventq, kev, n, NULL, 0, NULL)) < 0)
		if (errno != EINTR)
			return -1;
	return rc;
}

static int
kevent_get(struct kevent *kev, int n, struct timespec *timeout)
{
	int rc;
	while ((rc = kevent(eventq, NULL, 0 , kev, n, timeout)) < 0)
		if (errno != EINTR)
			return -1;
	return rc;
}

int
plugin_wait_for_process(pid_t pid, int (*cb)(int ident, void *data), void *data)
{
	struct event *ev;

	if ((ev = malloc(sizeof(*ev))) == NULL)
		return -1;

	ev->type = PLUGIN;
	ev->cb = cb;
	ev->data = data;

	EV_SET(&ev->kev, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT,
	       NOTE_EXIT, 0, ev);
	if (kevent_set(&ev->kev, 1) < 0) {
		ERR("failed to wait plugin process (%s)\n", strerror(errno));
		free(ev);
		return -1;
	}
	LIST_INSERT_HEAD(&events, ev, next);
	return 0;
}

int
plugin_set_timer(int second, int (*cb)(int ident, void *data), void *data)
{
	struct event *ev;

	if ((ev = malloc(sizeof(*ev))) == NULL)
		return -1;

	ev->type = PLUGIN;
	ev->cb = cb;
	ev->data = data;

	EV_SET(&ev->kev, ++timer_id, EVFILT_TIMER,
	       EV_ADD | EV_ONESHOT, NOTE_SECONDS, second, ev);
	if (kevent_set(&ev->kev, 1) < 0) {
		ERR("failed to wait plugin process (%s)\n", strerror(errno));
		free(ev);
		return -1;
	}
	LIST_INSERT_HEAD(&events, ev, next);
	return 0;
}

int
on_read_vm_output(int fd, void *data)
{
	struct vm_entry *vm_ent = data;
	struct event *ev, *evn;
	struct kevent *kev;

	if (write_err_log(fd, VM_PTR(vm_ent)) == 0) {
		LIST_FOREACH_SAFE (ev, &events, next, evn) {
			kev = &ev->kev;
			if (ev->data != vm_ent || kev->ident != fd ||
			    kev->filter != EVFILT_READ)
				continue;
			/*
			 * No need to remove fd from kqueue.
			 * It's already closed.
			 */
			LIST_REMOVE(ev, next);
			free(ev);
		}
	}
	return 0;
}

int
wait_for_vm_output(struct vm_entry *vm_ent)
{
	int i = 0, j;
	struct event *ev[2] = {NULL, NULL};
	struct kevent kev[2];

	if (VM_OUTFD(vm_ent) != -1) {
		if ((ev[i] = malloc(sizeof(struct event))) == NULL)
			goto err;
		ev[i]->type = EVENT;
		ev[i]->cb = on_read_vm_output;
		ev[i]->data = vm_ent;

		EV_SET(&ev[i]->kev, VM_OUTFD(vm_ent), EVFILT_READ, EV_ADD, 0, 0,
		       ev[i]);
		memcpy(&kev[i], &ev[i]->kev, sizeof(struct kevent));
		i++;
	}
	if (VM_ERRFD(vm_ent) != -1) {
		if ((ev[i] = malloc(sizeof(struct event))) == NULL)
			goto err;
		ev[i]->type = EVENT;
		ev[i]->cb = on_read_vm_output;
		ev[i]->data = vm_ent;

		EV_SET(&ev[i]->kev, VM_ERRFD(vm_ent), EVFILT_READ, EV_ADD, 0, 0,
		       ev[i]);
		memcpy(&kev[i], &ev[i]->kev, sizeof(struct kevent));
		i++;
	}

	if (kevent_set(kev, i) < 0) {
		ERR("failed to wait vm fds (%s)\n", strerror(errno));
		goto err;
	}

	for (j = 0; j < i; j++)
		LIST_INSERT_HEAD(&events, ev[j], next);

	return 0;
err:
	for (j = 0; j < i; j++)
		free(ev[j]);

	return -1;
}

int
stop_waiting_vm_output(struct vm_entry *vm_ent)
{
	struct event *ev, *evn;
	struct kevent *kev;

	LIST_FOREACH_SAFE (ev, &events, next, evn) {
		kev = &ev->kev;
		if (ev->data != vm_ent ||
		    (kev->ident != VM_OUTFD(vm_ent) &&
		     kev->ident != VM_ERRFD(vm_ent)) ||
		    kev->filter != EVFILT_READ)
			continue;
		kev->flags = EV_DELETE;
		if (kevent_set(kev, 1) < 0) {
			ERR("failed to remove vm output event (%s)\n",
			    strerror(errno));
		}
		LIST_REMOVE(ev, next);
		free(ev);
	}

	return 0;
}

void
free_events()
{
	struct event *ev, *evn;

	LIST_FOREACH_SAFE (ev, &events, next, evn)
		free(ev);
	LIST_INIT(&events);
}

int
on_timer(int ident, void *data)
{
	struct vm_entry *vm_ent = data;

	switch (VM_STATE(vm_ent)) {
	case TERMINATE:
		/* delayed boot */
		start_virtual_machine(vm_ent);
		break;
	case LOAD:
	case STOP:
	case REMOVE:
	case RESTART:
		/* loader timout or stop timeout */
		/* force to poweroff */
		ERR("timeout kill vm %s\n", VM_CONF(vm_ent)->name);
		VM_POWEROFF(vm_ent);
		break;
	case RUN:
		/* ignore timer */
		break;
	}
	return 0;
}

/*
 * Set event timer.
 * Event timer has 2 types. One is for stop timeout, the other is delay boot.
 * If an event is for delay boot, set flag = 1.
 */
int
set_timer(struct vm_entry *vm_ent, int second)
{
	struct event *ev;

	if ((ev = malloc(sizeof(*ev))) == NULL)
		goto err;

	ev->type = EVENT;
	ev->cb = on_timer;
	ev->data = vm_ent;

	EV_SET(&ev->kev, ++timer_id, EVFILT_TIMER,
	       EV_ADD | EV_ONESHOT, NOTE_SECONDS, second, ev);
	if (kevent_set(&ev->kev, 1) < 0)
		goto err;
	LIST_INSERT_HEAD(&events, ev, next);
	return 0;
err:
	free(ev);
	ERR("failed to set timer (%s)\n", strerror(errno));
	return -1;
}

/**
 * Clear all timers for VM.
 */
int
clear_all_timers(struct vm_entry *vm_ent)
{
	struct event *ev, *evn;

	LIST_FOREACH_SAFE (ev, &events, next, evn) {
		if (ev->kev.filter != EVFILT_TIMER || ev->data != vm_ent)
			continue;
		ev->kev.flags = EV_DELETE;
		if (kevent_set(&ev->kev, 1) < 0)
			ERR("failed to remove timer event (%s)\n",
			    strerror(errno));
		LIST_REMOVE(ev, next);
		free(ev);
	}
	return 0;
}

void stop_virtual_machine(struct vm_entry *vm_ent);

static char *
reason_string(int status)
{
	int sz;
	char *mes;

	if (WIFSIGNALED(status))
		sz = asprintf(&mes, " by signal %d%s", WTERMSIG(status),
			      (WCOREDUMP(status) ? " coredump" : ""));
	else if (WIFSTOPPED(status))
		sz = asprintf(&mes, " by signal %d", WSTOPSIG(status));
	else
		sz = ((mes = strdup("")) == NULL ? -1 : 0);

	return (sz < 0) ? NULL : mes;
}

int
on_vm_exit(int ident, void *data)
{
	int status;
	struct vm_entry *vm_ent = data;
	char *rs;

	if (waitpid(VM_PID(vm_ent), &status, 0) < 0)
		ERR("wait error (%s)\n", strerror(errno));
	switch (VM_STATE(vm_ent)) {
	case LOAD:
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			VM_CLOSE(vm_ent, INFD);
			stop_waiting_vm_output(vm_ent);
			start_virtual_machine(vm_ent);
		} else {
			ERR("failed loading vm %s (status:%d)\n",
			    VM_CONF(vm_ent)->name, WEXITSTATUS(status));
			stop_virtual_machine(vm_ent);
		}
		break;
	case RESTART:
		stop_virtual_machine(vm_ent);
		VM_STATE(vm_ent) = TERMINATE;
		set_timer(vm_ent, MAX(VM_CONF(vm_ent)->boot_delay, 3));
		break;
	case RUN:
		if (VM_CONF(vm_ent)->install == false &&
		    WIFEXITED(status) &&
		    (VM_CONF(vm_ent)->boot == ALWAYS ||
		     (VM_CONF(vm_ent)->backend == BHYVE &&
		      WEXITSTATUS(status) == 0))) {
			start_virtual_machine(vm_ent);
			break;
		}
		/* FALLTHROUGH */
	case STOP:
		rs = reason_string(status);
		INFO("vm %s is stopped%s\n", VM_CONF(vm_ent)->name,
		     (rs == NULL ? "" : rs));
		free(rs);
		stop_virtual_machine(vm_ent);
		VM_CONF(vm_ent)->install = false;
		break;
	case REMOVE:
		INFO("vm %s is stopped\n", VM_CONF(vm_ent)->name);
		stop_virtual_machine(vm_ent);
		SLIST_REMOVE(&vm_list, vm_ent, vm_entry, next);
		free_vm_entry(vm_ent);
		break;
	case TERMINATE:
		break;
	}

	return 0;
}

int
wait_for_vm(struct vm_entry *vm_ent)
{
	struct event *ev;
	if ((ev = malloc(sizeof(*ev))) == NULL)
		return -1;

	ev->type = EVENT;
	ev->cb = on_vm_exit;
	ev->data = vm_ent;

	EV_SET(&ev->kev, VM_PID(vm_ent), EVFILT_PROC, EV_ADD | EV_ONESHOT,
	       NOTE_EXIT, 0, ev);

	if (kevent_set(&ev->kev, 1) < 0) {
		ERR("failed to wait process (%s)\n", strerror(errno));
		free(ev);
		return -1;
	}

	LIST_INSERT_HEAD(&events, ev, next);
	return 0;
}

int
set_sock_buf_wait_flags(struct sock_buf *sb, short recv_f, short send_f)
{
	int i = 0;
	struct event *e, *ev[2] = {NULL, NULL};
	struct kevent kev[2];

	LIST_FOREACH (e, &events, next) {
		if (e->data == sb) {
			switch (e->kev.filter) {
			case EVFILT_READ:
				ev[0] = e;
				memcpy(&kev[i], &e->kev, sizeof(struct kevent));
				kev[i].flags = recv_f;
				break;
				i++;
			case EVFILT_WRITE:
				ev[1] = e;
				memcpy(&kev[i], &e->kev, sizeof(struct kevent));
				kev[i].flags = send_f;
				i++;
				break;
			default:
				break;
			}
			if (i >= 2)
				break;
		}
	}

	if (i == 0)
		return 0;

	if (kevent_set(kev, i) < 0) {
		ERR("failed to change cmd socket event (%s)\n",
		    strerror(errno));
		return -1;
	}

	if (ev[0])
		ev[0]->kev.flags =  recv_f;

	if (ev[1])
		ev[1]->kev.flags =  send_f;

	return 0;
}

int
stop_waiting_sock_buf(struct sock_buf *sb)
{
	struct event *ev, *evn;

	LIST_FOREACH_SAFE (ev, &events, next, evn)
		if (ev->data == sb) {
			ev->kev.flags = EV_DELETE;
			if (kevent_set(&ev->kev, 1) < 0) {
				ERR("failed to remove socket events(%s)\n",
				    strerror(errno));
				return -1;
			}
			LIST_REMOVE(ev, next);
			free(ev);
		}

	return 0;
}

int
on_recv_sock_buf(int ident, void *data)
{
	struct sock_buf *sb = data;

	switch (recv_sock_buf(sb)) {
	case 2:
		if (recv_command(sb) == 0) {
			clear_sock_buf(sb);
			set_sock_buf_wait_flags(sb, EV_DISABLE, EV_ENABLE);
			break;
		}
		/* FALLTHROUGH */
	case 1:
		break;
	default:
		stop_waiting_sock_buf(sb);
		destroy_sock_buf(sb);
	}
	return 0;
}

int
on_send_sock_buf(int ident, void *data)
{
	struct sock_buf *sb = data;

	switch (send_sock_buf(sb)) {
	case 2:
		clear_send_sock_buf(sb);
		set_sock_buf_wait_flags(sb,  EV_ENABLE, EV_DISABLE);
		/* FALLTHROUGH */
	case 1:
		break;
	default:
		stop_waiting_sock_buf(sb);
		destroy_sock_buf(sb);
		break;
	}
	return 0;
}

static int
wait_for_sock_buf(struct sock_buf *sb)
{
	struct event *ev[2];
	struct kevent kev[2];

	ev[0] = malloc(sizeof(struct event));
	ev[1] = malloc(sizeof(struct event));

	if (ev[0] == NULL || ev[1] == NULL) {
		free(ev[0]);
		free(ev[1]);
		return -1;
	}

	ev[0]->type = EVENT;
	ev[0]->cb = on_recv_sock_buf;
	ev[0]->data = sb;

	EV_SET(&ev[0]->kev, sb->fd, EVFILT_READ, EV_ADD, 0, 0, ev[0]);
	kev[0] = ev[0]->kev;

	ev[1]->type = EVENT;
	ev[1]->cb = on_send_sock_buf;
	ev[1]->data = sb;

	EV_SET(&ev[1]->kev, sb->fd, EVFILT_WRITE, EV_ADD, EV_DISABLE, 0, ev[1]);
	kev[1] = ev[1]->kev;

	if (kevent_set(kev, 2) < 0) {
		ERR("failed to wait socket buffer (%s)\n", strerror(errno));
		free(ev[0]);
		free(ev[1]);
		return -1;
	}

	LIST_INSERT_HEAD(&events, ev[1], next);
	LIST_INSERT_HEAD(&events, ev[0], next);
	return 0;
}

int
on_accept_cmd_sock(int ident, void *data)
{
	struct sock_buf *sb;
	int n, sock = gl_conf->cmd_sock;

	if ((n = accept_command_socket(sock)) < 0)
		return -1;

	if ((sb = create_sock_buf(n)) == NULL) {
		ERR("%s\n","failed to allocate socket buffer");
		close(n);
		return -1;
	}

	if (wait_for_sock_buf(sb) < 0) {
		close(n);
		return -1;
	}

	return 0;
}

static int
wait_for_cmd_sock(int sock)
{
	struct event *ev;

	if ((ev = malloc(sizeof(*ev))) == NULL)
		return -1;

	ev->type = EVENT;
	ev->cb = on_accept_cmd_sock;
	ev->data = NULL;

	EV_SET(&ev->kev, sock, EVFILT_READ, EV_ADD, 0, 0, ev);

	if (kevent_set(&ev->kev, 1) < 0) {
		ERR("failed to wait socket recv (%s)\n", strerror(errno));
		free(ev);
		return -1;
	}

	LIST_INSERT_HEAD(&events, ev, next);
	return 0;
}

int
load_plugins(const char *plugin_dir)
{
	DIR *d;
	void *hdl;
	int fd;
	struct dirent *ent;
	struct plugin_desc *desc;
	struct plugin_entry *pl_ent;
	static int loaded = 0;

	if (loaded != 0)
		return 0;

	if ((d = opendir(plugin_dir)) == NULL) {
		ERR("can not open %s\n", gl_conf->plugin_dir);
		return -1;
	}

	while ((ent = readdir(d)) != NULL) {
		if (ent->d_namlen < 4 || ent->d_name[0] == '.' ||
		    strcmp(&ent->d_name[ent->d_namlen - 3], ".so") != 0)
			continue;
		while ((fd = openat(dirfd(d), ent->d_name, O_RDONLY)) < 0)
			if (errno != EINTR)
				break;
		if (fd < 0)
			continue;

		if ((hdl = fdlopen(fd, RTLD_NOW)) == NULL) {
			ERR("failed to open plugin %s\n", ent->d_name);
			goto next;
		}

		if ((desc = dlsym(hdl, "plugin_desc")) == NULL ||
		    desc->version != PLUGIN_VERSION ||
		    (pl_ent = calloc(1, sizeof(*pl_ent))) == NULL) {
			dlclose(hdl);
			goto next;
		}

		if (desc->initialize && (*(desc->initialize))(gl_conf) < 0) {
			free(pl_ent);
			dlclose(hdl);
			goto next;
		}
		pl_ent->desc = *desc;
		pl_ent->handle = hdl;
		SLIST_INSERT_HEAD(&plugin_list, pl_ent, next);
		INFO("load plugin %s %s\n", desc->name, ent->d_name);
		loaded++;
	next:
		close(fd);
	}

	closedir(d);

	return 0;
}

int
remove_plugins()
{
	struct plugin_entry *pl_ent, *pln;

	SLIST_FOREACH_SAFE (pl_ent, &plugin_list, next, pln) {
		if (pl_ent->desc.finalize)
			(*pl_ent->desc.finalize)(gl_conf);
		dlclose(pl_ent->handle);
		free(pl_ent);
	}
	SLIST_INIT(&plugin_list);

	return 0;
}

void
call_plugins(struct vm_entry *vm_ent)
{
	struct plugin_data *pd;

	SLIST_FOREACH (pd, &VM_PLUGIN_DATA(vm_ent), next)
		if (pd->ent->desc.on_status_change)
			(pd->ent->desc.on_status_change)(VM_PTR(vm_ent),
							 pd->pl_conf);
}

int
call_plugin_parser(struct plugin_data_head *head,
		   const char *key, const char *val)
{
	int rc;
	struct plugin_data *pd;

	SLIST_FOREACH (pd, head, next)
		if (pd->ent->desc.parse_config &&
		    (rc = (pd->ent->desc.parse_config)(pd->pl_conf, key, val)) <= 0)
			return rc;
	return 1;
}

void
free_vm_entry(struct vm_entry *vm_ent)
{
	struct net_conf *nc, *nnc;
	struct event *ev, *evn;

	STAILQ_FOREACH_SAFE (nc, VM_TAPS(vm_ent), next, nnc)
		free_net_conf(nc);
	LIST_FOREACH_SAFE (ev, &events, next, evn) {
		if (ev->data != vm_ent)
			continue;
		/*
		  Basically no events are left on this timing.
		  Delete & free them for safty.
		*/
		ev->kev.flags = EV_DELETE;
		if (kevent_set(&ev->kev, 1) < 0)
			ERR("failed to remove vm event (%s)\n",
			    strerror(errno));
		LIST_REMOVE(ev, next);
		free(ev);
	}
	free(VM_MAPFILE(vm_ent));
	free(VM_VARSFILE(vm_ent));
	free(VM_ASCOMPORT(vm_ent));
	free_vm_conf_entry(VM_CONF_ENT(vm_ent));
	free(vm_ent);
}

void
free_vm_list()
{
	struct vm_entry *vm_ent, *vmn;

	SLIST_FOREACH_SAFE (vm_ent, &vm_list, next, vmn)
		free_vm_entry(vm_ent);
	SLIST_INIT(&vm_list);
}

void
free_plugin_data(struct plugin_data_head *head)
{
	struct plugin_data *pld, *pln;

	SLIST_FOREACH_SAFE (pld, head, next, pln) {
		nvlist_destroy(pld->pl_conf);
		free(pld);
	}
	SLIST_INIT(head);
}

void
free_vm_conf_entry(struct vm_conf_entry *conf_ent)
{
	free_plugin_data(&conf_ent->pl_data);
	free_vm_conf(&conf_ent->conf);
}

int
create_plugin_data(struct plugin_data_head *head)
{
	struct plugin_entry *pl_ent;
	struct plugin_data *pld;

	SLIST_INIT(head);
	SLIST_FOREACH (pl_ent, &plugin_list, next) {
		if ((pld = calloc(1, sizeof(*pld))) == NULL)
			goto err;
		pld->ent = pl_ent;
		if ((pld->pl_conf = nvlist_create(0)) == NULL) {
			free(pld);
			goto err;
		}
		SLIST_INSERT_HEAD(head, pld, next);
	}

	return 0;

err:
	free_plugin_data(head);
	return -1;
}

struct vm_entry *
create_vm_entry(struct vm_conf_entry *conf_ent)
{
	struct vm_entry *vm_ent;

	if ((vm_ent = calloc(1, sizeof(struct vm_entry))) == NULL)
		return NULL;
	VM_TYPE(vm_ent) = VMENTRY;
	VM_METHOD(vm_ent) = &method_list[conf_ent->conf.backend];
	VM_CONF(vm_ent) = &conf_ent->conf;
	VM_STATE(vm_ent) = TERMINATE;
	VM_PID(vm_ent) = -1;
	VM_INFD(vm_ent) = -1;
	VM_OUTFD(vm_ent) = -1;
	VM_ERRFD(vm_ent) = -1;
	VM_LOGFD(vm_ent) = -1;
	STAILQ_INIT(VM_TAPS(vm_ent));
	SLIST_INSERT_HEAD(&vm_list, vm_ent, next);

	return vm_ent;
}

static int
nmdm_selector(const struct dirent *e)
{
	return (strncmp(e->d_name, "nmdm", 4) == 0 &&
		e->d_name[e->d_namlen - 1] == 'B');
}

static int
get_nmdm_number(const char *p)
{
	int v = 0;

	if (p == NULL)
		return -1;

	for (; *p != '\0'; p++)
		if (isnumber(*p))
			v = v * 10 + *p - '0';
	return v;
}

/**
 * Assign new 'nmdm' which has a bigger number in all VM configurations and
 * "/dev/" directory.
 */
int
assign_comport(struct vm_entry *vm_ent)
{
	int fd, i, n, v, max = -1;
	struct dirent **names;
	char *new_com;
	struct vm_entry *e;
	struct vm_conf *conf = VM_CONF(vm_ent);

	if (conf->comport == NULL)
		return 0;

	/* Already assigned */
	if (VM_ASCOMPORT(vm_ent))
		return 0;

	/* If no need to assign comport, copy from `struct vm_conf.comport`. */
	if (strcasecmp(conf->comport, "auto")) {
		if ((VM_ASCOMPORT(vm_ent) = strdup(conf->comport)) == NULL)
			return -1;
		return 0;
	}

	/* Get maximum nmdm number of all VMs. */
	SLIST_FOREACH (e, &vm_list, next) {
		v = get_nmdm_number(VM_CONF(e)->comport);
		if (v > max)
			max = v;
		v = get_nmdm_number(VM_ASCOMPORT(e));
		if (v > max)
			max = v;
	}

	/* Get maximum nmdm number in "/dev" directory. */
	if ((n = scandir("/dev", &names, nmdm_selector, NULL)) < 0)
		return -1;

	for (i = 0; i < n; i++) {
		v = get_nmdm_number(names[i]->d_name);
		if (v > max)
			max = v;
		free(names[i]);
	}
	free(names);

	if (max < gl_conf->nmdm_offset - 1)
		max = gl_conf->nmdm_offset - 1;

	for (i = 1; i < 6; i++) {
		if (asprintf(&new_com, "/dev/nmdm%dB", max + i) < 0)
			return -1;

		while ((fd = open(new_com, O_RDWR|O_NONBLOCK)) < 0)
			if (errno != EINTR)
				break;
		if (fd >= 0)
			break;
		free(new_com);
	}
	if (fd < 0)
		return -1;
	close(fd);
	VM_ASCOMPORT(vm_ent) = new_com;

	return 0;
}

int
start_virtual_machine(struct vm_entry *vm_ent)
{
	struct vm_conf *conf = VM_CONF(vm_ent);
	char *name = conf->name;

	VM_METHOD(vm_ent) = &method_list[conf->backend];

	if (assign_comport(vm_ent) < 0) {
		ERR("failed to assign comport for vm %s\n", name);
		return -1;
	}

	if (VM_START(vm_ent) < 0) {
		ERR("failed to start vm %s\n", name);
		VM_CLEANUP(vm_ent);
		return -1;
	}

	if (wait_for_vm(vm_ent) < 0 || wait_for_vm_output(vm_ent) < 0) {
		ERR("failed to set kevent for vm %s\n", name);
		/*
		 * Force to kill bhyve.
		 * If this error happens, we can't manage bhyve process at all.
		 */
		VM_POWEROFF(vm_ent);
		waitpid(VM_PID(vm_ent), NULL, 0);
		VM_CLEANUP(vm_ent);
		return -1;
	}

	if (VM_STATE(vm_ent) == RUN)
		INFO("start vm %s\n", name);

	call_plugins(vm_ent);
	if (VM_STATE(vm_ent) == LOAD && conf->loader_timeout >= 0 &&
	    set_timer(vm_ent, conf->loader_timeout) < 0) {
		ERR("failed to set timer for vm %s\n", name);
		return -1;
	}

	if (VM_LOGFD(vm_ent) == -1)
		while ((VM_LOGFD(vm_ent) = open(conf->err_logfile,
						O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC,
						0644)) < 0)
			if (errno != EINTR)
				break;

	return 0;
}

int
start_virtual_machines()
{
	struct vm_conf *conf;
	struct vm_conf_entry *conf_ent;
	struct vm_entry *vm_ent;
	struct kevent sigev[3];

	EV_SET(&sigev[0], SIGTERM, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
	EV_SET(&sigev[1], SIGINT, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
	EV_SET(&sigev[2], SIGHUP, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);

	if (kevent_set(sigev, 3) < 0)
		return -1;

	LIST_FOREACH (conf_ent, &vm_conf_list, next) {
		vm_ent = create_vm_entry(conf_ent);
		if (vm_ent == NULL)
			return -1;
		conf = &conf_ent->conf;
		if (conf->boot == NO)
			continue;
		if (conf->boot_delay > 0) {
			if (set_timer(vm_ent, conf->boot_delay) < 0)
				ERR("failed to set boot delay timer for vm %s\n",
				    conf->name);
			continue;
		}
		start_virtual_machine(vm_ent);
	}

	return 0;
}

void
stop_virtual_machine(struct vm_entry *vm_ent)
{
	stop_waiting_vm_output(vm_ent);
	clear_all_timers(vm_ent);
	VM_CLEANUP(vm_ent);
	call_plugins(vm_ent);
}

struct vm_entry *
lookup_vm_by_name(const char *name)
{
	struct vm_entry *vm_ent;

	SLIST_FOREACH (vm_ent, &vm_list, next)
		if (strcmp(VM_CONF(vm_ent)->name, name) == 0)
			return vm_ent;
	return NULL;
}

int
reload_virtual_machines()
{
	struct vm_conf *conf;
	struct vm_conf_entry *conf_ent, *cen;
	struct vm_entry *vm_ent, *vmn;
	struct vm_conf_head new_list = LIST_HEAD_INITIALIZER();

	if (load_config_file(&new_list, false) < 0)
		return -1;

	/* make sure new_conf is NULL */
	SLIST_FOREACH (vm_ent, &vm_list, next)
		VM_NEWCONF(vm_ent) = NULL;

	LIST_FOREACH (conf_ent, &new_list, next) {
		conf = &conf_ent->conf;
		vm_ent = lookup_vm_by_name(conf->name);
		if (vm_ent == NULL) {
			vm_ent = create_vm_entry(conf_ent);
			if (vm_ent == NULL)
				return -1;
			VM_NEWCONF(vm_ent) = conf;
			if (conf->boot == NO)
				continue;
			if (conf->boot_delay > 0) {
				if (set_timer(vm_ent, conf->boot_delay) < 0)
					ERR("failed to set timer for %s\n",
					    conf->name);
				continue;
			}
			start_virtual_machine(vm_ent);
			continue;
		}
		if (VM_LOGFD(vm_ent) != -1 &&
		    VM_CONF(vm_ent)->err_logfile != NULL) {
			VM_CLOSE(vm_ent, LOGFD);
			while ((VM_LOGFD(vm_ent) =
				open(VM_CONF(vm_ent)->err_logfile,
				     O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC,
				     0644)) < 0)
				if (errno != EINTR)
					break;
		}
		VM_NEWCONF(vm_ent) = conf;
		if (conf->boot != NO && conf->reboot_on_change &&
		    compare_vm_conf(conf, VM_CONF(vm_ent)) != 0) {
			switch (VM_STATE(vm_ent)) {
			case TERMINATE:
				set_timer(vm_ent, MAX(conf->boot_delay, 1));
				break;
			case LOAD:
			case RUN:
				INFO("reboot vm %s\n", conf->name);
				VM_ACPI_POWEROFF(vm_ent);
				set_timer(vm_ent, conf->stop_timeout);
				VM_STATE(vm_ent) = RESTART;
				break;
			case STOP:
				VM_STATE(vm_ent) = RESTART;
			default:
				break;
			}
			continue;
		}
		if (VM_NEWCONF(vm_ent)->boot == VM_CONF(vm_ent)->boot)
			continue;
		switch (conf->boot) {
		case NO:
			if (VM_STATE(vm_ent) == LOAD ||
			    VM_STATE(vm_ent) == RUN) {
				INFO("acpi power off vm %s\n", conf->name);
				VM_ACPI_POWEROFF(vm_ent);
				set_timer(vm_ent, conf->stop_timeout);
				VM_STATE(vm_ent) = STOP;
			} else if (VM_STATE(vm_ent) == RESTART)
				VM_STATE(vm_ent) = STOP;
			break;
		case ALWAYS:
		case YES:
			if (VM_STATE(vm_ent) == TERMINATE) {
				VM_CONF(vm_ent) = conf;
				start_virtual_machine(vm_ent);
			} else if (VM_STATE(vm_ent) == STOP)
				VM_STATE(vm_ent) = RESTART;
			break;
		case ONESHOT:
			// do nothing
			break;
		}
	}

	SLIST_FOREACH_SAFE (vm_ent, &vm_list, next, vmn)
		if (VM_NEWCONF(vm_ent) == NULL) {
			switch (VM_STATE(vm_ent)) {
			case LOAD:
			case RUN:
				conf = VM_CONF(vm_ent);
				INFO("acpi power off vm %s\n", conf->name);
				VM_ACPI_POWEROFF(vm_ent);
				set_timer(vm_ent, conf->stop_timeout);
				/* FALLTHROUGH */
			case STOP:
			case REMOVE:
			case RESTART:
				VM_STATE(vm_ent) = REMOVE;
				/* remove vm_conf_entry from the list
				   to keep it until actually freed. */
				LIST_REMOVE(VM_CONF_ENT(vm_ent), next);
				break;
			default:
				SLIST_REMOVE(&vm_list, vm_ent, vm_entry,
					     next);
				LIST_REMOVE(VM_CONF_ENT(vm_ent), next);
				free_vm_entry(vm_ent);
			}

		} else {
			VM_CONF(vm_ent) = VM_NEWCONF(vm_ent);
			VM_NEWCONF(vm_ent) = NULL;
		}

	LIST_FOREACH_SAFE (conf_ent, &vm_conf_list, next, cen)
		free_vm_conf_entry(conf_ent);
	LIST_INIT(&vm_conf_list);

	LIST_CONCAT(&vm_conf_list, &new_list, vm_conf_entry, next);

	return 0;
}

int
event_loop()
{
	struct kevent ev;
	struct event *event;
	int n;
	struct timespec *to, timeout;

	if (wait_for_cmd_sock(gl_conf->cmd_sock) < 0)
		return -1;

wait:
	to = calc_timeout(COMMAND_TIMEOUT_SEC, &timeout);
	if ((n = kevent_get(&ev, 1, to)) < 0) {
		ERR("kevent failure (%s)\n", strerror(errno));
		return -1;
	}
	if (n == 0) {
		close_timeout_sock_buf(COMMAND_TIMEOUT_SEC);
		goto wait;
	}

	event = ev.udata;
	if (event != NULL && event->type >= EVENT) {
		if (event->cb && (*event->cb)(ev.ident, event->data) < 0)
			ERR("%s\n", "callback failed");
		if (event->kev.flags & EV_ONESHOT) {
			LIST_REMOVE(event, next);
			free(event);
		}
		goto wait;
	}

	if (ev.filter == EVFILT_SIGNAL) {
		switch (ev.ident) {
		case SIGTERM:
		case SIGINT:
			INFO("%s\n", "stopping daemon");
			goto end;
		case SIGHUP:
			INFO("%s\n", "reload config files");
			reload_virtual_machines();
			goto wait;
		}
	} else {
		ERR("recieved unexpcted event! (%d)", ev.filter);
		return -1;
	}

	goto wait;
end:
	return 0;
}

int
stop_virtual_machines()
{
	struct kevent ev;
	struct event *event;
	struct vm_entry *vm_ent;
	int count = 0;

	SLIST_FOREACH (vm_ent, &vm_list, next) {
		if (VM_STATE(vm_ent) == LOAD || VM_STATE(vm_ent) == RUN) {
			count++;
			VM_ACPI_POWEROFF(vm_ent);
			set_timer(vm_ent, VM_CONF(vm_ent)->stop_timeout);
		}
	}

	while (count > 0) {
		if (kevent_get(&ev, 1, NULL) < 0)
			return -1;
		event = ev.udata;
		if (event != NULL && event->type >= EVENT) {
			if (event->type == EVENT &&
			    event->kev.filter == EVFILT_PROC)
				count--;
			if (event->cb && (*event->cb)(ev.ident, event->data) < 0)
				ERR("%s\n", "callback failed");
			if (event->kev.flags & EV_ONESHOT) {
				LIST_REMOVE(event, next);
				free(event);
			}
			continue;
		}
	}
#if __FreeBSD_version < 1400059
	// waiting for vm memory is actually freed in the kernel.
	sleep(3);
#endif

	return 0;
}

int
parse_opt(int argc, char *argv[])
{
	int ch;

	while ((ch = getopt(argc, argv, "Fc:f:p:m:")) != -1) {
		switch (ch) {
		case 'F':
			gl_conf->foreground = 1;
			break;
		case 'c':
			free(gl_conf->config_file);
			gl_conf->config_file = strdup(optarg);
			break;
		case 'f':
			free(gl_conf->pid_path);
			gl_conf->pid_path = strdup(optarg);
			break;
		case 'p':
			free(gl_conf->plugin_dir);
			gl_conf->plugin_dir = strdup(optarg);
			break;
		case 'm':
			free(gl_conf->unix_domain_socket_mode);
			gl_conf->unix_domain_socket_mode = strdup(optarg);
			break;
		default:
			fprintf(stderr,
			    "usage: %s [-F] [-f pid file] "
			    "[-p plugin directory] \n"
			    "\t[-m unix domain socket permission] \n"
			    "\t[-c config file]\n",
			    argv[0]);
			return -1;
		}
	}

	if (gl_conf->foreground == 0)
		daemon(0, 0);

	return 0;
}

int
strendswith(const char *t, const char *s)
{
	const char *p = &t[strlen(t)];
	const char *q = &s[strlen(s)];

	while (p > t && q > s)
		if (*--p != *--q)
			return (*p) - (*q);

	return (*p) - (*q);
}

int control(int argc, char *argv[]);
struct vm_conf_entry *lookup_vm_conf(const char *name);
int read_stdin(struct vm *vm);

int
main(int argc, char *argv[])
{
	FILE *fp;
	sigset_t nmask, omask;

	if (init_gl_conf() < 0) {
		fprintf(stderr, "failed to allocate memory "
			"for global configuration\n");
		return 1;
	}

	if (init_global_vars() < 0) {
		fprintf(stderr,	"failed to allocate memory "
			"for global variables\n");
		free_gl_conf();
		return 1;
	}

	if (strendswith(argv[0], "ctl") == 0)
		return control(argc, argv);

	if (parse_opt(argc, argv) < 0)
		return 1;

	if (gl_conf->foreground)
		LOG_OPEN_PERROR();
	else
		LOG_OPEN();


	sigemptyset(&nmask);
	sigaddset(&nmask, SIGTERM);
	if (gl_conf->foreground)
		sigaddset(&nmask, SIGINT);
	sigaddset(&nmask, SIGHUP);
	sigaddset(&nmask, SIGPIPE);
	sigprocmask(SIG_BLOCK, &nmask, &omask);

	if (procctl(P_PID, getpid(), PROC_SPROTECT, &(int[]) { PPROT_SET }[0]) <
	    0)
		WARN("%s\n", "can not protect from OOM killer");

	if (load_config_file(&vm_conf_list, true) < 0)
		return 1;

	if ((eventq = kqueue()) < 0) {
		ERR("%s\n", "can not open kqueue");
		return 1;
	}

	if ((gl_conf->cmd_sock = create_command_server(gl_conf)) < 0) {
		ERR("can not bind %s\n", gl_conf->cmd_sock_path);
		return 1;
	}

	if (gl_conf->foreground == 0 &&
	    (fp = fopen(gl_conf->pid_path, "w")) != NULL) {
		fprintf(fp, "%d\n", getpid());
		fclose(fp);
	}

	INFO("%s\n", "start daemon");

	if (start_virtual_machines() < 0) {
		ERR("%s\n", "failed to start virtual machines");
		return 1;
	}

	event_loop();

	unlink(gl_conf->cmd_sock_path);
	close(gl_conf->cmd_sock);

	stop_virtual_machines();
	free_vm_list();
	close(eventq);
	free_events();
	remove_plugins();
	free_id_list();
	free_global_vars();
	free_gl_conf();
	INFO("%s\n", "quit daemon");
	LOG_CLOSE();
	return 0;
}

int
direct_run(const char *name, bool install, bool single)
{
	int i, status;
	struct vm_conf *conf;
	struct vm_conf_entry *conf_ent;
	struct vm_entry *vm_ent;
	struct kevent ev, ev2[3];

	LOG_OPEN_PERROR();

	if ((eventq = kqueue()) < 0) {
		ERR("%s\n", "can not open kqueue");
		return 1;
	}

	conf_ent = lookup_vm_conf(name);
	if (conf_ent == NULL) {
		ERR("no such VM %s\n", name);
		return 1;
	}

	conf = &conf_ent->conf;
	free(conf->comport);
	conf->comport = strdup("stdio");
	conf->install = install;
	set_single_user(conf, single);

	vm_ent = create_vm_entry(conf_ent);
	if (vm_ent == NULL) {
		free_vm_conf_entry(conf_ent);
		return 1;
	}

	if (assign_comport(vm_ent) < 0) {
		ERR("failed to assign comport for vm %s\n", name);
		goto err;
	}

	if (VM_START(vm_ent) < 0)
		goto err;
	i = 0;
	EV_SET(&ev2[i++], VM_PID(vm_ent), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT,
	       0, vm_ent);
	if (VM_STATE(vm_ent) == LOAD && conf->loader_timeout >= 0)
		EV_SET(&ev2[i++], 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
		       NOTE_SECONDS, VM_CONF(vm_ent)->loader_timeout, vm_ent);
	if (VM_INFD(vm_ent) != -1)
		EV_SET(&ev2[i++], 0, EVFILT_READ, EV_ADD, 0, 0, vm_ent);
	if (kevent_set(ev2, i) < 0) {
		ERR("failed to wait process (%s)\n", strerror(errno));
		VM_POWEROFF(vm_ent);
		goto err;
	}
	call_plugins(vm_ent);

wait:
	if (kevent_get(&ev, 1, NULL) < 0) {
		ERR("kevent failure (%s)\n", strerror(errno));
		VM_POWEROFF(vm_ent);
		goto err;
	}

	switch (ev.filter) {
	case EVFILT_READ:
		read_stdin(VM_PTR(vm_ent));
		goto wait;
	case EVFILT_PROC:
		if (waitpid(ev.ident, &status, 0) < 0)
			goto err;
		if (ev.ident != VM_PID(vm_ent))
			goto wait;
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
			break;
		goto err;
	case EVFILT_TIMER:
	default:
		VM_POWEROFF(vm_ent);
		goto err;
	}

	if (VM_STATE(vm_ent) == LOAD) {
		if (VM_START(vm_ent) < 0)
			goto err;
		call_plugins(vm_ent);
		if (waitpid(VM_PID(vm_ent), &status, 0) < 0)
			goto err;
	}

	VM_CLEANUP(vm_ent);
	call_plugins(vm_ent);
	free_vm_entry(vm_ent);
	remove_plugins();
	free_id_list();
	return 0;
err:
	VM_CLEANUP(vm_ent);
	call_plugins(vm_ent);
	free_vm_entry(vm_ent);
	remove_plugins();
	free_id_list();
	return 1;
}
