/*
 * Copyright (c) 2012-2017 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

/******************************************************************************
* wlan_nlink_srv.c
*
* This file contains the definitions specific to the wlan_nlink_srv
*
******************************************************************************/

/*
 * If MULTI_IF_NAME is not defined, then this is the primary instance of the
 * driver and the diagnostics netlink socket will be available. If
 * MULTI_IF_NAME is defined then this is not the primary instance of the driver
 * and the diagnotics netlink socket will not be available since this
 * diagnostics netlink socket can only be exposed by one instance of the driver.
 */
#ifndef MULTI_IF_NAME

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <wlan_nlink_srv.h>
#include <vos_trace.h>

#ifdef CNSS_GENL
#include <vos_memory.h>
#include <wlan_nlink_common.h>
#include <net/genetlink.h>
#include <net/cnss_nl.h>
#endif

/* Global variables */
static DEFINE_MUTEX(nl_srv_sem);
static struct sock *nl_srv_sock;
static nl_srv_msg_callback nl_srv_msg_handler[NLINK_MAX_CALLBACKS];

/* Forward declaration */
static void nl_srv_rcv (struct sk_buff *sk);
static void nl_srv_rcv_skb (struct sk_buff *skb);
static void nl_srv_rcv_msg (struct sk_buff *skb, struct nlmsghdr *nlh);

/*
 * Initialize the netlink service.
 * Netlink service is usable after this.
 */
int nl_srv_init(void)
{
   int retcode = 0;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,6,0))
   struct netlink_kernel_cfg cfg = {
      .groups = WLAN_NLINK_MCAST_GRP_ID,
      .input = nl_srv_rcv
   };
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,6,0))
   nl_srv_sock = netlink_kernel_create(&init_net, WLAN_NLINK_PROTO_FAMILY,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,7,0))
                                       THIS_MODULE,
#endif
                                       &cfg);
#else
   nl_srv_sock = netlink_kernel_create(&init_net, WLAN_NLINK_PROTO_FAMILY,
      WLAN_NLINK_MCAST_GRP_ID, nl_srv_rcv, NULL, THIS_MODULE);
#endif

   if (nl_srv_sock != NULL) {
      memset(nl_srv_msg_handler, 0, sizeof(nl_srv_msg_handler));
   } else {
      VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
         "NLINK: netlink_kernel_create failed");
      retcode = -ECONNREFUSED;
   }
   return retcode;
}

/*
 * Deinit the netlink service.
 * Netlink service is unusable after this.
 */
#ifdef WLAN_KD_READY_NOTIFIER
void nl_srv_exit(int dst_pid)
#else
void nl_srv_exit(void)
#endif /* WLAN_KD_READY_NOTIFIER */
{
   netlink_kernel_release(nl_srv_sock);
   nl_srv_sock = NULL;
}

/*
 * Register a message handler for a specified module.
 * Each module (e.g. WLAN_NL_MSG_BTC )will register a
 * handler to handle messages addressed to it.
 */
int nl_srv_register(tWlanNlModTypes msg_type, nl_srv_msg_callback msg_handler)
{
   int retcode = 0;

   if ((msg_type >= WLAN_NL_MSG_BASE) && (msg_type < WLAN_NL_MSG_MAX) &&
        msg_handler != NULL)
   {
      nl_srv_msg_handler[msg_type - WLAN_NL_MSG_BASE] = msg_handler;
   }
   else {
      VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_WARN,
         "NLINK: nl_srv_register failed for msg_type %d", msg_type);
      retcode = -EINVAL;
   }

   return retcode;
}
/*
 * Unregister the message handler for a specified module.
 */
int nl_srv_unregister(tWlanNlModTypes msg_type, nl_srv_msg_callback msg_handler)
{
   int retcode = 0;

   if ((msg_type >= WLAN_NL_MSG_BASE) && (msg_type < WLAN_NL_MSG_MAX) &&
       (nl_srv_msg_handler[msg_type - WLAN_NL_MSG_BASE] == msg_handler))
   {
      nl_srv_msg_handler[msg_type - WLAN_NL_MSG_BASE] = NULL;
   }
   else
   {
      VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_WARN,
         "NLINK: nl_srv_unregister failed for msg_type %d", msg_type);
      retcode = -EINVAL;
   }

   return retcode;
}

#ifdef CNSS_GENL

/**
 * nl80211hdr_put() - API to fill genlmsg header
 * @skb: Sk buffer
 * @portid: Port ID
 * @seq: Sequence number
 * @flags: Flags
 * @cmd: Command id
 *
 * API to fill genl message header for brodcast events to user space
 *
 * Return: Pointer to user specific header/payload
 */
static inline void *nl80211hdr_put(struct sk_buff *skb, uint32_t portid,
					uint32_t seq, int flags, uint8_t cmd)
{
	struct genl_family *cld80211_fam = cld80211_get_genl_family();

	return genlmsg_put(skb, portid, seq, cld80211_fam, flags, cmd);
}

/**
 * cld80211_fill_data() - API to fill payload to nl message
 * @msg: Sk buffer
 * @portid: Port ID
 * @seq: Sequence number
 * @flags: Flags
 * @cmd: Command ID
 * @buf: data buffer/payload to be filled
 * @len: length of the payload ie. @buf
 *
 * API to fill the payload/data of the nl message to be sent
 *
 * Return: zero on success
 */
static int cld80211_fill_data(struct sk_buff *msg, uint32_t portid,
					uint32_t seq, int flags, uint8_t cmd,
					uint8_t *buf, int len)
{
	void *hdr;
	struct nlattr *nest;

	hdr = nl80211hdr_put(msg, portid, seq, flags, cmd);
	if (!hdr) {
		VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
						"nl80211 hdr put failed");
		return -EPERM;
	}

	nest = nla_nest_start(msg, CLD80211_ATTR_VENDOR_DATA);
	if (!nest) {
		VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
						"nla_nest_start failed");
		goto nla_put_failure;
	}

	if (nla_put(msg, CLD80211_ATTR_DATA, len, buf)) {
		VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
							"nla_put failed");
		goto nla_put_failure;
	}

	nla_nest_end(msg, nest);
	genlmsg_end(msg, hdr);

	return 0;
nla_put_failure:
	genlmsg_cancel(msg, hdr);
	return -EPERM;
}

/**
 * send_msg_to_cld80211() - API to send message to user space Application
 * @cld_mcgroup_id: Multicast group ID
 * @pid: Port ID
 * @app_id: Application ID
 * @buf: Data/payload buffer to be sent
 * @len: Length of the data ie. @buf
 *
 * API to send the nl message to user space application.
 *
 * Return: zero on success
 */
static int send_msg_to_cld80211(enum cld80211_multicast_groups cld_mcgroup_id,
				int pid, int app_id, uint8_t *buf, int len)
{
	struct sk_buff *msg;
	int status;
	int flags = GFP_KERNEL;
	int mcgroup_id;

	if (in_interrupt() || irqs_disabled() || in_atomic())
		flags = GFP_ATOMIC;

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, flags);
	if (!msg) {
		VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
						"nlmsg malloc fails");
		return -EPERM;
	}

	status = cld80211_fill_data(msg, pid, 0, 0, app_id, buf, len);
	if (status) {
		nlmsg_free(msg);
		return -EPERM;
	}
	mcgroup_id = cld80211_get_mcgrp_id(cld_mcgroup_id);
	if (mcgroup_id == -1) {
		nlmsg_free(msg);
		return -EINVAL;
	}
	genlmsg_multicast_netns(&init_net, msg, 0, mcgroup_id, flags);
	return 0;
}

/**
 * nl_srv_bcast() - wrapper function to do broadcast events to user space apps
 * @skb: the socket buffer to send
 * @mcgroup_id: multicast group id
 * @app_id: application id
 *
 * This function is common wrapper to send broadcast events to different
 * user space applications.
 *
 * return: none
 */
int nl_srv_bcast(struct sk_buff *skb, int mcgroup_id, int app_id)
{
	struct nlmsghdr *nlh = (struct nlmsghdr *)skb->data;
	void *msg = NLMSG_DATA(nlh);
	uint32_t msg_len = nlmsg_len(nlh);
	uint8_t *tempbuf;
	int status;

	tempbuf = (uint8_t *)vos_mem_malloc(msg_len);
	vos_mem_copy(tempbuf, msg, msg_len);
	status = send_msg_to_cld80211(mcgroup_id, 0, app_id, tempbuf, msg_len);
	if (status) {
		VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
			"send msg to cld80211 fails for app id %d", app_id);
		return -EPERM;
	}

	dev_kfree_skb(skb);
	vos_mem_free(tempbuf);
	return 0;
}

/**
 * nl_srv_ucast() - wrapper function to do unicast events to user space apps
 * @skb: the socket buffer to send
 * @dst_pid: destination process IF
 * @flag: flags
 * @app_id: application id
 * @mcgroup_id: Multicast group ID
 *
 * This function is common wrapper to send unicast events to different
 * user space applications. This internally used broadcast API with multicast
 * group mcgrp_id. This wrapper serves as a common API in both
 * new generic netlink infra and legacy implementation.
 *
 * return: zero on success, error code otherwise
 */
int nl_srv_ucast(struct sk_buff *skb, int dst_pid, int flag,
					int app_id, int mcgroup_id)
{
	struct nlmsghdr *nlh = (struct nlmsghdr *)skb->data;
	void *msg = NLMSG_DATA(nlh);
	uint32_t msg_len = nlmsg_len(nlh);
	uint8_t *tempbuf;
	int status;

	tempbuf = (uint8_t *)vos_mem_malloc(msg_len);
	vos_mem_copy(tempbuf, msg, msg_len);
	status = send_msg_to_cld80211(mcgroup_id, dst_pid, app_id,
					tempbuf, msg_len);
	if (status) {
		VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
			"send msg to cld80211 fails for app id %d", app_id);
		return -EPERM;
	}

	dev_kfree_skb(skb);
	vos_mem_free(tempbuf);
	return 0;
}

#else
/*
 * Unicast the message to the process in user space identfied
 * by the dst-pid
 */
int nl_srv_ucast(struct sk_buff *skb, int dst_pid, int flag)
{
   int err = 0;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,7,0))
   NETLINK_CB(skb).pid = 0; //sender's pid
#else
   NETLINK_CB(skb).portid = 0; //sender's pid
#endif
   NETLINK_CB(skb).dst_group = 0; //not multicast

   if (nl_srv_sock != NULL) {
       err = netlink_unicast(nl_srv_sock, skb, dst_pid, flag);
   } else {
       dev_kfree_skb(skb);
   }

   if (err < 0)
      VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_WARN,
      "NLINK: netlink_unicast to pid[%d] failed, ret[%d]", dst_pid, err);

   return err;
}

int nl_srv_bcast(struct sk_buff *skb)
{
   int err = 0;
   int flags = GFP_KERNEL;

   if (in_interrupt() || irqs_disabled() || in_atomic())
       flags = GFP_ATOMIC;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,7,0))
   NETLINK_CB(skb).pid = 0; //sender's pid
#else
   NETLINK_CB(skb).portid = 0; //sender's pid
#endif
   NETLINK_CB(skb).dst_group = WLAN_NLINK_MCAST_GRP_ID; //destination group

   if (nl_srv_sock != NULL) {
       err = netlink_broadcast(nl_srv_sock, skb, 0, WLAN_NLINK_MCAST_GRP_ID, flags);
   } else {
       dev_kfree_skb(skb);
   }
   if (err < 0)
   {
      VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_WARN,
         "NLINK: netlink_broadcast failed err = %d", err);
   }
   return err;
}

#endif

/*
 *  Processes the Netlink socket input queue.
 *  Dequeue skb's from the socket input queue and process
 *  all the netlink messages in that skb, before moving
 *  to the next skb.
 */
static void nl_srv_rcv (struct sk_buff *sk)
{
   mutex_lock(&nl_srv_sem);
   nl_srv_rcv_skb(sk);
   mutex_unlock(&nl_srv_sem);
}

/*
 * Each skb could contain multiple Netlink messages. Process all the
 * messages in one skb and discard malformed skb's silently.
 */
static void nl_srv_rcv_skb (struct sk_buff *skb)
{
   struct nlmsghdr * nlh;

   while (skb->len >= NLMSG_SPACE(0)) {
      u32 rlen;

      nlh = (struct nlmsghdr *)skb->data;

      if (nlh->nlmsg_len < sizeof(*nlh) || skb->len < nlh->nlmsg_len) {
         VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_WARN, "NLINK: Invalid "
            "Netlink message: skb[%p], len[%d], nlhdr[%p], nlmsg_len[%d]",
            skb, skb->len, nlh, nlh->nlmsg_len);
         return;
      }

      rlen = NLMSG_ALIGN(nlh->nlmsg_len);
      if (rlen > skb->len)
         rlen = skb->len;
      nl_srv_rcv_msg(skb, nlh);
      skb_pull(skb, rlen);
   }
}

/*
 * Process a netlink message.
 * Each netlink message will have a message of type tAniMsgHdr inside.
 */
static void nl_srv_rcv_msg (struct sk_buff *skb, struct nlmsghdr *nlh)
{
   int type;

   /* Only requests are handled by kernel now */
   if (!(nlh->nlmsg_flags & NLM_F_REQUEST)) {
      VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_WARN,
         "NLINK: Received Invalid NL Req type [%x]", nlh->nlmsg_flags);
      return;
   }

   type = nlh->nlmsg_type;

   /* Unknown message */
   if (type < WLAN_NL_MSG_BASE || type >= WLAN_NL_MSG_MAX) {
      VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_WARN,
         "NLINK: Received Invalid NL Msg type [%x]", type);
      return;
   }

   /*
   * All the messages must at least carry the tAniMsgHdr
   * Drop any message with invalid length
   */
   if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(tAniMsgHdr))) {
      VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_WARN,
         "NLINK: Received NL Msg with invalid len[%x]", nlh->nlmsg_len);
      return;
   }

   // turn type into dispatch table offset
   type -= WLAN_NL_MSG_BASE;

   // dispatch to handler
   if (nl_srv_msg_handler[type] != NULL) {
      (nl_srv_msg_handler[type])(skb);
   } else {
      VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_WARN,
         "NLINK: No handler for Netlink Msg [0x%X]", type);
   }
}

/**
 * nl_srv_is_initialized() - This function is used check if the netlink
 * service is initialized
 *
 * This function is used check if the netlink service is initialized
 *
 * Return: Return -EPERM if the service is not initialized
 *
 */
int nl_srv_is_initialized()
{
	if (nl_srv_sock)
		return 0;
	else
		return -EPERM;
}

#endif