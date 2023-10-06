#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <glob.h>
#include <libgen.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>

#include "log.h"
#include "confparse.h"
#include "bmd.h"
#include "server.h"

extern FILE *yyin;
extern int yynerrs;
extern int lineno;
extern struct cfsections cfglobals;
extern struct cfsections cftemplates;
extern struct cfsections cfvms;
extern struct vartree *global_vars;

struct input_file {
	FILE *fp;
	char *filename;
	int line;
	TAILQ_ENTRY(input_file) next;
};

TAILQ_HEAD(input_file_head, input_file);
static struct input_file_head input_file_list = TAILQ_HEAD_INITIALIZER(input_file_list);
static struct input_file *cur_file;

static struct cfsection *lookup_template(const char *name);
static int vm_conf_set_params(struct vm_conf *conf, struct cfsection *vm);

static int
parse_int(int *val, char *value)
{
	long n;
	char *p;

	n = strtol(value, &p, 10);
	if (*p != '\0')
		return -1;
	*val = n;
	return 0;
}

static char *token_to_string(struct variables *vars, struct cftokens *tokens);

static int
parse_apply(struct vm_conf *conf, struct cfvalue *vl)
{
	int rc;
	struct cfsection *tp;
	char *val, *argval;
	struct vartree *args, *old_args;
	struct cfargdef *def;
	struct cfarg    *arg;

	val = token_to_string(&conf->vars, &vl->tokens);
	if (val == NULL)
		return -1;

	tp = lookup_template(val);
	if (tp == NULL) {
		ERR("%s: unknown template %s\n", conf->name, val);
		free(val);
		return -1;
	}
	if (tp->applied) {
		ERR("%s: template %s is already applied\n", conf->name, val);
		free(val);
		return 0;
	}
	free(val);

	if ((args = malloc(sizeof(*args))) == NULL)
		return -1;
	RB_INIT(args);

	arg = TAILQ_FIRST(&vl->args);
	TAILQ_FOREACH (def, &tp->argdefs, next) {
		argval = token_to_string(&conf->vars, arg ? &arg->tokens : &def->tokens);
		if (set_var0(args, def->name, argval ? argval : "") < 0)
			ERR("failed to set \"%s\" argument! (%s)\n",
			    def->name, strerror(errno));
		free(argval);
		arg = arg ? TAILQ_NEXT(arg, next) : NULL;
	}

	tp->applied++;
	old_args = conf->vars.args;
	conf->vars.args = args;
	rc = vm_conf_set_params(conf, tp);
	conf->vars.args = old_args;
	free_vartree(args);
	return rc;
}

static int
parse_name(struct vm_conf *conf, char *val)
{
	if (set_var(&conf->vars, "NAME", val) < 0)
		ERR("failed to set \"NAME\" variable! (%s)\n",
		    strerror(errno));
	set_name(conf, val);
	return 0;
}

static int
parse_ncpu(struct vm_conf *conf, char *val)
{
	int n;

	if (parse_int(&n, val) < 0)
		return -1;

	set_ncpu(conf, n);
	return 0;
}

static int
parse_memory(struct vm_conf *conf, char *val)
{
	char *p;

	strtol(val, &p, 10);
	switch (*p) {
	case '\0':
		break;
	case 'T':
	case 't':
	case 'G':
	case 'g':
	case 'M':
	case 'm':
	case 'K':
	case 'k':
		if (p[1] != '\0')
			return -1;
		break;
	default:
		return -1;
	}

	set_memory_size(conf, val);
	return 0;
}

static int
parse_passthru(struct vm_conf *conf, char *val)
{
	char *p;
	for (p = val; *p != '\0'; p++)
		if (*p != '/' && (*p < '0' || *p > '9'))
			return -1;

	return add_passthru_conf(conf, val);
}

static int
parse_disk(struct vm_conf *conf, char *val)
{
	size_t n;
	char * const *p;
	static char * const types[] = {
		"ahci-hd", "virtio-blk", "nvme" };

	ARRAY_FOREACH (p, types) {
		n = strlen(*p);
		if (strncmp(val, *p, n) == 0 && val[n] == ':')
			return add_disk_conf(conf, *p, &val[n+1]);
	}

	return add_disk_conf(conf, "virtio-blk", val);
}

static int
parse_iso(struct vm_conf *conf, char *val)
{
	size_t n;
	char * const *p;
	static char * const types[] = { "ahci-cd" };

	ARRAY_FOREACH (p, types) {
		n = strlen(*p);
		if (strncmp(val, *p, n) == 0 && val[n] == ':')
			return add_iso_conf(conf, *p, &val[n+1]);
	}

	return add_iso_conf(conf, "ahci-cd", val);
}

static int
parse_net(struct vm_conf *conf, char *val)
{
	size_t n;
	char * const *p;
	static char * const types[] = { "virtio-net", "e1000" };

	ARRAY_FOREACH (p, types) {
		n = strlen(*p);
		if (strncmp(val, *p, n) == 0 && val[n] == ':')
			return add_net_conf(conf, *p, &val[n+1]);
	}

	return add_net_conf(conf, "virtio-net", val);
}

static int
parse_loadcmd(struct vm_conf *conf, char *val)
{
	set_loadcmd(conf, val);
	return 0;
}

static int
parse_installcmd(struct vm_conf *conf, char *val)
{
	set_installcmd(conf, val);
	return 0;
}

static int
parse_err_logfile(struct vm_conf *conf, char *val)
{
	set_err_logfile(conf, val);
	return 0;
}

static int
parse_loader(struct vm_conf *conf, char *val)
{
	char * const *p;
	static char * const values[] = { "uefi", "csm", "bhyveload", "grub" };

	ARRAY_FOREACH (p, values)
		if (strcasecmp(val, *p) == 0)
			return set_loader(conf, val);
	return -1;
}

static int
parse_loader_timeout(struct vm_conf *conf, char *val)
{
	int timeout;

	if (parse_int(&timeout, val) < 0)
		return -1;

	return set_loader_timeout(conf, timeout);
}

static int
parse_stop_timeout(struct vm_conf *conf, char *val)
{
	int timeout;

	if (parse_int(&timeout, val) < 0)
		return -1;

	return set_stop_timeout(conf, timeout);
}

static int
parse_grub_run_partition(struct vm_conf *conf, char *val)
{
	return set_grub_run_partition(conf, val);
}

static int
parse_debug_port(struct vm_conf *conf, char *val)
{
	return set_debug_port(conf, val);
}

static int
is_in_group(char *user, gid_t base, gid_t target)
{
	gid_t *grlist = NULL;
	int i, ngroups;

	ngroups = sysconf(_SC_NGROUPS_MAX) + 1;
	if ((grlist = malloc(sizeof(gid_t) * ngroups)) == NULL ||
	    getgrouplist(user, base, grlist, &ngroups) < 0)
		goto err;

	for (i = 0; i < ngroups; i++)
		if (grlist[i] == target)
			break;
	if (i == ngroups)
		goto err;

	free(grlist);
	return 0;
err:
	free(grlist);
	return -1;
}

static int
parse_owner(struct vm_conf *conf, char *val)
{
	char *user, *group, *val2 = NULL;
	struct passwd *pwd;
	struct group  *grp;

	if (strchr(val, ':') != NULL) {
		if ((val2 = strdup(val)) == NULL)
			return -1;
		group = strchr(val2, ':');
		*group++ = '\0';
		user = val2;
	} else {
		user = val;
		group = NULL;
	}

	if ((pwd = getpwnam(user)) == NULL)
		goto err;

	if (get_owner(conf) != 0 && get_owner(conf) != pwd->pw_uid) {
		ERR("%s\n", "Changing owner is not allowed.");
		goto err;
	}

	if (group != NULL) {
		if ((grp = getgrnam(group)) == NULL)
			goto err;
		if (get_owner(conf) != 0 &&
		    is_in_group(pwd->pw_name, pwd->pw_gid, grp->gr_gid) < 0) {
			ERR("%s is not a member of %s group.\n", user, group);
			goto err;
		}
	}

	set_owner(conf, pwd->pw_uid);
	if (set_var(&conf->vars, "OWNER", user) < 0)
		ERR("failed to set \"OWNER\" variable! (%s)\n",
		    strerror(errno));

	if (group != NULL) {
		set_group(conf, grp->gr_gid);
		if (set_var(&conf->vars, "GROUP", group) < 0)
			ERR("failed to set \"GROUP\" variable! (%s)\n",
			    strerror(errno));
	}

	free(val2);
	return 0;
err:
	free(val2);
	return -1;
}

static int
parse_boot(struct vm_conf *conf, char *val)
{
	char * const *p;
	static char * const values[] = { "yes", "true", "oneshot", "always" };
	static enum BOOT const r[] = { YES, YES, ONESHOT, ALWAYS, NO };

	ARRAY_FOREACH (p, values)
		if (strcasecmp(val, *p) == 0)
			break;

	return set_boot(conf, r[p - values]);
}

static int
parse_hostbridge(struct vm_conf *conf, char *val)
{
	char * const *p;
	static char * const values[] = {
		"none", "standard", "intel", "amd" };
	static enum HOSTBRIDGE_TYPE const t[] = {
		NONE, INTEL, INTEL, AMD };

	ARRAY_FOREACH (p, values)
		if (strcasecmp(val, *p) == 0)
			break;

	if (p == &values[sizeof(values) / sizeof(values[0])])
		return -1;

	return set_hostbridge(conf, t[p - values]);
}

static int
parse_backend(struct vm_conf *conf, char *val)
{
	if (vm_method_exists(val) < 0)
		return -1;

	return set_backend(conf,val);
}

static int
parse_keymap(struct vm_conf *conf, char *val)
{
	return set_keymap(conf, val);
}

static int
parse_boot_delay(struct vm_conf *conf, char *val)
{
	int delay;

	if (parse_int(&delay, val) < 0)
		return -1;

	return set_boot_delay(conf, delay);
}

static int
parse_comport(struct vm_conf *conf, char *val)
{
	return set_comport(conf, val);
}

static bool
parse_boolean(const char *value)
{
	return (strcasecmp(value, "yes") == 0 ||
		strcasecmp(value, "true") == 0);
}

static int
parse_reboot_on_change(struct vm_conf *conf, char *val)
{
	return set_reboot_on_change(conf, parse_boolean(val));
}

static int
parse_install(struct vm_conf *conf, char *val)
{
	return set_install(conf, parse_boolean(val));
}

static int
parse_graphics(struct vm_conf *conf, char *val)
{
	return set_fbuf_enable(conf->fbuf, parse_boolean(val));
}

static int
parse_graphics_port(struct vm_conf *conf, char *val)
{
	int port;

	if (parse_int(&port, val) < 0)
		return -1;

	return set_fbuf_port(conf->fbuf, port);
}

static int
parse_graphics_listen(struct vm_conf *conf, char *val)
{
	return set_fbuf_ipaddr(conf->fbuf, val);
}

static int
parse_graphics_res(struct vm_conf *conf, char *val)
{
	char *p;
	int width, height;

	if ((p = strchr(val, 'x')) == NULL)
		return -1;

	*p = '\0';

	if (parse_int(&width, val) < 0 || parse_int(&height, p + 1) < 0)
		return -1;

	return set_fbuf_res(conf->fbuf, width, height);
}

static int
parse_graphics_vga(struct vm_conf *conf, char *val)
{
	return set_fbuf_vgaconf(conf->fbuf, val);
}

static int
parse_graphics_wait(struct vm_conf *conf, char *val)
{
	return set_fbuf_wait(conf->fbuf, parse_boolean(val));
}

static int
parse_graphics_password(struct vm_conf *conf, char *val)
{
	return set_fbuf_password(conf->fbuf, val);
}

static int
parse_xhci_mouse(struct vm_conf *conf, char *val)
{
	return set_mouse(conf, parse_boolean(val));
}

static int
parse_wired_memory(struct vm_conf *conf, char *val)
{
	return set_wired_memory(conf, parse_boolean(val));
}

static int
parse_utctime(struct vm_conf *conf, char *val)
{
	return set_utctime(conf, parse_boolean(val));
}

typedef int (*pfunc)(struct vm_conf *conf, char *val);
typedef void (*cfunc)(struct vm_conf *conf);

struct parser_entry {
	char *name;
	pfunc parse;
	cfunc clear;
};

/* must be sorted by name */
struct parser_entry parser_list[] = {
	{ "backend", &parse_backend, NULL },
	{ "boot", &parse_boot, NULL },
	{ "boot_delay", &parse_boot_delay, NULL },
	{ "comport", &parse_comport, NULL },
	{ "debug_port", &parse_debug_port, NULL },
	{ "disk", &parse_disk, &clear_disk_conf },
	{ "err_logfile", &parse_err_logfile, NULL },
	{ "graphics", &parse_graphics, NULL },
	{ "graphics_listen", &parse_graphics_listen, NULL },
	{ "graphics_password", &parse_graphics_password, NULL },
	{ "graphics_port", &parse_graphics_port, NULL },
	{ "graphics_res", &parse_graphics_res, NULL },
	{ "graphics_vga", &parse_graphics_vga, NULL },
	{ "graphics_wait", &parse_graphics_wait, NULL },
	{ "grub_run_partition", &parse_grub_run_partition, NULL },
	{ "hostbridge", &parse_hostbridge, NULL },
	{ "install", &parse_install, NULL },
	{ "installcmd", &parse_installcmd, NULL },
	{ "iso", &parse_iso, &clear_iso_conf },
	{ "keymap", &parse_keymap, NULL },
	{ "loadcmd", &parse_loadcmd, NULL },
	{ "loader", &parse_loader, NULL },
	{ "loader_timeout", &parse_loader_timeout, NULL },
	{ "memory", &parse_memory, NULL },
	{ "name", &parse_name, NULL },
	{ "ncpu", &parse_ncpu, NULL },
	{ "network", &parse_net, &clear_net_conf },
	{ "owner", &parse_owner, NULL },
	{ "passthru", &parse_passthru, &clear_passthru_conf },
	{ "reboot_on_change", &parse_reboot_on_change, NULL },
	{ "stop_timeout", &parse_stop_timeout, NULL },
	{ "utctime", &parse_utctime, NULL },
	{ "wired_memory", &parse_wired_memory, NULL },
	{ "xhci_mouse", &parse_xhci_mouse, NULL },
};

static int
compare_parser_entry(const void *a, const void *b)
{
	const char *name = a;
	const struct parser_entry *ent = b;
	return strcasecmp(name, ent->name);
}

static int
check_conf(struct vm_conf *conf)
{
	char *name = conf->name;

	if (name == NULL) {
		ERR("%s\n", "vm name is required");
		return -1;
	}

	if (conf->ncpu == NULL) {
		ERR("ncpu is required for vm %s\n", name);
		return -1;
	}

	if (conf->memory == NULL) {
		ERR("memory is required for vm %s\n", name);
		return -1;
	}

	if (strcmp(conf->backend, "bhyve") == 0 && conf->loader == NULL) {
		ERR("loader is required for vm %s\n", name);
		return -1;
	}

	return 0;
}

static struct cfsection *
lookup_template(const char *name)
{
	struct cfsection *tp;

	TAILQ_FOREACH (tp, &cftemplates, next)
		if (strcmp(tp->name, name) == 0)
			return tp;
	return NULL;
}

static int
calc_expr(struct variables *vars, struct cfexpr *ex, long *v, char *fn, int ln)
{
	char *p, *val;
	long n, left, right;

	switch (ex->type) {
	case CF_NUM:
		n = strtol(ex->val, &p, 0);
		if (*p != '\0') {
			ERR("%s line %d: %s is not a number\n",
			    fn, ln, ex->val);
			return -1;
		}
		*v = n;
		return 0;
	case CF_VAR:
		if (vars == NULL) {
			*v = 0;
			return 0;
		}
		if ((val = get_var(vars, ex->val)) == NULL) {
			ERR("%s line %d: ${%s} is undefined\n",
			    fn, ln, ex->val);
			return -1;
		}
		n = strtol(val, &p, 0);
		if (*p != '\0') {
			ERR("%s line %d: ${%s} is not a number\n",
			    fn, ln, ex->val);
			return -1;
		}
		*v = n;
		return 0;
	case CF_EXPR:
		if (calc_expr(vars, ex->left, &left, fn, ln) < 0)
			return -1;
		if (ex->op == '~') {
			*v = -1 * left;
			return 0;
		}
		if (calc_expr(vars, ex->right, &right, fn, ln) < 0)
			return -1;
		switch (ex->op) {
		case '+':
			*v = left + right;
			return 0;
		case '-':
			*v = left - right;
			return 0;
		case '*':
			*v = left * right;
			return 0;
		case '/':
			if (right == 0) {
				ERR("%s line %d: divided by zero\n", fn, ln);
				return -1;
			}
			*v = left / right;
			return 0;
		case '%':
			*v = left % right;
			return 0;
		}
		break;
	default:
		break;
	}

	ERR("%s line %d: unknown operator\n", fn, ln);
	return -1;
}

static char *
token_to_string(struct variables *vars, struct cftokens *tokens)
{
	FILE *fp;
	char *str, *val;
	size_t len;
	struct cftoken *tk;
	long num;

	if ((fp = open_memstream(&str, &len)) == NULL)
		return NULL;

	TAILQ_FOREACH(tk, tokens, next) {
		switch (tk->type) {
		case CF_STR:
			fwrite(tk->s, 1, tk->len, fp);
			break;
		case CF_VAR:
			if (vars == NULL)
				continue;
			if ((val = get_var(vars, tk->s)) == NULL) {
				ERR("%s line %d: ${%s} is undefined",
				    tk->filename, tk->lineno, tk->s);
				goto err;
			}
			fwrite(val, 1, strlen(val), fp);
			break;
		case CF_EXPR:
			if (calc_expr(vars, tk->expr, &num, tk->filename, tk->lineno) < 0)
				goto err;
			fprintf(fp, "%ld", num);
			break;
		case CF_NUM:
			/* Unused */
			break;
		}
	}


	fclose(fp);
	return str;
err:
	fclose(fp);
	free(str);
	return NULL;
}

int
apply_global_vars(struct cfsection *sc)
{
	struct cfparam *pr;
	struct cfvalue *vl;
	char *val;
	struct variables vars;

	vars.global = global_vars;
	vars.local = NULL;
	vars.args = NULL;

	TAILQ_FOREACH(pr, &sc->params, next)
		if (pr->key->type == CF_VAR) {
			vl = TAILQ_FIRST(&pr->vals);
			val = token_to_string(&vars, &vl->tokens);
			if (val == NULL)
				continue;
			if (set_var(&vars, pr->key->s, val) < 0)
				ERR("failed to set \"%s\" variable! (%s)\n",
				    pr->key->s, strerror(errno));

			free(val);
		}

	return 0;
}

static int
gl_conf_set_params(struct global_conf *gc, struct variables *vars,
		   struct cfsection *sc)
{
	struct cfparam *pr;
	struct cfvalue *vl;
	char *key, *val, **t, *p, *nmdm_offset_s = NULL;

	TAILQ_FOREACH(pr, &sc->params, next) {
		key = pr->key->s;
		if (pr->key->type == CF_VAR) {
			vl = TAILQ_FIRST(&pr->vals);
			val = token_to_string(vars, &vl->tokens);
			if (val == NULL)
				continue;
			if (set_var(vars, key, val) < 0)
				ERR("failed to set \"%s\" variable! (%s)\n",
				    key, strerror(errno));

			free(val);
			continue;
		}
		switch (key[0]) {
		case 'c':
			if (strcmp(key, "cmd_socket_mode") == 0)
				t = &gc->unix_domain_socket_mode;
			else if (strcmp(key, "cmd_socket_path") == 0)
				t = &gc->cmd_sock_path;
			else
				goto unknown;
			break;
		case 'v':
			if (strcmp(key, "vars_directory") == 0)
				t = &gc->vars_dir;
			else
				goto unknown;
			break;
		case 'n':
			if (strcmp(key, "nmdm_offset") == 0)
				t = &nmdm_offset_s;
			else
				goto unknown;
			break;
		case 'p':
			if (strcmp(key, "pid_file") == 0)
				t = &gc->pid_path;
			else if (strcmp(key, "plugin_directory") == 0)
				t = &gc->plugin_dir;
			else
				goto unknown;
			break;
		default:
			goto unknown;
		}

		TAILQ_FOREACH(vl, &pr->vals, next) {
			val = token_to_string(vars, &vl->tokens);
			if (val == NULL)
				continue;
			if (*t == NULL)
				*t = val;
			else
				free(val);
		}
		continue;

	unknown:
		ERR("%s: unknown key %s\n", "global", key);

	}

	if (nmdm_offset_s) {
		gc->nmdm_offset = strtol(nmdm_offset_s, &p, 0);
		if (*p != '\0')
			gc->nmdm_offset = DEFAULT_NMDM_OFFSET;
		free(nmdm_offset_s);
	}

	return 0;
}

static int
vm_conf_set_params(struct vm_conf *conf, struct cfsection *sc)
{
	struct cfparam *pr;
	struct cfvalue *vl;
	struct cftoken *tk;
	struct parser_entry *parser;
	struct vm_conf_entry *conf_ent = (struct vm_conf_entry *)conf;
	char *key, *val;
	int rc;

	TAILQ_FOREACH(pr, &sc->params, next) {
		key = pr->key->s;
		if (pr->key->type == CF_VAR) {
			vl = TAILQ_FIRST(&pr->vals);
			val = token_to_string(&conf->vars, &vl->tokens);
			if (val == NULL)
				continue;
			if (set_var(&conf->vars, key, val) < 0)
				ERR("failed to set \"%s\" variable! (%s)\n",
				    key, strerror(errno));
			free(val);
			continue;
		}
		if (strcasecmp(key, ".apply") == 0) {
			TAILQ_FOREACH(vl, &pr->vals, next)
				parse_apply(conf, vl);
			continue;
		}
		parser = bsearch(key, parser_list,
				 sizeof(parser_list) / sizeof(parser_list[0]),
				 sizeof(parser_list[0]), compare_parser_entry);
		if (parser && parser->clear != NULL && pr->operator == 0)
			(*parser->clear)(conf);
		TAILQ_FOREACH(vl, &pr->vals, next) {
			val = token_to_string(&conf->vars, &vl->tokens);
			if (val == NULL)
				continue;
			if (parser) {
				if ((*parser->parse)(conf, val) < 0) {
					tk = TAILQ_FIRST(&vl->tokens);
					tk = tk ? tk : pr->key;
					ERR("%s line %d: vm %s: invalid value: %s = %s\n",
					    tk->filename, tk->lineno,
					    sc->name, key, val);
				}
			} else {
				rc = call_plugin_parser(&conf_ent->pl_data, key, val);
				if (rc > 0) {
					ERR("%s line %d: %s: unknown key %s\n",
					    pr->key->filename, pr->key->lineno,
					    sc->name, key);
				} else	if (rc < 0) {
					tk = TAILQ_FIRST(&vl->tokens);
					tk = tk ? tk : pr->key;
					ERR("%s line %d: %s: invalid value: %s = %s\n",
					    tk->filename, tk->lineno,
					    sc->name, key, val);
				}
			}
			free(val);
		}
	}

	return 0;
}

void
free_cfexpr(struct cfexpr *ex)
{
	if (ex == NULL)
		return;
	free(ex->val);
	free_cfexpr(ex->left);
	free_cfexpr(ex->right);
	free(ex);
}

void
free_cftoken(struct cftoken *tk)
{
	if (tk == NULL)
		return;
	free_cfexpr(tk->expr);
	free(tk->s);
	free(tk);
}

void
free_cftokens(struct cftokens *ts)
{
	struct cftoken *tk, *tn;
	if (ts == NULL)
		return;
	TAILQ_FOREACH_SAFE(tk, ts, next, tn)
		free_cftoken(tk);
}

void
free_cfvalue(struct cfvalue *vl)
{
	if (vl == NULL)
		return;
	free_cftokens(&vl->tokens);
	free(vl);
}

void
free_cfvalues(struct cfvalues *vs)
{
	struct cfvalue	*vl, *vn;
	if (vs == NULL)
		return;
	TAILQ_FOREACH_SAFE(vl, vs, next, vn)
		free_cfvalue(vl);
}

void
free_cfparam(struct cfparam *pr)
{
	if (pr == NULL)
		return;
	free_cfvalues(&pr->vals);
	free_cftoken(pr->key);
	free(pr);
}

void
free_cfparams(struct cfparams *ps)
{
	struct cfparam *pr, *pn;
	if (ps == NULL)
		return;
	TAILQ_FOREACH_SAFE(pr, ps, next, pn)
		free_cfparam(pr);
}

void
free_cfsection(struct cfsection *sec)
{
	if (sec == NULL)
		return;
	free_cfparams(&sec->params);
	free(sec->name);
	free(sec);
}

void
free_cfsections(struct cfsections *ss)
{
	struct cfsection *sc, *sn;

	if (ss == NULL)
		return;
	TAILQ_FOREACH_SAFE(sc, ss, next, sn)
		free_cfsection(sc);
}

void
free_cfarg(struct cfarg *ag)
{
	if (ag == NULL)
		return;
	free_cftokens(&ag->tokens);
	free(ag);
}

void
free_cfargs(struct cfargs *as)
{
	struct cfarg *ag, *an;
	if (as == NULL)
		return;
	TAILQ_FOREACH_SAFE (ag, as, next, an)
		free_cfarg(ag);
	free(as);
}

void
free_cfargdef(struct cfargdef *ad)
{
	if (ad == NULL)
		return;
	free(ad->name);
	free_cftokens(&ad->tokens);
	free(ad);
}

void
free_cfargdefs(struct cfargdefs *ds)
{
	struct cfargdef *ad, *an;
	if (ds == NULL)
		return;
	TAILQ_FOREACH_SAFE (ad, ds, next, an)
		free_cfargdef(ad);
	free(ds);
}

static int
push_file(char *fn)
{
	FILE *fp;
	struct input_file *file;
	char *rpath;
	struct stat st;

	if (fn == NULL || (rpath = realpath(fn, NULL)) == NULL)
		return 0;

	if (strncmp(rpath, "/dev", 4) == 0) {
		ERR("%s: include denied\n", rpath);
		goto err;
	}

	TAILQ_FOREACH (file, &input_file_list, next)
		if (strcmp(file->filename, rpath) == 0) {
			ERR("%s is already included\n", rpath);
			goto err;
		}

	if ((file = malloc(sizeof(*file))) == NULL)
		goto err;

	if ((fp = fopen(rpath, "r")) == NULL) {
		ERR("failed to open %s\n", rpath);
		goto err2;
	}
	if (fstat(fileno(fp), &st) < 0 || (! S_ISREG(st.st_mode))) {
		ERR("%s is not a file\n", rpath);
		goto err3;
	}
	file->filename = rpath;
	file->line = 0;
	file->fp = fp;
	TAILQ_INSERT_TAIL(&input_file_list, file, next);
	if (cur_file == NULL)
		cur_file = TAILQ_FIRST(&input_file_list);
	INFO("load config %s\n", rpath);
	return 0;
err3:
	fclose(fp);
err2:
	free(file);
err:
	free(rpath);
	return -1;
}

static struct input_file *
pop_file()
{
	return (cur_file = cur_file ? TAILQ_NEXT(cur_file, next) : NULL);
}

static struct input_file *
peek_file()
{
	return cur_file;
}

uid_t
peek_fileowner()
{
	struct stat st;
	char *fn = cur_file ? cur_file->filename :
		TAILQ_LAST_FAST(&input_file_list, input_file, next)->filename;
	return stat(fn, &st) < 0 ? UID_NOBODY: st.st_uid;
}

char *
peek_filename()
{
	return cur_file ? cur_file->filename :
		TAILQ_LAST_FAST(&input_file_list, input_file, next)->filename;
}

static void
free_file(struct input_file *file)
{
	if (file == NULL)
		return;
	fclose(file->fp);
	free(file->filename);
	free(file);
}

static void
clean_file()
{
	struct input_file *inf, *n;
	TAILQ_FOREACH_SAFE(inf, &input_file_list, next ,n)
		free_file(inf);
	TAILQ_INIT(&input_file_list);
}

void
glob_path(struct cftokens *ts)
{
	struct cftoken *tk, *tn;
	char *path, *conf, *dir, *npath;
	struct variables vars;
	struct stat st;
	glob_t g;
	int i;

	vars.global = global_vars;
	vars.local = NULL;
	vars.args = NULL;

	if ((tk = TAILQ_FIRST(ts)) == NULL)
		goto ret;

	if (stat(tk->filename, &st) < 0 || st.st_uid != 0) {
		ERR("%s: .include macro is not allowed.\n",
		    tk->filename);
		goto ret;
	}

	if ((path = token_to_string(&vars, ts)) == NULL)
		goto ret;

	if (path[0] != '/' &&
	    (conf = strdup(tk->filename)) != NULL) {
		dir = dirname(conf);
		if (asprintf(&npath, "%s/%s", dir, path) >= 0) {
			free(path);
			path = npath;
		}
		free(conf);
	}

	if (glob(path, 0, NULL, &g) < 0) {
		ERR("failed to glob %s\n", path);
		goto ret2;
	}

	for (i = 0; i < g.gl_pathc; i++)
		push_file(g.gl_pathv[i]);

	globfree(&g);
ret2:
	free(path);
ret:
	TAILQ_FOREACH_SAFE (tk, ts, next, tn)
		free_cftoken(tk);
	free(ts);
}

int
yywrap(void) {
	struct input_file *ifp;

	if ((ifp = pop_file()) == NULL)
		return 1;

	yyin = ifp->fp;
	lineno = 1;
	return 0;
}

static void
clear_applied()
{
	struct cfsection *sc;

	TAILQ_FOREACH (sc, &cftemplates, next)
		sc->applied = 0;
}

static int
check_duplicate()
{
	struct cfsection *sc;

	TAILQ_FOREACH (sc, &cftemplates, next)
		if (sc->duplicate)
			return -1;
	return 0;
}

int
load_config_file(struct vm_conf_head *list, bool update_gl_conf)
{
	struct cfsection *sc, *sn;
	struct vm_conf *conf;
	struct vm_conf_entry *conf_ent;
	struct input_file *inf;
	struct global_conf *global_conf;
	struct vartree *gv;
	struct variables vars;
	int rc = 0;
	struct plugin_data_head head;
	struct passwd *pw;

	gv = malloc(sizeof(*gv));
	global_conf = calloc(1, sizeof(*global_conf));
	if (global_conf == NULL || gv == NULL) {
		free(global_conf);
		free(gv);
		ERR("%s\n", "failed to allocate global config memory.");
		return -1;
	}
	RB_INIT(gv);
	vars.local = NULL;
	vars.global = gv;
	vars.args = NULL;
	cur_file = NULL;

	if (set_var0(gv, "LOCALBASE", LOCALBASE) < 0)
		ERR("%s\n", "failed to set \"LOCALBASE\" variable!");

	if (push_file(gl_conf->config_file) < 0) {
		free_vartree(gv);
		free(global_conf);
		return -1;
	}

	inf = peek_file();
	yyin = inf->fp;

	if (yyparse() || yynerrs) {
		fclose(yyin);
		free_vartree(gv);
		free(global_conf);
		rc = -1;
		goto cleanup;
	}

	if (check_duplicate() != 0) {
		free_vartree(gv);
		rc = -1;
		goto cleanup;
	}

	TAILQ_FOREACH(sc, &cfglobals, next)
		if (sc->owner == 0)
			gl_conf_set_params(global_conf, &vars, sc);
		else
			ERR("%s: global section is not allowed.\n", sc->filename);

	if (list == NULL)
		goto set_global;

	load_plugins(global_conf->plugin_dir ?
		     global_conf->plugin_dir : gl_conf->plugin_dir);

	TAILQ_FOREACH(sc, &cfvms, next) {
		if (create_plugin_data(&head) < 0)
			continue;
		if ((conf = create_vm_conf(sc->name)) == NULL) {
			free_plugin_data(&head);
			continue;
		}
		conf->vars.global = gv;
		conf->owner = sc->owner;
		if ((pw = getpwuid(conf->owner)) == NULL ||
		    set_var(&conf->vars, "OWNER", pw->pw_name) < 0)
			ERR("failed to set \"OWNER\" variable! (%s)\n",
			    strerror(errno));
		if (set_var(&conf->vars, "GROUP", "") < 0)
			ERR("failed to set \"GROUP\" variable! (%s)\n",
			    strerror(errno));
		if ((conf_ent = realloc(conf, sizeof(*conf_ent))) == NULL) {
			free_plugin_data(&head);
			free_vm_conf(conf);
			continue;
		}
		conf_ent->pl_data = head;
		conf = &conf_ent->conf;
		clear_applied();
		if (vm_conf_set_params(conf, sc) < 0 ||
		    finalize_vm_conf(conf) < 0 || check_conf(conf) < 0) {
			free_plugin_data(&head);
			free_vm_conf(conf);
			continue;
		}
		LIST_INSERT_HEAD(list, conf_ent, next);
	}

set_global:
	set_global_vars(gv);
	if (update_gl_conf)
		merge_global_conf(global_conf);
	else
		free_global_conf(global_conf);

cleanup:
	yylex_destroy();
	clean_file();

	TAILQ_FOREACH_SAFE(sc, &cfglobals, next, sn)
		free_cfsection(sc);
	TAILQ_INIT(&cfglobals);

	TAILQ_FOREACH_SAFE(sc, &cftemplates, next, sn)
		free_cfsection(sc);
	TAILQ_INIT(&cftemplates);

	TAILQ_FOREACH_SAFE(sc, &cfvms, next, sn)
		free_cfsection(sc);
	TAILQ_INIT(&cfvms);

	if (rc != 0)
		ERR("%s\n", "failed to parse config file");
	return rc;
}
