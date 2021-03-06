/*
 *	"condition" match extension for Xtables
 *
 *	Description: This module allows firewall rules to match using
 *	condition variables available through procfs.
 *
 *	Authors:
 *	Stephane Ouellette <ouellettes [at] videotron ca>, 2002-10-22
 *	Massimiliano Hofer <max [at] nucleus it>, 2006-05-15
 *	Grzegorz Kuczyński <grzegorz.kuczynski [at] koba pl>, 2017-02-27
 *
 *	This program is free software; you can redistribute it and/or modify it
 *	under the terms of the GNU General Public License; either version 2
 *	or 3 of the License, as published by the Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/netfilter/x_tables.h>
#include <asm/uaccess.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <linux/netfilter/xt_condition.h>

#ifndef CONFIG_PROC_FS
#	error "proc file system support is required for this module"
#endif

/* Defaults, these can be overridden on the module command-line. */
static unsigned int condition_list_perms = S_IRUGO | S_IWUSR;
static unsigned int condition_uid_perms = 0;
static unsigned int condition_gid_perms = 0;

MODULE_AUTHOR("Stephane Ouellette <ouellettes@videotron.ca>");
MODULE_AUTHOR("Massimiliano Hofer <max@nucleus.it>");
MODULE_AUTHOR("Jan Engelhardt ");
MODULE_DESCRIPTION("Allows rules to match against condition variables");
MODULE_LICENSE("GPL");
module_param(condition_list_perms, uint, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(condition_list_perms, "permissions on /proc/net/nf_condition/* files");
module_param(condition_uid_perms, uint, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(condition_uid_perms, "user owner of /proc/net/nf_condition/* files");
module_param(condition_gid_perms, uint, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(condition_gid_perms, "group owner of /proc/net/nf_condition/* files");
MODULE_ALIAS("ipt_condition");
MODULE_ALIAS("ip6t_condition");

struct condition_variable {
	struct list_head list;
	struct proc_dir_entry *status_proc;
	unsigned int refcount;
	bool enabled;
	char name[sizeof(((struct xt_condition_mtinfo *)NULL)->name)];
};

/* proc_lock is a user context only semaphore used for write access */
/*           to the conditions' list.                               */
static DEFINE_MUTEX(proc_lock);

struct condition_net {
	struct list_head conditions_list;
	struct proc_dir_entry *proc_net_condition;
	bool after_clear;
};

static int condition_net_id;

static inline struct condition_net *condition_pernet(struct net *net)
{
	return net_generic(net, condition_net_id);
}

static int condition_proc_show(struct seq_file *m, void *data)
{
	const struct condition_variable *var = m->private;

	seq_printf(m, var->enabled ? "1\n" : "0\n");
	return 0;
}

static int condition_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, condition_proc_show, PDE_DATA(inode));
}

static ssize_t
condition_proc_write(struct file *file, const char __user *buffer,
                     size_t length, loff_t *loff)
{
	struct condition_variable *var = PDE_DATA(file_inode(file));
	char newval;

	if (length > 0) {
		if (get_user(newval, buffer) != 0)
			return -EFAULT;
		/* Match only on the first character */
		switch (newval) {
		case '0':
			var->enabled = false;
			break;
		case '1':
			var->enabled = true;
			break;
		}
	}
	return length;
}

static const struct file_operations condition_proc_fops = {
	.open    = condition_proc_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.write   = condition_proc_write,
	.release = single_release,
};

static bool
condition_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
	const struct xt_condition_mtinfo *info = par->matchinfo;
	const struct condition_variable *var   = info->condvar;

	return var->enabled ^ info->invert;
}

static int condition_mt_check(const struct xt_mtchk_param *par)
{
	struct xt_condition_mtinfo *info = par->matchinfo;
	struct condition_variable *var;
	struct condition_net *condition_net = condition_pernet(par->net);

	/* Forbid certain names */
	if (*info->name == '\0' || *info->name == '.' ||
	    info->name[sizeof(info->name)-1] != '\0' ||
	    memchr(info->name, '/', sizeof(info->name)) != NULL) {
		printk(KERN_INFO KBUILD_MODNAME ": name not allowed or too "
		       "long: \"%.*s\"\n", (unsigned int)sizeof(info->name),
		       info->name);
		return -EINVAL;
	}
	/*
	 * Let's acquire the lock, check for the condition and add it
	 * or increase the reference counter.
	 */
	mutex_lock(&proc_lock);
	list_for_each_entry(var, &condition_net->conditions_list, list) {
		if (strcmp(info->name, var->name) == 0) {
			var->refcount++;
			mutex_unlock(&proc_lock);
			info->condvar = var;
			return 0;
		}
	}

	/* At this point, we need to allocate a new condition variable. */
	var = kmalloc(sizeof(struct condition_variable), GFP_KERNEL);
	if (var == NULL) {
		mutex_unlock(&proc_lock);
		return -ENOMEM;
	}

	memcpy(var->name, info->name, sizeof(info->name));
	/* Create the condition variable's proc file entry. */
	var->status_proc = proc_create_data(info->name, condition_list_perms,
	                   condition_net->proc_net_condition, &condition_proc_fops, var);
	if (var->status_proc == NULL) {
		kfree(var);
		mutex_unlock(&proc_lock);
		return -ENOMEM;
	}

	proc_set_user(var->status_proc,
	              make_kuid(&init_user_ns, condition_uid_perms),
	              make_kgid(&init_user_ns, condition_gid_perms));
	var->refcount = 1;
	var->enabled  = false;
	wmb();
	list_add(&var->list, &condition_net->conditions_list);
	mutex_unlock(&proc_lock);
	info->condvar = var;
	return 0;
}

static void condition_mt_destroy(const struct xt_mtdtor_param *par)
{
	const struct xt_condition_mtinfo *info = par->matchinfo;
	struct condition_variable *var = info->condvar;
	struct condition_net *cnet = condition_pernet(par->net);

	if (cnet->after_clear)
		return;

	mutex_lock(&proc_lock);
	if (--var->refcount == 0) {
		list_del(&var->list);
		remove_proc_entry(var->name, cnet->proc_net_condition);
		mutex_unlock(&proc_lock);
		kfree(var);
		return;
	}
	mutex_unlock(&proc_lock);
}

static struct xt_match condition_mt_reg[] __read_mostly = {
	{
		.name       = "condition",
		.revision   = 1,
		.family     = NFPROTO_IPV4,
		.matchsize  = sizeof(struct xt_condition_mtinfo),
		.match      = condition_mt,
		.checkentry = condition_mt_check,
		.destroy    = condition_mt_destroy,
		.me         = THIS_MODULE,
	},
	{
		.name       = "condition",
		.revision   = 1,
		.family     = NFPROTO_IPV6,
		.matchsize  = sizeof(struct xt_condition_mtinfo),
		.match      = condition_mt,
		.checkentry = condition_mt_check,
		.destroy    = condition_mt_destroy,
		.me         = THIS_MODULE,
	},
};

static const char *const dir_name = "nf_condition";

static int __net_init condition_net_init(struct net *net)
{
	struct condition_net *condition_net = condition_pernet(net);
	INIT_LIST_HEAD(&condition_net->conditions_list);
	condition_net->proc_net_condition = proc_mkdir(dir_name, net->proc_net);
	if (condition_net->proc_net_condition == NULL)
		return -EACCES;
	condition_net->after_clear = 0;
	return 0;
}

static void __net_exit condition_net_exit(struct net *net)
{
	struct condition_net *condition_net = condition_pernet(net);
	struct list_head *pos, *q;
	struct condition_variable *var = NULL;

	remove_proc_subtree(dir_name, net->proc_net);
	mutex_lock(&proc_lock);
	list_for_each_safe(pos, q, &condition_net->conditions_list) {
		var = list_entry(pos, struct condition_variable, list);
		list_del(pos);
		kfree(var);
	}
	mutex_unlock(&proc_lock);
	condition_net->after_clear = true;
}

static struct pernet_operations condition_net_ops = {
	.init   = condition_net_init,
	.exit   = condition_net_exit,
	.id     = &condition_net_id,
	.size   = sizeof(struct condition_net),
};


static int __init condition_mt_init(void)
{
	int ret;

	mutex_init(&proc_lock);
	ret = register_pernet_subsys(&condition_net_ops);
	if (ret != 0)
		return ret;

	ret = xt_register_matches(condition_mt_reg, ARRAY_SIZE(condition_mt_reg));
	if (ret < 0) {
		unregister_pernet_subsys(&condition_net_ops);
		return ret;
	}

	return 0;
}

static void __exit condition_mt_exit(void)
{
	xt_unregister_matches(condition_mt_reg, ARRAY_SIZE(condition_mt_reg));
	unregister_pernet_subsys(&condition_net_ops);
}

module_init(condition_mt_init);
module_exit(condition_mt_exit);
