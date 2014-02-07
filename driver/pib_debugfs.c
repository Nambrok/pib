/*
 * pib_debugfs.c - Object inspection & Error injection & Execution trace
 *
 * Copyright (c) 2013,2014 Minoru NAKAMURA <nminoru@nminoru.jp>
 *
 * This code is licenced under the GPL version 2 or BSD license.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/export.h>

#include "pib.h"
#include "pib_packet.h"
#include "pib_trace.h"


static struct dentry *debugfs_root;
static cycles_t	init_timestamp;

static int register_dev(struct dentry *root, struct pib_dev *dev);
static void unregister_dev(struct pib_dev *dev);


void show_timespec(struct seq_file *file, const struct timespec *time_p)
{
	unsigned int value, msec, usec, nsec;
	struct timespec	time;
	struct tm tm;

	time = *time_p;
	time_to_tm(time.tv_sec, 0, &tm);

	value    = time.tv_nsec;

	nsec   = value % 1000;
	value /= 1000;
	usec   = value % 1000;
	value /= 1000;
	msec   = value;

	seq_printf(file, "[%04ld-%02d-%02d %02d:%02d:%02d.%03u,%03u,%03u]",
		   tm.tm_year + 1900, tm.tm_mon  + 1, tm.tm_mday,
		   tm.tm_hour, tm.tm_min, tm.tm_sec,
		   msec, usec, nsec);
}



/******************************************************************************/
/* Objection Inspection                                                       */
/******************************************************************************/

static const char *debug_file_symbols[] = {
	[PIB_DEBUGFS_UCONTEXT] = "ucontext",
	[PIB_DEBUGFS_PD]       = "pd",
	[PIB_DEBUGFS_MR]       = "mr",
	[PIB_DEBUGFS_SRQ]      = "srq",
	[PIB_DEBUGFS_AH]       = "ah",
	[PIB_DEBUGFS_CQ]       = "cq",
	[PIB_DEBUGFS_QP]       = "qp",
};


struct pib_base_record {
	u32	obj_num;
	struct timespec	creation_time;
};


struct pib_ucontext_record {
	struct pib_base_record	base;
	pid_t	pid;
	pid_t	tgid;	
	char	comm[TASK_COMM_LEN];
};


struct pib_pd_record {
	struct pib_base_record	base;
};


struct pib_mr_record {
	struct pib_base_record	base;
	u32	pd_num;
	u8	access_flags;
	u8	is_dma;
	u64	start;
	u64	length;
	u32	lkey;
	u32	rkey;
};


struct pib_srq_record {
	struct pib_base_record	base;
	u32	pd_num;
	int	state;
	int	max_wqe;
	int	nr_wqe;
};


struct pib_ah_record {
	struct pib_base_record	base;
	u32	pd_num;
	u16	dlid;
	u8	ah_flags;
	u8	port_num;
};


struct pib_cq_record {
	struct pib_base_record	base;
	int	state;
	int	max_cqe;
	int	nr_cqe;
	u8	flag;	
	u8	notified;
};


struct pib_qp_record {
	struct pib_base_record	base;
	u32	pd_num;
	u32	send_cq_num;
	u32	recv_cq_num;
	u32	srq_num;
	int	max_swqe;
	int	nr_swqe;
	int	max_rwqe;
	int	nr_rwqe;
	u8	qp_type;
	u8	state;
};


struct pib_record_control {
	struct pib_dev	       *dev;
	enum pib_debugfs_type	type;
	int			count;
	int			pos;
	size_t			record_size;
	struct pib_base_record	records[];
};


static void *inspection_seq_start(struct seq_file *file, loff_t *pos)
{
	struct pib_record_control *control = file->private;

	if (*pos != 0)
		goto next;

	switch (control->type) {
	case PIB_DEBUGFS_AH:
	case PIB_DEBUGFS_QP:
		seq_printf(file, "%-6s %-33s ", "OID", "CREATIONTIME");
		break;

	default:
		seq_printf(file, "%-4s %-33s ", "OID", "CREATIONTIME");
		break;
	}

	switch (control->type) {
	case PIB_DEBUGFS_UCONTEXT:
		seq_printf(file, "%-5s %-5s %-5s\n", "PID", "TIG", "COMM");
		break;

	case PIB_DEBUGFS_MR:
		seq_printf(file, "%-4s %-16s %-16s %-8s %-8s DMA AC\n", "PD", "STATRT", "LENGTH", "LKEY", "RKEY");
		break;

	case PIB_DEBUGFS_SRQ:
		seq_printf(file, "%-4s %-3s %-5s %-5s\n", "PD", "S", "MAX", "CUR");
		break;

	case PIB_DEBUGFS_AH:
		seq_printf(file, "%-4s %-4s %-2s PORT\n", "PD", "DLID", "AC");
		break;

	case PIB_DEBUGFS_CQ:
		seq_printf(file, "%-3s %-5s %-5s %-4s %-4s\n", "S", "MAX", "CUR", "TYPE", "NOTIFY");
		break;

	case PIB_DEBUGFS_QP:
		seq_printf(file, "%-4s %-3s %-5s %-4s %-4s %-4s %-5s %-5s %-5s %-5s\n",
			   "PD", "QT", "STATE", "S-CQ", "R-CQ", "SRQ", "MAX-S", "CUR-S", "MAX-R", "CUR-R");
		break;

	default:
		seq_puts(file, "\n");
		break;
	}

next:
	if (control->count <= *pos)
		return NULL;

	control->pos = *pos;

	return control;
}


static void *inspection_seq_next(struct seq_file *file, void *iter, loff_t *pos)
{
	struct pib_record_control *control = file->private;

	++*pos;

	if (control->count <= *pos)
		return NULL;

	control->pos = *pos;

	return iter;
}


static void inspection_seq_stop(struct seq_file *file, void *iter_ptr)
{
}


static int inspection_seq_show(struct seq_file *file, void *iter)
{
	struct pib_record_control *control = file->private;
	struct pib_base_record *record;
	int pos;

	control = file->private;
	pos     = control->pos;

	record = (struct pib_base_record*)((unsigned long)control->records + (pos * control->record_size));

	switch (control->type) {
	case PIB_DEBUGFS_AH:
	case PIB_DEBUGFS_QP:
		seq_printf(file, "%06x ", record->obj_num);
		break;

	default:
		seq_printf(file, "%04x ", record->obj_num);
		break;
	}

	show_timespec(file, &record->creation_time);

	switch (control->type) {

	case PIB_DEBUGFS_UCONTEXT: {
		struct pib_ucontext_record *ucontext_rec = (struct pib_ucontext_record *)record;
		seq_printf(file, " %5u %5u %s",
			   ucontext_rec->pid, ucontext_rec->tgid, ucontext_rec->comm);
		break;
	}

	case PIB_DEBUGFS_PD:
		break;

	case PIB_DEBUGFS_MR: {
		struct pib_mr_record *mr_rec = (struct pib_mr_record *)record;
		seq_printf(file, " %04x %016llx %016llx %08x %08x %s %x",
			   mr_rec->pd_num, mr_rec->start, mr_rec->length,
			   mr_rec->lkey, mr_rec->rkey,
			   (mr_rec->is_dma ? "DMA" : "USR"),
			   mr_rec->access_flags);
		break;
	}

	case PIB_DEBUGFS_SRQ: {
		struct pib_srq_record *srq_rec = (struct pib_srq_record *)record;
		seq_printf(file, " %04x %-3s %5u %5u",
			   srq_rec->pd_num,
			   ((srq_rec->state == PIB_STATE_OK) ? "OK" : "ERR"),
			   srq_rec->max_wqe, srq_rec->nr_wqe);
		break;
	}

	case PIB_DEBUGFS_AH: {
		struct pib_ah_record *ah_rec = (struct pib_ah_record *)record;
		seq_printf(file, " %04x %04x %2u %u",
			   ah_rec->pd_num,
			   ah_rec->dlid,
			   ah_rec->ah_flags,
			   ah_rec->port_num);
		break;
	}

	case PIB_DEBUGFS_CQ: {
		const char *channel_type;
		struct pib_cq_record *cq_rec = (struct pib_cq_record *)record;
		channel_type = (cq_rec->flag == 0) ? "NONE" :
			((cq_rec->flag == IB_CQ_SOLICITED) ? "SOLI" : "COMP");
		
		seq_printf(file, " %-3s %5u %5u %-4s %-4s",
			   ((cq_rec->state == PIB_STATE_OK) ? "OK " : "ERR"),
			   cq_rec->max_cqe, cq_rec->nr_cqe,
			   channel_type,
			   (cq_rec->notified ? "NOTIFY" : "WAIT"));
		break;
	}

	case PIB_DEBUGFS_QP: {
		struct pib_qp_record *qp_rec = (struct pib_qp_record *)record;
		seq_printf(file, " %04x %-3s %-5s %04x %04x %04x %5u %5u %5u %5u",
			   qp_rec->pd_num,
			   pib_get_qp_type(qp_rec->qp_type), pib_get_qp_state(qp_rec->state),
			   qp_rec->send_cq_num, qp_rec->recv_cq_num, qp_rec->srq_num,
			   qp_rec->max_swqe, qp_rec->nr_swqe,
			   qp_rec->max_rwqe, qp_rec->nr_rwqe);
		break;
	}

	default:
		BUG();
	}

	seq_putc(file, '\n');

	return 0;
}


static const struct seq_operations inspection_seq_ops = {
	.start = inspection_seq_start,
	.next  = inspection_seq_next,
	.stop  = inspection_seq_stop,
	.show  = inspection_seq_show,
};


static int inspection_open(struct inode *inode, struct file *file)
{
	int i, ret;
	struct pib_dev *dev;
	unsigned long flags;
	struct seq_file *seq;
	struct pib_debugfs_entry *entry;
	struct pib_record_control *control;

	entry = inode->i_private;
	dev   = entry->dev;

	switch (entry->type) {

	case PIB_DEBUGFS_UCONTEXT: {
		struct pib_ucontext *ucontext;
		struct pib_ucontext_record *records;

		control = vzalloc(sizeof(struct pib_record_control) +
				  dev->nr_ucontext * sizeof(struct pib_ucontext_record));
		if (!control)
			return -ENOMEM;

		records  = (struct pib_ucontext_record *)control->records;

		i=0;
		spin_lock_irqsave(&dev->lock, flags);
		list_for_each_entry(ucontext, &dev->ucontext_head, list) {
			records[i].base.obj_num       = ucontext->ucontext_num;
			records[i].base.creation_time = ucontext->creation_time;
			records[i].pid  = ucontext->pid;
			records[i].tgid = ucontext->tgid;
			memcpy(records[i].comm, ucontext->comm, sizeof(ucontext->comm));
			i++;
		}
		spin_unlock_irqrestore(&dev->lock, flags);
		
		control->count = i;
		control->record_size = sizeof(struct pib_ucontext_record);
		break;
	}

	case PIB_DEBUGFS_PD: {
		struct pib_pd *pd;
		struct pib_pd_record *records;

		control = vzalloc(sizeof(struct pib_record_control) +
				  dev->nr_pd * sizeof(struct pib_pd_record));
		if (!control)
			return -ENOMEM;

		records  = (struct pib_pd_record *)control->records;

		i=0;
		spin_lock_irqsave(&dev->lock, flags);
		list_for_each_entry(pd, &dev->pd_head, list) {
			records[i].base.obj_num       = pd->pd_num;
			records[i].base.creation_time = pd->creation_time;
			i++;
		}
		spin_unlock_irqrestore(&dev->lock, flags);
		
		control->count = i;
		control->record_size = sizeof(struct pib_pd_record);
		break;
	}

	case PIB_DEBUGFS_MR: {
		struct pib_mr *mr;
		struct pib_mr_record *records;

		control = vzalloc(sizeof(struct pib_record_control) +
				  dev->nr_ucontext * sizeof(struct pib_mr_record));
		if (!control)
			return -ENOMEM;

		records  = (struct pib_mr_record *)control->records;

		i=0;
		spin_lock_irqsave(&dev->lock, flags);
		list_for_each_entry(mr, &dev->mr_head, list) {
			records[i].base.obj_num       = mr->mr_num;
			records[i].base.creation_time = mr->creation_time;
			records[i].pd_num	      = to_ppd(mr->ib_mr.pd)->pd_num,
			records[i].is_dma             = mr->is_dma;
			records[i].access_flags       = mr->access_flags;
			records[i].start	      = mr->start;
			records[i].length             = mr->length;
			records[i].lkey	 	      = mr->ib_mr.lkey;
			records[i].rkey     	      = mr->ib_mr.rkey;
			i++;
		}
		spin_unlock_irqrestore(&dev->lock, flags);
		
		control->count = i;
		control->record_size = sizeof(struct pib_mr_record);
		break;
	}

	case PIB_DEBUGFS_SRQ: {
		struct pib_srq *srq;
		struct pib_srq_record *records;

		control = vzalloc(sizeof(struct pib_record_control) +
				  dev->nr_ucontext * sizeof(struct pib_srq_record));
		if (!control)
			return -ENOMEM;

		records  = (struct pib_srq_record *)control->records;

		i=0;
		spin_lock_irqsave(&dev->lock, flags);
		list_for_each_entry(srq, &dev->srq_head, list) {
			records[i].base.obj_num       = srq->srq_num;
			records[i].base.creation_time = srq->creation_time;
			records[i].pd_num	      = to_ppd(srq->ib_srq.pd)->pd_num;
			records[i].state              = srq->state;
			records[i].max_wqe            = srq->ib_srq_attr.max_wr;
			records[i].nr_wqe             = srq->ib_srq_attr.max_wr - srq->nr_recv_wqe;
			i++;
		}
		spin_unlock_irqrestore(&dev->lock, flags);
		
		control->count = i;
		control->record_size = sizeof(struct pib_srq_record);
		break;
	}

	case PIB_DEBUGFS_AH: {
		struct pib_ah *ah;
		struct pib_ah_record *records;

		control = vzalloc(sizeof(struct pib_record_control) +
				  dev->nr_ucontext * sizeof(struct pib_ah_record));
		if (!control)
			return -ENOMEM;

		records  = (struct pib_ah_record *)control->records;

		i=0;
		spin_lock_irqsave(&dev->lock, flags);
		list_for_each_entry(ah, &dev->ah_head, list) {
			records[i].base.obj_num       = ah->ah_num;
			records[i].base.creation_time = ah->creation_time;
			records[i].pd_num	      = to_ppd(ah->ib_ah.pd)->pd_num;
			records[i].dlid		      = ah->ib_ah_attr.dlid;
			records[i].ah_flags	      = ah->ib_ah_attr.ah_flags;
			records[i].port_num	      = ah->ib_ah_attr.port_num;
			i++;
		}
		spin_unlock_irqrestore(&dev->lock, flags);
		
		control->count = i;
		control->record_size = sizeof(struct pib_ah_record);
		break;
	}

	case PIB_DEBUGFS_CQ: {
		struct pib_cq *cq;
		struct pib_cq_record *records;

		control = vzalloc(sizeof(struct pib_record_control) +
				  dev->nr_ucontext * sizeof(struct pib_cq_record));
		if (!control)
			return -ENOMEM;

		records  = (struct pib_cq_record *)control->records;

		i=0;
		spin_lock_irqsave(&dev->lock, flags);
		list_for_each_entry(cq, &dev->cq_head, list) {
			records[i].base.obj_num       = cq->cq_num;
			records[i].base.creation_time = cq->creation_time;
			records[i].state              = cq->state;
			records[i].max_cqe            = cq->ib_cq.cqe;
			records[i].nr_cqe             = cq->nr_cqe;
			i++;
		}
		spin_unlock_irqrestore(&dev->lock, flags);
		
		control->count = i;
		control->record_size = sizeof(struct pib_cq_record);
		break;
	}

	case PIB_DEBUGFS_QP: {
		struct pib_qp *qp;
		struct pib_qp_record *records;

		control = vzalloc(sizeof(struct pib_record_control) +
				  dev->nr_ucontext * sizeof(struct pib_qp_record));
		if (!control)
			return -ENOMEM;

		records  = (struct pib_qp_record *)control->records;

		i=0;
		spin_lock_irqsave(&dev->lock, flags);
		list_for_each_entry(qp, &dev->qp_head, list) {
			records[i].base.obj_num       = qp->ib_qp.qp_num;
			records[i].base.creation_time = qp->creation_time;
			records[i].pd_num	      = to_ppd(qp->ib_qp.pd)->pd_num;
			records[i].send_cq_num	      = qp->send_cq->cq_num;
			if (qp->recv_cq)
				records[i].recv_cq_num= qp->recv_cq->cq_num;
			if (qp->ib_qp_init_attr.srq)
				records[i].srq_num    = to_psrq(qp->ib_qp_init_attr.srq)->srq_num;
			records[i].max_swqe	      = qp->ib_qp_init_attr.cap.max_send_wr;
			records[i].nr_swqe	      = qp->requester.nr_submitted_swqe +
				qp->requester.nr_sending_swqe + qp->requester.nr_waiting_swqe;
			records[i].max_rwqe	      = qp->ib_qp_init_attr.cap.max_recv_wr;
			records[i].nr_rwqe	      = qp->ib_qp_init_attr.cap.max_recv_wr - qp->responder.nr_recv_wqe;
			records[i].qp_type	      = qp->qp_type;
			records[i].state	      = qp->state;
			i++;
		}
		spin_unlock_irqrestore(&dev->lock, flags);
		
		control->count = i;
		control->record_size = sizeof(struct pib_qp_record);
		break;
	}

	default:
		BUG();
	}

	control->dev   = dev;
	control->type  = entry->type;

	ret = seq_open(file, &inspection_seq_ops);
	if (ret)
		goto err_seq_open;

	seq = file->private_data;
	seq->private = control;

	return 0;

err_seq_open:
	vfree(control);

	return ret;
}


static int inspection_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq;

	seq = file->private_data;
	vfree(seq->private);

	return seq_release(inode, file);
}


static const struct file_operations inspection_fops = {
	.owner   = THIS_MODULE,
	.open    = inspection_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = inspection_release,
};


/******************************************************************************/
/* Error Injection                                                            */
/******************************************************************************/

static int inject_err_open(struct inode *inode, struct file *file)
{
	if (!try_module_get(THIS_MODULE))
		return -EBUSY;

	nonseekable_open(inode, file);

	file->private_data = inode->i_private;

	return 0;
}


static ssize_t
inject_err_write(struct file *file, const char __user *buf,
		 size_t len, loff_t *ppos)
{
	int ret = 0;
	u32 oid = 0;
	enum ib_event_type type;
	struct pib_dev *dev;
	unsigned long flags;
	
	dev = file->private_data;

	if (*ppos != 0)
		return 0;

	if (strncasecmp(buf, "CQ", 2) == 0) {
		type = IB_EVENT_CQ_ERR;
		buf += 2;
	} else if (strncasecmp(buf, "QP", 2) == 0) {
		type = IB_EVENT_QP_FATAL;
		buf += 2;
	} else if (strncasecmp(buf, "SRQ", 3) == 0) {
		type = IB_EVENT_SRQ_ERR;
		buf += 3;
	} else
		return -EINVAL;

	if (sscanf(buf, "%x", &oid) != 1)
		return -EINVAL;

	spin_lock_irqsave(&dev->lock, flags);
	if (list_empty(&dev->debugfs.inject_err_work.entry)) {
		dev->debugfs.inject_err_type = type;
		dev->debugfs.inject_err_oid  = oid;
		pib_queue_work(dev, &dev->debugfs.inject_err_work);
	} else {
		ret = -EBUSY;
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	*ppos = len;

	return len;
}


static ssize_t inject_err_read(struct file *file, char __user *buf,
			       size_t count, loff_t *ppos)
{
	int ret;

	if (*ppos != 0)
		return 0;

	ret = snprintf(buf, count, "[CQ|QP|SRQ|] OID\n");

	*ppos = ret;

	return ret;
}


static int inject_err_release(struct inode *inode, struct file *file)
{
	module_put(THIS_MODULE);
	return 0;
}


static const struct file_operations inject_err_fops = {
	.owner   = THIS_MODULE,
	.open    = inject_err_open,
	.read    = inject_err_read,
	.write   = inject_err_write,
	.llseek  = no_llseek,
	.release = inject_err_release,
};


void pib_inject_err_handler(struct pib_work_struct *work)
{
	struct pib_dev *dev = work->data;
	u32 oid;

	oid = dev->debugfs.inject_err_oid;

	BUG_ON(!spin_is_locked(&dev->lock));

	switch (dev->debugfs.inject_err_type) {

	case IB_EVENT_CQ_ERR: {
		struct pib_cq *cq;
		list_for_each_entry(cq, &dev->cq_head, list) {
			if (cq->cq_num == oid) {
				/* cq をロックしない */
				pib_util_insert_async_cq_error(dev, cq);
				break;
			}
		}
		break;
	}

	case IB_EVENT_QP_FATAL: {
		struct pib_qp *qp;
		list_for_each_entry(qp, &dev->qp_head, list) {
			if (qp->ib_qp.qp_num == oid) {
				spin_lock(&qp->lock);
				qp->state = IB_QPS_ERR;
				pib_util_flush_qp(qp, 0);
				pib_util_insert_async_qp_error(qp, IB_EVENT_QP_FATAL);
				spin_unlock(&qp->lock);
				break;
			}
		}
		break;
	}

	case IB_EVENT_SRQ_ERR: {
		struct pib_srq *srq;
		list_for_each_entry(srq, &dev->srq_head, list) {
			if (srq->srq_num == oid) {
				/* srq をロックしない */
				pib_util_insert_async_srq_error(dev, srq);
				break;
			}
		}
		break;
	}

	default:
		BUG();
	}
}


/******************************************************************************/
/* Execution trace                                                            */
/******************************************************************************/

enum pib_trace_act {
	PIB_TRACE_ACT_NONE,
	PIB_TRACE_ACT_API,
	PIB_TRACE_ACT_SEND,
	PIB_TRACE_ACT_RECV1,
	PIB_TRACE_ACT_RECV2,
	PIB_TRACE_ACT_ASYNC,
	PIB_TRACE_ACT_TIMEDATE
};


struct pib_trace_entry {
	u8	act;
	u8	op;
	u8	port;

	union {
		struct {
			u32	oid;
		} api;

		struct {
			u16	len;
			u16	slid;
			u16	dlid;
			u32	sqpn;
			u32	dqpn;
			u32	psn;
		} send;

		struct {
			u16	len;
			u16	slid;
			u16	dlid;
			u32	dqpn;
			u32	psn;
		} recv1;

		struct {
			u32	sqpn;
			u32	psn;
			u32	data;
		} recv2;

		struct {
			u32	oid;
		} async;

		struct {
			struct timespec	time;
		} timedate;
	} u;

	cycles_t	timestamp;
};


struct pib_trace_info {
	struct pib_trace_entry *entry;
	u32		start;
	u32		index;
	struct timespec	base_timespec;
	cycles_t	base_timestamp;
	double		rate;
};


static const char *str_act[] = {
	[PIB_TRACE_ACT_API]     = "API ",
	[PIB_TRACE_ACT_SEND]    = "SEND",
	[PIB_TRACE_ACT_RECV1]   = "RCV1",
	[PIB_TRACE_ACT_RECV2]   = "RCV2",
	[PIB_TRACE_ACT_ASYNC]   = "ASYC",
	[PIB_TRACE_ACT_TIMEDATE]= "TIME",
};


static void *trace_seq_start(struct seq_file *file, loff_t *ppos)
{
	struct pib_dev *dev = file->private;
	struct pib_trace_info *info;
	struct pib_trace_entry *entry;
	struct timespec	now_timespec;
	cycles_t	now_timestamp;
	u64             duration_ns;

	if (!dev->debugfs.trace_data)
		return NULL;

	info = kzalloc(sizeof(struct pib_trace_info), GFP_KERNEL);
	if (!info)
		return NULL;

	getnstimeofday(&now_timespec);
	now_timestamp = get_cycles();

	info->entry = dev->debugfs.trace_data;
	info->start = atomic_read(&dev->debugfs.trace_index) % PIB_TRACE_MAX_ENTRIES;

retry:
	if ((*ppos < 0) || (PIB_TRACE_MAX_ENTRIES <= *ppos))
		goto error;

	info->index = *ppos;

	entry = &info->entry[(info->start + info->index) % PIB_TRACE_MAX_ENTRIES];
	
	if (entry->act != PIB_TRACE_ACT_TIMEDATE) {
		++*ppos;
		goto retry;
	}

	info->base_timespec  = entry->u.timedate.time;
	info->base_timestamp = entry->timestamp;

	duration_ns = (now_timespec.tv_sec - info->base_timespec.tv_sec) * 1000000000
		+ (now_timespec.tv_nsec - info->base_timespec.tv_nsec);

	info->rate = 1.0 * duration_ns / (now_timestamp - info->base_timestamp);

	return info;

error:
	kfree(info);

	return NULL;
}


static void *trace_seq_next(struct seq_file *file, void *iter_ptr,
			    loff_t *ppos)
{
	struct pib_trace_info *info = iter_ptr;
	struct pib_trace_entry *entry;

	++*ppos;

	if ((*ppos < 0) || (PIB_TRACE_MAX_ENTRIES <= *ppos))
		return NULL;

	info->index = *ppos;

	entry = &info->entry[(info->start + info->index) % PIB_TRACE_MAX_ENTRIES];

	if (entry->act == PIB_TRACE_ACT_NONE)
		return NULL;
	else if (entry->act == PIB_TRACE_ACT_TIMEDATE) {
		info->base_timespec  = entry->u.timedate.time;
		info->base_timestamp = entry->timestamp;
	}

	return iter_ptr;
}


static void trace_seq_stop(struct seq_file *file, void *iter_ptr)
{
	/* nothing for now */
	kfree(iter_ptr);
}


static int trace_seq_show(struct seq_file *file, void *iter_ptr)
{
	struct pib_trace_info *info = iter_ptr;
	struct pib_trace_entry *entry;
	struct timespec	timespec;
	char buffer[32];

	timespec = info->base_timespec;
	
	entry = &info->entry[(info->start + info->index) % PIB_TRACE_MAX_ENTRIES];

	timespec.tv_nsec += (entry->timestamp - info->base_timestamp) * info->rate;

	if (timespec.tv_nsec >= 1000000000) {
		timespec.tv_nsec -= 1000000000;
		timespec.tv_sec++;
	}

	show_timespec(file, &timespec);

	seq_printf(file, " %s ", str_act[entry->act]);

	switch (entry->act) {
	case PIB_TRACE_ACT_API:
		switch (entry->op) {
		case IB_USER_VERBS_CMD_CREATE_AH:
		case IB_USER_VERBS_CMD_MODIFY_AH:
		case IB_USER_VERBS_CMD_QUERY_AH:
		case IB_USER_VERBS_CMD_DESTROY_AH:
		case IB_USER_VERBS_CMD_CREATE_QP:
		case IB_USER_VERBS_CMD_QUERY_QP:
		case IB_USER_VERBS_CMD_MODIFY_QP:
		case IB_USER_VERBS_CMD_DESTROY_QP:
		case IB_USER_VERBS_CMD_POST_SEND:
		case IB_USER_VERBS_CMD_POST_RECV:
		case IB_USER_VERBS_CMD_ATTACH_MCAST:
		case IB_USER_VERBS_CMD_DETACH_MCAST:
			snprintf(buffer, sizeof(buffer), "%s", pib_get_uverbs_cmd(entry->op));
			seq_printf(file, "%-18s OID:%06x\n", buffer, entry->u.api.oid);
			break;

		default:
			snprintf(buffer, sizeof(buffer), "%s", pib_get_uverbs_cmd(entry->op));
			seq_printf(file, "%-18s OID:%04x\n", buffer, entry->u.api.oid);
			break;
		}
		break;

	case PIB_TRACE_ACT_SEND:
		if (pib_get_trans_op(entry->op))
			snprintf(buffer, sizeof(buffer), "%s/%s",
				 pib_get_service_type(entry->op), pib_get_trans_op(entry->op));
		else
			snprintf(buffer, sizeof(buffer), "UNKNOWN(%u)", entry->op);

		seq_printf(file, "%-18s PORT:%u PSN:%06x LEN:%04u SLID:%04x SQPN:%06x DLID:%04x DQPN:%06x\n",
			   buffer,
			   entry->port, entry->u.send.psn, entry->u.send.len,
			   entry->u.send.slid, entry->u.send.sqpn,
			   entry->u.send.dlid, entry->u.send.dqpn);
		break;

	case PIB_TRACE_ACT_RECV1:
		if (pib_get_trans_op(entry->op))
			snprintf(buffer, sizeof(buffer), "%s/%s",
				 pib_get_service_type(entry->op), pib_get_trans_op(entry->op));
		else
			snprintf(buffer, sizeof(buffer), "UNKNOWN(%u)", entry->op);

		seq_printf(file, "%-18s PORT:%u PSN:%06x LEN:%04u SLID:%04x DLID:%04x DQPN:%06x\n",
			   buffer,
			   entry->port, entry->u.recv1.psn, entry->u.recv1.len,
			   entry->u.recv1.slid, entry->u.recv1.dlid, entry->u.recv1.dqpn);
		break;

	case PIB_TRACE_ACT_RECV2:
		if (pib_get_trans_op(entry->op))
			snprintf(buffer, sizeof(buffer), "%s/%s",
				 pib_get_service_type(entry->op), pib_get_trans_op(entry->op));
		else
			snprintf(buffer, sizeof(buffer), "UNKNOWN(%u)", entry->op);

		seq_printf(file, "%-18s PORT:%u PSN:%06x DATA:%04u SQPN:%06x\n",
			   buffer,
			   entry->port, entry->u.recv2.psn, entry->u.recv2.data, entry->u.recv2.sqpn);
		break
;
	case PIB_TRACE_ACT_ASYNC:
		snprintf(buffer, sizeof(buffer), "%s", pib_get_async_event(entry->op));
		seq_printf(file, "%-18s OID:%06x\n", buffer, entry->u.async.oid);
		break;

	case PIB_TRACE_ACT_TIMEDATE:
		seq_puts(file, "\n");
		break;

	default:
		BUG();
	}

	return 0;
}


static const struct seq_operations trace_seq_ops = {
	.start = trace_seq_start,
	.next  = trace_seq_next,
	.stop  = trace_seq_stop,
	.show  = trace_seq_show,
};


static int trace_open(struct inode *inode, struct file *file)
{
	struct seq_file *seq;
	int ret;

	ret = seq_open(file, &trace_seq_ops);
	if (ret)
		return ret;

	seq = file->private_data;
	seq->private = inode->i_private;

	return 0;
}


static const struct file_operations trace_fops = {
	.owner   = THIS_MODULE,
	.open    = trace_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release,
};


static struct pib_trace_entry *alloc_new_trace(struct pib_dev *dev)
{
	int index;
	struct pib_trace_entry *entry;

	if (!dev->debugfs.trace_data)
		return NULL;

	dev->debugfs.count_records--;

	if ((dev->debugfs.count_records < 0) || time_after(jiffies, dev->debugfs.last_record_time + HZ)) {

		index = atomic_add_return(1, &dev->debugfs.trace_index);
		index = (index - 1) % PIB_TRACE_MAX_ENTRIES;

		entry = (struct pib_trace_entry *)dev->debugfs.trace_data + index;

		entry->act = PIB_TRACE_ACT_NONE;
	
		barrier();

		getnstimeofday(&entry->u.timedate.time);
		entry->timestamp = get_cycles();

		barrier();

		entry->act = PIB_TRACE_ACT_TIMEDATE;

		dev->debugfs.count_records    = 100;
		dev->debugfs.last_record_time = jiffies;
	}

	index = atomic_add_return(1, &dev->debugfs.trace_index);
	index = (index - 1) % PIB_TRACE_MAX_ENTRIES;

	entry = (struct pib_trace_entry *)dev->debugfs.trace_data + index;

	entry->act = PIB_TRACE_ACT_NONE;

	barrier();

	entry->timestamp = get_cycles();

	return entry;
}


void pib_trace_api(struct pib_dev *dev, int cmd, u32 oid)
{
	struct pib_trace_entry *entry;

	entry = alloc_new_trace(dev);

	if (!entry)
		return;

	entry->op        = cmd;
	entry->u.api.oid = oid;

	barrier();

	entry->act = PIB_TRACE_ACT_API;
}


void pib_trace_send(struct pib_dev *dev, u8 port_num, int size)
{
	struct pib_trace_entry *entry;
	void *buffer;
	struct pib_packet_lrh *lrh;
	struct pib_packet_bth *bth;

	entry = alloc_new_trace(dev);

	if (!entry)
		return;
	
	entry->port = port_num;

	entry->u.send.len = size;
	entry->u.send.slid = dev->thread.slid;
	entry->u.send.dlid = dev->thread.dlid;
	entry->u.send.sqpn = dev->thread.src_qp_num;

	buffer = dev->thread.buffer;
	
	lrh = buffer;
	buffer += sizeof(*lrh);

	if ((lrh->sl_rsv_lnh & 0x3) == 0x3)
		buffer += sizeof(struct ib_grh);

	bth = buffer;

	entry->u.send.dqpn = be32_to_cpu(bth->destQP);
	entry->u.send.psn  = be32_to_cpu(bth->psn) & PIB_PSN_MASK;
	entry->op   = bth->OpCode;

	barrier();

	entry->act  = PIB_TRACE_ACT_SEND;
}


void pib_trace_recv(struct pib_dev *dev, u8 port_num, u8 opcode, u32 psn, int size, u16 slid, u16 dlid, u32 dqpn)
{
	struct pib_trace_entry *entry;

	entry = alloc_new_trace(dev);

	if (!entry)
		return;

	entry->op   = opcode;
	entry->port = port_num;
	entry->u.recv1.len = size;
	entry->u.recv1.slid = slid;
	entry->u.recv1.dlid = dlid;
	entry->u.recv1.dqpn = dqpn;
	entry->u.recv1.psn  = psn;

	barrier();

	entry->act  = PIB_TRACE_ACT_RECV1;
}


void pib_trace_recv_ok(struct pib_dev *dev, u8 port_num, u8 opcode, u32 psn, u32 sqpn, u32 data)
{
	struct pib_trace_entry *entry;

	entry = alloc_new_trace(dev);

	if (!entry)
		return;

	entry->op   = opcode;
	entry->port = port_num;
	entry->u.recv2.data = data;
	entry->u.recv2.sqpn = sqpn;
	entry->u.recv2.psn  = psn;

	barrier();

	entry->act  = PIB_TRACE_ACT_RECV2;
}


void pib_trace_async(struct pib_dev *dev, enum ib_event_type type, u32 oid)
{
	struct pib_trace_entry *entry;

	entry = alloc_new_trace(dev);

	if (!entry)
		return;

	entry->op           = type;
	entry->u.async.oid  = oid;

	barrier();

	entry->act = PIB_TRACE_ACT_ASYNC;
}


/******************************************************************************/
/* Driver looad/unload                                                        */
/******************************************************************************/

int pib_register_debugfs(void)
{
	int i, j;

	debugfs_root = debugfs_create_dir("pib", NULL);
	if (!debugfs_root) {
		pr_err("pib: failed to create debugfs \"pib/\"\n");
		return -ENOMEM;
	}

	for (i=0 ; i<pib_phys_port_cnt ; i++)
		if (register_dev(debugfs_root, pib_devs[i]))
			goto err_register_dev;

	return 0;

err_register_dev:
	for (j=0 ; j<i ; j++)
		unregister_dev(pib_devs[j]);

	debugfs_remove(debugfs_root);
	debugfs_root = NULL;

	return -ENOMEM;
}


void pib_unregister_debugfs(void)
{
	int i;

	if (!debugfs_root)
		return;

	for (i=0 ; i<pib_phys_port_cnt ; i++) {
		BUG_ON(!pib_devs[i]);
		unregister_dev(pib_devs[i]);
	}

	debugfs_remove(debugfs_root);
	debugfs_root = NULL;
}


static int register_dev(struct dentry *root, struct pib_dev *dev)
{
	int i;

	init_timestamp = get_cycles();

	dev->debugfs.dir = debugfs_create_dir(dev->ib_dev.name, root);
	if (!dev->debugfs.dir) {
		pr_err("pib: failed to create debugfs \"pib/%s/\"\n", dev->ib_dev.name);
		goto err;
	}

	/* Error injection */
	dev->debugfs.inject_err = debugfs_create_file("inject_err", S_IFREG | S_IRWXUGO,
						      dev->debugfs.dir,
						      dev,
						      &inject_err_fops);
	if (!dev->debugfs.inject_err) {
		pr_err("pib: failed to create debugfs \"pib/%s/inject_err\"\n", dev->ib_dev.name);
		goto err;
	}

	/* Execution trace */
	dev->debugfs.trace = debugfs_create_file("trace", S_IFREG | S_IRUGO,
						      dev->debugfs.dir,
						      dev,
						      &trace_fops);
	if (!dev->debugfs.trace) {
		pr_err("pib: failed to create debugfs \"pib/%s/trace\"\n", dev->ib_dev.name);
		goto err;
	}

	dev->debugfs.trace_data = vzalloc(sizeof(struct pib_trace_entry) * PIB_TRACE_MAX_ENTRIES);
	if (!dev->debugfs.trace_data) {
		pr_err("pib: failed to allocate memory of debugfs \"pib/%s/trace\"\n", dev->ib_dev.name);
		goto err;
	}

	/* Object inspection */
	for (i=0 ; i<PIB_DEBUGFS_LAST ; i++) {
		struct dentry *dentry;
		dentry = debugfs_create_file(debug_file_symbols[i], S_IFREG | S_IRUGO,
					     dev->debugfs.dir,
					     &dev->debugfs.entries[i],
					     &inspection_fops);
		if (!dentry) {
			pr_err("pib: failed to create debugfs \"pib/%s/%s\"\n",
			       dev->ib_dev.name, debug_file_symbols[i]);
			goto err;
		}

		dev->debugfs.entries[i].dev    = dev;
		dev->debugfs.entries[i].dentry = dentry;
		dev->debugfs.entries[i].type   = i;
	}

	return 0;

err:
	unregister_dev(dev);

	return -ENOMEM;
}


static void unregister_dev(struct pib_dev *dev)
{
	int i;

	for (i=PIB_DEBUGFS_LAST-1 ; 0 <= i ; i--) {
		struct dentry *dentry;
		dentry = dev->debugfs.entries[i].dentry;
		dev->debugfs.entries[i].dentry = NULL;
		if (dentry)
			debugfs_remove(dentry);
	}

	if (dev->debugfs.inject_err) {
		debugfs_remove(dev->debugfs.inject_err);
		dev->debugfs.inject_err = NULL;
	}

	if (dev->debugfs.trace_data) {
		vfree(dev->debugfs.trace_data);
		dev->debugfs.trace_data = NULL;
	}

	if (dev->debugfs.trace) {
		debugfs_remove(dev->debugfs.trace);
		dev->debugfs.trace = NULL;
	}

	if (dev->debugfs.dir) {
		debugfs_remove(dev->debugfs.dir);
		dev->debugfs.dir = NULL;
	}
}
