// SPDX-License-Identifier: GPL-2.0-only
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif
#include "sgx545_abi.h"

int sgx545_session_close(struct sgx545_session *s,
	const struct sgx545_bridge_ops *ops, void *ctx)
{
	int ret;

	if (!s->services)
		return 0;
	ret = ops->disconnect(ctx);
	if (!ret)
		memset(s, 0, sizeof(*s));
	return ret;
}

int sgx545_bridge_execute(struct sgx545_session *s,
	const struct sgx545_bridge_ops *ops, void *ctx,
	const struct sgx545_bridge_request *r)
{
	union {
		struct sgx545_connect_out connect;
		struct sgx545_enumerate_out enumerate;
		struct sgx545_handle_out handle;
		struct sgx545_init_info_out init_info;
		struct sgx545_alloc_shared_out allocated;
		struct sgx545_map_info_out mapped;
	} out = {0};
	__u32 in[14] = {0}, cmd, token = 0, out_size;
	int ret, copied, rollback = 0, created = 0;

	if ((r->bridge_id & ~0xffU) != SGX545_COMMAND_BASE)
		return -EINVAL;
	cmd = r->bridge_id & 0xffU;
	switch (cmd) {
	case SGX545_CMD_CONNECT:
		out_size = sizeof(out.connect);
		if (r->in_size != 8 || r->services)
			return -EINVAL;
		break;
	case SGX545_CMD_ACQUIRE:
		out_size = sizeof(out.handle);
		if (r->in_size != 12)
			return -EINVAL;
		break;
	case SGX545_CMD_ALLOC_SHARED:
		out_size = sizeof(out.allocated);
		if (r->in_size != 12)
			return -EINVAL;
		break;
	case SGX545_CMD_FREE_SHARED:
		out_size = sizeof(__u32);
		if (r->in_size != 8 + sizeof(struct sgx545_client_mem32))
			return -EINVAL;
		break;
	case SGX545_CMD_MAP_INFO:
		out_size = sizeof(out.mapped);
		if (r->in_size != 8)
			return -EINVAL;
		break;
	case SGX545_CMD_INIT_INFO:
		out_size = sizeof(out.init_info);
		if (r->in_size != 8)
			return -EINVAL;
		break;
	case SGX545_CMD_INIT_DISCONNECT:
		out_size = sizeof(out.handle);
		if (r->in_size != 8)
			return -EINVAL;
		break;
	case SGX545_CMD_ENUMERATE:
	case SGX545_CMD_DISCONNECT:
	case SGX545_CMD_INIT_CONNECT:
		out_size = cmd == SGX545_CMD_ENUMERATE ? sizeof(out.enumerate) : sizeof(out.handle);
		if (r->in_size != 0 && r->in_size != 4)
			return -EINVAL;
		break;
	default:
		return -EOPNOTSUPP;
	}
	if (r->out_size != out_size)
		return -EINVAL;
	if (cmd != SGX545_CMD_CONNECT && (!s->services || r->services != s->services))
		return -ENOENT;
	if (r->in_size) {
		ret = ops->copy_in(ctx, in, r->param_in, r->in_size);
		if (ret)
			return ret;
	}
	if (in[0] || (cmd == SGX545_CMD_CONNECT && in[1]))
		return -EOPNOTSUPP;

	switch (cmd) {
	case SGX545_CMD_CONNECT:
		if (s->services) {
			out.connect.services_lo = s->services;
			ret = 0;
		} else {
			ret = ops->connect(ctx, &token);
			if (!ret && !token) {
				ops->disconnect(ctx);
				return -EIO;
			}
			if (!ret) {
				created = 1;
				out.connect.services_lo = token;
			}
		}
		if (ret < 0)
			return ret;
		out.connect.error = ret;
		break;
	case SGX545_CMD_ENUMERATE:
		ret = ops->enumerate(ctx, &out.enumerate);
		if (ret < 0)
			return ret;
		if (out.enumerate.count > SGX545_MAX_DEVICES)
			return -EOVERFLOW;
		out.enumerate.error = ret;
		break;
	case SGX545_CMD_ACQUIRE:
		if (in[2] != SGX545_DEVICE_TYPE)
			return -EOPNOTSUPP;
		if (s->device) {
			if (in[1] != s->device_index || in[2] != s->device_type)
				return -EOPNOTSUPP;
			out.handle.handle = s->device;
			ret = 0;
		} else {
			ret = ops->acquire(ctx, in[1], in[2], &token);
			if (!ret && !token) {
				ops->release_device(ctx);
				return -EIO;
			}
			if (!ret) {
				created = 1;
				out.handle.handle = token;
			}
		}
		if (ret < 0)
			return ret;
		out.handle.error = ret;
		break;
	case SGX545_CMD_DISCONNECT:
		ret = sgx545_session_close(s, ops, ctx);
		if (ret < 0)
			return ret;
		out.handle.error = ret;
		break;
	case SGX545_CMD_INIT_CONNECT:
		ret = ops->init_connect(ctx);
		if (ret < 0)
			return ret;
		created = !ret;
		out.handle.error = ret;
		break;
	case SGX545_CMD_INIT_DISCONNECT:
		/* Successful finalization would enable hardware. Abort is the only path. */
		if (in[1])
			return -EOPNOTSUPP;
		if (!s->initializer)
			return -EACCES;
		ret = ops->init_abort(ctx, 0);
		if (ret < 0)
			return ret;
		if (!ret)
			s->initializer = 0;
		out.handle.error = ret;
		break;
	case SGX545_CMD_INIT_INFO:
		if (!s->initializer)
			return -EACCES;
		if (!s->device || in[1] != s->device)
			return -ENOENT;
		ret = ops->init_info(ctx, &out.init_info);
		if (ret < 0)
			return ret;
		created = !ret && !s->init_info;
		out.init_info.error = ret;
		break;
	case SGX545_CMD_ALLOC_SHARED:
		if (!s->initializer)
			return -EACCES;
		if (in[1] != SGX545_BUFFER_WC)
			return -EOPNOTSUPP;
		if (!in[2] || in[2] > SGX545_BUFFER_MAX_BYTES)
			return -EINVAL;
		ret = ops->alloc_shared(ctx, in[1], in[2], &out.allocated);
		if (ret < 0)
			return ret;
		out.allocated.error = ret;
		created = !ret;
		break;
	case SGX545_CMD_FREE_SHARED:
		/* The legacy trailing client record contains no trusted pointers. */
		ret = ops->free_shared(ctx, in[1]);
		if (ret < 0)
			return ret;
		out.handle.error = ret;
		break;
	case SGX545_CMD_MAP_INFO:
		ret = ops->map_info(ctx, in[1], &out.mapped);
		if (ret < 0)
			return ret;
		out.mapped.error = ret;
		break;
	}
	copied = ops->copy_out(ctx, r->param_out, &out, out_size);
	if (copied && created) {
		if (cmd == SGX545_CMD_CONNECT)
			rollback = ops->disconnect(ctx);
		else if (cmd == SGX545_CMD_ACQUIRE)
			rollback = ops->release_device(ctx);
		else if (cmd == SGX545_CMD_INIT_CONNECT)
			rollback = ops->init_abort(ctx, 1);
		else if (cmd == SGX545_CMD_ALLOC_SHARED)
			rollback = ops->free_shared(ctx, out.allocated.kernel_mem);
		else
			rollback = ops->release_init_info(ctx);
	}
	/* Preserve ownership if rollback failed, so close can retry cleanup. */
	if (created && (!copied || rollback)) {
		if (cmd == SGX545_CMD_CONNECT)
			s->services = token;
		else if (cmd == SGX545_CMD_ACQUIRE) {
			s->device = token;
			s->device_index = in[1];
			s->device_type = in[2];
		} else if (cmd == SGX545_CMD_INIT_CONNECT)
			s->initializer = 1;
		else if (cmd == SGX545_CMD_INIT_INFO)
			s->init_info = 1;
	}
	return copied;
}
