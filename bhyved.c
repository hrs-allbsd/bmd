#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "vars.h"
#include "tap.h"
#include "conf.h"
#include "parser.h"

SLIST_HEAD(, vm_conf) vm_conf_list = SLIST_HEAD_INITIALIZER();
SLIST_HEAD(, vm) vm_list = SLIST_HEAD_INITIALIZER();

char *config_directory = "./conf.d";
int kq;

pid_t
bhyve_load(struct vm_conf *conf)
{
	pid_t pid;
	char *args[9];

	args[0] = "/usr/sbin/bhyveload";
	args[1] = "-c";
	args[2] = conf->comport;
	args[3] = "-m";
	args[4] = conf->memory;
	args[5] = "-d";
	args[6] = STAILQ_FIRST(&conf->disks)->path;
	args[7] = conf->name;
	args[8] = NULL;

	pid = fork();
	if (pid > 0) {
	} else if (pid == 0) {
		execv(args[0],args);
		fprintf(stderr, "can not exec %s\n", args[0]);
		exit(1);
	} else {
		fprintf(stderr, "can not fork (%s)\n", strerror(errno));
		return -1;
	}

	return pid;
}

int
remove_taps(struct vm_conf *conf)
{
	int s;
	struct net_conf *nc;

	s = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (s < 0)
		return -1;

	STAILQ_FOREACH(nc, &conf->nets, next)
		if (nc->tap != NULL) {
			destroy_tap(s, nc->tap);
			free(nc->tap);
			nc->tap = NULL;
		}

	close(s);
	return 0;
}

int
activate_taps(struct vm_conf *conf)
{
	int s;
	struct net_conf *nc;

	s = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (s < 0)
		return -1;
	STAILQ_FOREACH(nc, &conf->nets, next)
		activate_tap(s, nc->tap);
	close(s);
	return 0;
}

int
assign_taps(struct vm_conf *conf)
{
	int s;
	struct net_conf *nc;

	s = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (s < 0)
		return -1;

	STAILQ_FOREACH(nc, &conf->nets, next)
		if (create_tap(s, &nc->tap) < 0 ||
		    add_to_bridge(s, nc->bridge, nc->tap) < 0) {
			fprintf(stderr, "failed to create tap\n");
			remove_taps(conf);
			close(s);
			return -1;
		}

	close(s);
	return 0;
}

int
exec_bhyve(struct vm *vm)
{
	struct vm_conf *conf = vm->conf;
	struct disk_conf *dc;
	struct iso_conf *ic;
	struct net_conf *nc;
	pid_t pid;
	int i, pcid;
	char **args;

	args = malloc(sizeof(char*) *
		      (17
		       + conf->ndisks * 2
		       + conf->nisoes * 2
		       + conf->nnets * 2));
	if (args == NULL)
		return -1;

	i = 0;
	args[i++] = "/usr/sbin/bhyve";
	args[i++] = "-A";
	args[i++] = "-H";
	args[i++] = "-u";
	args[i++] = "-w";
	args[i++] = "-c";
	args[i++] = conf->ncpu;
	args[i++] = "-m";
	args[i++] = conf->memory;
	args[i++] = "-l";
	asprintf(&args[i++], "com1,%s", conf->comport);
	args[i++] = "-s";
	args[i++] = "0,hostbridge";
	args[i++] = "-s";
	args[i++] = "1,lpc";

	pcid = 2;
	STAILQ_FOREACH(dc, &conf->disks, next) {
		args[i++] = "-s";
		asprintf(&args[i++], "%d,%s,%s", pcid++, dc->type, dc->path);
	}
	STAILQ_FOREACH(ic, &conf->isoes, next) {
		args[i++] = "-s";
		asprintf(&args[i++], "%d,%s,%s", pcid++, ic->type, ic->path);
	}
	STAILQ_FOREACH(nc, &conf->nets, next) {
		args[i++] = "-s";
		asprintf(&args[i++], "%d,%s,%s", pcid++, nc->type, nc->tap);
	}
	args[i++] = conf->name;
	args[i++] = NULL;

	pid = fork();
	if (pid > 0) {
		free(args[10]);
		free(args);
	} else if (pid == 0) {
		execv(args[0],args);
		fprintf(stderr, "can not exec %s\n", args[0]);
		exit(1);
	} else {
		fprintf(stderr, "can not fork (%s)\n", strerror(errno));
		exit(1);
	}

	vm->pid = pid;
	vm->state = RUN;
	EV_SET(&vm->kevent, vm->pid, EVFILT_PROC, EV_ADD,
	       NOTE_EXIT, 0, vm);
	kevent(kq, &vm->kevent, 1, NULL, 0, NULL);

	return 0;
}

int
destroy_vm(struct vm_conf *conf)
{
	pid_t pid;
	int status;
	char *args[4];

	args[0]="/usr/sbin/bhyvectl";
	args[1]="--destroy";
	asprintf(&args[2], "--vm=%s", conf->name);
	args[3]=NULL;

	pid = fork();
	if (pid > 0) {
		free(args[2]);
	} else if (pid == 0) {
		execv(args[0],args);
		fprintf(stderr, "can not exec %s\n", args[0]);
		exit(1);
	} else {
		fprintf(stderr, "can not fork (%s)\n", strerror(errno));
		exit(1);
	}

	if (waitpid(pid, &status, 0) < 0) {
		fprintf(stderr, "wait error (%s)\n", strerror(errno));
		exit(1);
	}

	return 0;
}

#if 0
int
do_bhyve(struct vm_conf *conf)
{
	int status;

	if (assign_taps(conf) < 0)
		return -1;
reload:
	if (activate_taps(conf) < 0 ||
	    bhyve_load(conf) < 0 ||
	    exec_bhyve(conf) < 0)
		goto err;

	if (waitpid(conf->pid, &status, 0) < 0) {
		fprintf(stderr, "wait error (%s)\n", strerror(errno));
		exit(1);
	}

	if (WIFEXITED(status) && (WEXITSTATUS(status) == 0))
	    goto reload;

	remove_taps(conf);
	destroy_vm(conf);
	return 0;
err:
	remove_taps(conf);
	destroy_vm(conf);
	return -1;
}
#endif

int
load_config_files()
{
	char *path;
	DIR *d;
	struct dirent *ent;
	struct vm_conf *conf;

	d = opendir(config_directory);
	if (d == NULL) {
		fprintf(stderr,"can not open %s\n", config_directory);
		return -1;
	}

	while ((ent = readdir(d)) != NULL) {
		if (ent->d_namlen > 0 &&
		    ent->d_name[0] == '.')
			continue;
		if (asprintf(&path, "%s/%s", config_directory, ent->d_name) < 0)
			continue;
		conf = parse_file(path);
		free(path);
		if (conf == NULL)
			continue;
		SLIST_INSERT_HEAD(&vm_conf_list, conf, next);
	}

	closedir(d);

	SLIST_FOREACH(conf, &vm_conf_list, next) {
		dump_vm_conf(conf);
	}

	return 0;
}

int
start_vm(struct vm *vm)
{
	pid_t pid;
	struct vm_conf *conf = vm->conf;

	if (activate_taps(conf) < 0)
		return -1;

	pid = bhyve_load(conf);
	if (pid < 0) {
		remove_taps(conf);
		return -1;
	}
	vm->pid = pid;
	vm->state = LOAD;

	EV_SET(&vm->kevent, vm->pid, EVFILT_PROC, EV_ADD,
	       NOTE_EXIT, 0, vm);
	kevent(kq, &vm->kevent, 1, NULL, 0, NULL);

	return 0;
}

int
start_virtual_machines()
{
	struct vm_conf *conf;
	struct vm *vm;
	struct kevent sigev[3];

	EV_SET(&sigev[0], SIGTERM, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
	EV_SET(&sigev[1], SIGINT,  EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
	EV_SET(&sigev[2], SIGHUP,  EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
	if (kevent(kq, sigev, 3, NULL, 0, NULL) < 0)
		return -1;

	SLIST_FOREACH(conf, &vm_conf_list, next) {
		vm = malloc(sizeof(struct vm));
		vm->conf = conf;
		vm->state = STOP;
		vm->pid = -1;
		if (conf->boot != NO)
			if (assign_taps(conf) < 0 ||
			    start_vm(vm) < 0) {
				free(vm);
				continue;
			}
		SLIST_INSERT_HEAD(&vm_list, vm, next);
	}

	return 0;
}

int
event_loop()
{
	struct kevent ev;
	struct vm *vm;
	int status;

wait:
	if (kevent(kq, NULL, 0, &ev, 1, NULL) < 0) {
		if (errno == EINTR) goto wait;
		return -1;
	}

	switch (ev.filter) {
	case EVFILT_PROC:
		vm = ev.udata;
		vm->kevent.flags = EV_DELETE;
		kevent(kq, &vm->kevent, 1, NULL, 0, NULL);
		if (waitpid(vm->pid, &status, 0) < 0) {
			fprintf(stderr, "wait error (%s)\n",
				strerror(errno));
		}
		switch (vm->state) {
		case STOP:
			break;
		case LOAD:
			exec_bhyve(vm);
			break;
		case RUN:
			if (WIFEXITED(status) && (WEXITSTATUS(status) == 0)) {
				start_vm(vm);
				break;
			}
			remove_taps(vm->conf);
			destroy_vm(vm->conf);
			vm->state=TERMINATE;
			break;
		case TERMINATE:
			break;
		}
		break;
	case EVFILT_SIGNAL:
		switch (ev.ident) {
		case SIGTERM:
		case SIGINT:
			return 0;
		case SIGHUP:
			// do_reload();
			goto wait;
		}
		break;
	default:
		// unknown event
		return -1;
	}

	goto wait;

	return 0;
}

int
stop_virtual_machines()
{
	return 0;
}

int
main(int argc, char *argv[])
{
	kq = kqueue();

#if 0
	struct vm_conf *conf = parse_file("./freebsd");
	dump_vm_conf(conf);
	do_bhyve(conf);
	free_vm_conf(conf);
#endif
	if (load_config_files() < 0 ||
	    start_virtual_machines())
		return 1;

	event_loop();

	stop_virtual_machines();
	close(kq);
	return 0;
}
