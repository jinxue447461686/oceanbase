/**
 * (C) 2007-2010 Taobao Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Version: $Id$
 *
 * Authors:
 *   yanran <yanran.hfs@taobao.com>
 *     - some work details if you want
 */

#include "ob_ups_slave_mgr.h"
#include "common/ob_malloc.h"
#include "common/utility.h"
using namespace oceanbase::updateserver;

static const int64_t DEFAULT_NETWORK_TIMEOUT = 1000 * 1000;

ObUpsSlaveMgr::ObUpsSlaveMgr()
{
  is_initialized_ = false;
  n_slave_last_post_ = -1;
  slave_num_ = 0;
  rpc_stub_ = NULL;
  role_mgr_ = NULL;
}

ObUpsSlaveMgr::~ObUpsSlaveMgr()
{
  ServerNode* node = NULL;
  ObDLink* p = slave_head_.server_list_link.next();
  while (p != &slave_head_.server_list_link)
  {
    node = (ServerNode*)p;
    p = p->next();
    p->prev()->remove();
    ob_free(node);
  }
}

int ObUpsSlaveMgr::init(IObAsyncClientCallback* callback, ObUpsRoleMgr *role_mgr,
    ObCommonRpcStub *rpc_stub, int64_t log_sync_timeout)
{
  int err = OB_SUCCESS;
  if (NULL == role_mgr || NULL == rpc_stub)
  {
    err = OB_INVALID_ARGUMENT;
    TBSYS_LOG(WARN, "invalid argument, role_mgr_=%p, rpc_stub=%p, log_sync_timeout=%ld", role_mgr, rpc_stub, log_sync_timeout);
  }
  else if (OB_SUCCESS != (err = ack_queue_.init(callback, rpc_stub->get_client_mgr(), DEFAULT_ACK_QUEUE_LEN)))
  {
    TBSYS_LOG(ERROR, "ack_queue.init(callback=%p, client_mgr=%p, queue_len=%ld)=>%d",
              callback, rpc_stub->get_client_mgr(), DEFAULT_ACK_QUEUE_LEN, err);
  }
  else
  {
    log_sync_timeout_ = log_sync_timeout;
    role_mgr_ = role_mgr;
    rpc_stub_ = rpc_stub;
    is_initialized_ = true;
  }
  return err;
}

int ObUpsSlaveMgr::set_log_sync_timeout_us(const int64_t timeout)
{
  return ObSlaveMgr::set_log_sync_timeout_us(timeout);
}

int ObUpsSlaveMgr::send_data(const char* data, const int64_t length)
{
  int ret = OB_NOT_SUPPORTED;
  UNUSED(data);
  UNUSED(length);
  return ret;
}

int ObUpsSlaveMgr::get_slaves(ObServer* slaves, int64_t limit, int64_t& slave_count)
{
  int ret = OB_SUCCESS;
  slave_count = 0;
  slave_info_mutex_.lock();
  ServerNode* slave_node = NULL;
  ObDLink* p = slave_head_.server_list_link.next();
  while (OB_SUCCESS == ret && p != NULL && p != &slave_head_.server_list_link)
  {
    if (slave_count >= limit)
    {
      TBSYS_LOG(ERROR, "too many slaves[%ld], limit=%ld", slave_count, limit);
      break;
    }
    slave_node = (ServerNode*)(p);
    slaves[slave_count++] = slave_node->server;
    p = p->next();
  }

  if (NULL == p)
  {
    TBSYS_LOG(ERROR, "Server list encounter NULL pointer, this should not be reached");
    ret = OB_ERROR;
  }
  slave_info_mutex_.unlock();
  return ret;
}

int ObUpsSlaveMgr::post_log_to_slave(const ObLogCursor& start_cursor, const ObLogCursor& end_cursor, const char* data, const int64_t length)
{
  int ret = check_inner_stat();
  int send_err = OB_SUCCESS;
  ObServer slaves[MAX_SLAVE_NUM];
  int64_t slave_num = 0;
  ObDataBuffer send_buf;

  if (NULL == rpc_stub_)
  {
    ret = OB_NOT_INIT;
    TBSYS_LOG(ERROR, "rpc_stub == NULL");
  }
  else if (NULL == data || length < 0)
  {
    TBSYS_LOG(ERROR, "parameters are invalid[data=%p length=%ld]", data, length);
    ret = OB_INVALID_ARGUMENT;
  }
  else
  {
    send_buf.set_data(const_cast<char*>(data), length);
    send_buf.get_position() = length;
  }

  if (OB_SUCCESS != ret)
  {}
  else if (OB_SUCCESS != (ret = get_slaves(slaves, MAX_SLAVE_NUM, slave_num)))
  {
    TBSYS_LOG(ERROR, "get_slaves(limit=%ld)=>%d", MAX_SLAVE_NUM, ret);
  }
  else if (OB_SUCCESS != (send_err = ack_queue_.many_post(slaves, slave_num, start_cursor.log_id_, end_cursor.log_id_, OB_SEND_LOG, log_sync_timeout_, send_buf)))
  {
    TBSYS_LOG(ERROR, "post_to_servers(server_num=%ld, log=[%s,%s])=>%d", slave_num, to_cstring(start_cursor), to_cstring(end_cursor), send_err);
  }
  return ret;
}

int ObUpsSlaveMgr::wait_post_log_to_slave(const char* data, const int64_t length, int64_t& delay_us)
{
  int ret = OB_SUCCESS;
  UNUSED(data);
  UNUSED(length);
  UNUSED(delay_us);
  return ret;
}

int64_t ObUpsSlaveMgr::get_acked_clog_id() const
{
  return const_cast<ObAckQueue*>(&ack_queue_)->get_next_acked_seq();
}

int ObUpsSlaveMgr::grant_keep_alive()
{
  int err = OB_SUCCESS;
  ServerNode* slave_node = NULL;
  ObDLink *p = slave_head_.server_list_link.next();
  timeval time_val;
  gettimeofday(&time_val, NULL);
  int64_t cur_time_us = time_val.tv_sec * 1000 * 1000 + time_val.tv_usec;
  while(p != NULL && p != &slave_head_.server_list_link)
  {
    slave_node = (ServerNode*)(p);
    TBSYS_LOG(DEBUG, "send keep alive msg to slave[%s], time=%ld", slave_node->server.to_cstring(), cur_time_us);
    err = rpc_stub_->send_keep_alive(slave_node->server);
    if (OB_SUCCESS != err)
    {
      TBSYS_LOG(WARN, "fail to send keep alive msg to slave[%s], err = %d", slave_node->server.to_cstring(), err);
    }
    p = p->next();
  }
  return err;
}

int ObUpsSlaveMgr::add_server(const ObServer &server)
{
  return ObSlaveMgr::add_server(server);
}
int ObUpsSlaveMgr::delete_server(const ObServer &server)
{
  return ObSlaveMgr::delete_server(server);
}

int ObUpsSlaveMgr::reset_slave_list()
{
  return ObSlaveMgr::reset_slave_list();
}
int ObUpsSlaveMgr::get_num() const
{
  return ObSlaveMgr::get_num();
}
int ObUpsSlaveMgr::set_send_log_point(const ObServer &server, const uint64_t send_log_point)
{
  return ObSlaveMgr::set_send_log_point(server, send_log_point);
}
void ObUpsSlaveMgr::print(char *buf, const int64_t buf_len, int64_t& pos)
{
  return ObSlaveMgr::print(buf, buf_len, pos);
}
